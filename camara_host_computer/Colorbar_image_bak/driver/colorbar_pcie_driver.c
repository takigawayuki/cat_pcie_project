#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/swab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "colorbar_pcie_driver.h"

#define COLORBAR_DMA_PREFILL_PATTERN 0xA5
#define COLORBAR_STOP_TIMEOUT_MS 100
#define COLORBAR_BME_TIMEOUT_MS 100
#define COLORBAR_DONE_TIMEOUT_MS 1000

struct colorbar_dma_buffer {
	void *cpu_addr;
	dma_addr_t dma_addr;
};

struct colorbar_device {
	struct pci_dev *pdev;
	void __iomem *bar;
	resource_size_t bar_len;
	int bar_index;
	struct mutex lock;
	struct colorbar_dma_buffer bufs[COLORBAR_BUFFER_COUNT];
	bool bufs_allocated;
	bool ring_configured;
	bool started;
	bool frame_ready;
	size_t buffer_size;
	u32 frame_counter;
	u32 last_buffer_index;
	u32 active_buffer_index;
	u32 next_buffer_index;
};

static struct colorbar_device *g_cdev;
static dev_t g_devt;
static struct cdev g_chardev;
static struct class *g_class;
static struct device *g_device;

static int bar = COLORBAR_BAR_DEFAULT;
module_param(bar, int, 0444);
MODULE_PARM_DESC(bar, "FPGA BAR index for colorbar control registers");

static bool cmd_byteswap;
module_param(cmd_byteswap, bool, 0644);
MODULE_PARM_DESC(cmd_byteswap, "byteswap 32-bit ARM/LEN/START command values before BAR writes; default false for PCIE_DMA_single_5");

static bool addr_byteswap;
module_param(addr_byteswap, bool, 0644);
MODULE_PARM_DESC(addr_byteswap, "byteswap 32-bit DMA addresses before writing BAR+0x110; default false for PCIE_DMA_single_5");

static bool readback_byteswap;
module_param(readback_byteswap, bool, 0644);
MODULE_PARM_DESC(readback_byteswap, "byteswap 32-bit command/readback registers after BAR reads; default false for PCIE_DMA_single_5");

static bool allow_dma_start;
module_param(allow_dma_start, bool, 0644);
MODULE_PARM_DESC(allow_dma_start, "allow START after PCIE_DMA_single_5 four-buffer ARM/ADDR/LEN/status checks; default false");

static bool block_unsafe_dma;
module_param(block_unsafe_dma, bool, 0644);
MODULE_PARM_DESC(block_unsafe_dma, "emergency block switch; true refuses all START requests");

static uint dma_len_bytes = COLORBAR_FRAME_SIZE;
module_param(dma_len_bytes, uint, 0644);
MODULE_PARM_DESC(dma_len_bytes, "PCIE_DMA_single_5 requires exactly 4147200 valid frame bytes");

static uint frame_wait_ms = COLORBAR_DONE_TIMEOUT_MS;
module_param(frame_wait_ms, uint, 0644);
MODULE_PARM_DESC(frame_wait_ms, "timeout in ms while waiting for one PCIE_DMA_single_5 frame DONE");

static bool verify_readback = true;
module_param(verify_readback, bool, 0644);
MODULE_PARM_DESC(verify_readback, "read all four ADDR_ECHO registers and STATUS before START; default true");

static bool colorbar_dma_len_valid(void)
{
	return dma_len_bytes == COLORBAR_FRAME_SIZE;
}

static size_t colorbar_requested_buffer_size(void)
{
	if (!colorbar_dma_len_valid())
		return 0;

	return COLORBAR_BUFFER_SIZE;
}

static int colorbar_set_dma_mask(struct pci_dev *pdev)
{
	int ret;

	/* The current FPGA colorbar path emits MWR_32 TLPs, so keep DMA below 4GB. */
	ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "failed to set 32-bit DMA mask: %d\n", ret);
		return ret;
	}

	ret = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "failed to set 32-bit consistent DMA mask: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "using 32-bit DMA mask for colorbar MWR_32 path\n");
	return 0;
}

static void colorbar_free_buffers_locked(struct colorbar_device *cdev)
{
	size_t buffer_size = cdev->buffer_size;
	int i;

	if (!buffer_size)
		buffer_size = COLORBAR_BUFFER_SIZE;

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
		if (cdev->bufs[i].cpu_addr) {
			dma_free_coherent(&cdev->pdev->dev, buffer_size,
					  cdev->bufs[i].cpu_addr,
					  cdev->bufs[i].dma_addr);
			cdev->bufs[i].cpu_addr = NULL;
			cdev->bufs[i].dma_addr = 0;
		}
	}

	cdev->bufs_allocated = false;
	cdev->ring_configured = false;
	cdev->buffer_size = 0;
	cdev->started = false;
	cdev->frame_ready = false;
	cdev->frame_counter = 0;
	cdev->last_buffer_index = 0;
	cdev->active_buffer_index = 0;
	cdev->next_buffer_index = 0;
}

static int colorbar_alloc_buffers_locked(struct colorbar_device *cdev)
{
	size_t buffer_size = colorbar_requested_buffer_size();
	int i;

	if (!buffer_size) {
		dev_err(&cdev->pdev->dev,
			"refuse to allocate DMA buffers: PCIE_DMA_single_5 requires dma_len_bytes=%u, got %u\n",
			COLORBAR_FRAME_SIZE, dma_len_bytes);
		return -EINVAL;
	}

	if (cdev->bufs_allocated) {
		if (cdev->buffer_size == buffer_size)
			return 0;
		colorbar_free_buffers_locked(cdev);
	}

	cdev->buffer_size = buffer_size;

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
		cdev->bufs[i].cpu_addr = dma_alloc_coherent(&cdev->pdev->dev,
							 buffer_size,
							 &cdev->bufs[i].dma_addr,
							 GFP_KERNEL);
		if (!cdev->bufs[i].cpu_addr) {
			dev_err(&cdev->pdev->dev,
				"failed to allocate coherent DMA buffer%d size=%zu requested_len=%u; check CMA/free contiguous DMA memory\n",
				i, buffer_size, dma_len_bytes);
			goto fail;
		}

		if (upper_32_bits(cdev->bufs[i].dma_addr) ||
		    (lower_32_bits(cdev->bufs[i].dma_addr) & (COLORBAR_DMA_ALIGN_BYTES - 1)) ||
		    ((u64)lower_32_bits(cdev->bufs[i].dma_addr) + COLORBAR_FRAME_SIZE > BIT_ULL(32))) {
			dev_err(&cdev->pdev->dev,
				"invalid DMA address for PCIE_DMA_single_5: buffer %d dma=%pad size=%u align=%u\n",
				i, &cdev->bufs[i].dma_addr, COLORBAR_FRAME_SIZE,
				COLORBAR_DMA_ALIGN_BYTES);
			goto fail;
		}

		memset(cdev->bufs[i].cpu_addr, COLORBAR_DMA_PREFILL_PATTERN, buffer_size);
		dev_info(&cdev->pdev->dev,
			 "buffer%d cpu=%p dma=%pad size=%zu frame_len=%u guard=%u\n",
			 i, cdev->bufs[i].cpu_addr, &cdev->bufs[i].dma_addr,
			 buffer_size, COLORBAR_FRAME_SIZE, COLORBAR_DMA_GUARD_SIZE);
	}

	cdev->bufs_allocated = true;
	return 0;

fail:
	colorbar_free_buffers_locked(cdev);
	return -ENOMEM;
}

static void colorbar_iowrite32(struct colorbar_device *cdev, u32 value, u32 offset)
{
	iowrite32(value, cdev->bar + offset);
}

static u32 colorbar_ioread32(struct colorbar_device *cdev, u32 offset)
{
	return ioread32(cdev->bar + offset);
}

static u32 colorbar_read_cmd32(struct colorbar_device *cdev, u32 offset)
{
	u32 raw = colorbar_ioread32(cdev, offset);
	u32 value = readback_byteswap ? swab32(raw) : raw;

	dev_info(&cdev->pdev->dev,
		 "read DMA command: offset=0x%03x raw=0x%08x value=0x%08x readback_byteswap=%d\n",
		 offset, raw, value, readback_byteswap);
	return value;
}

static u32 colorbar_read_cmd32_quiet(struct colorbar_device *cdev, u32 offset)
{
	u32 raw = colorbar_ioread32(cdev, offset);

	return readback_byteswap ? swab32(raw) : raw;
}

static u32 colorbar_decode_status(u32 raw)
{
	u32 swapped = swab32(raw);

	if (COLORBAR_STATUS_VERSION(raw) == COLORBAR_DMA_STATUS_VERSION)
		return raw;
	if (COLORBAR_STATUS_VERSION(swapped) == COLORBAR_DMA_STATUS_VERSION)
		return swapped;

	return raw;
}

static u32 colorbar_read_status_locked(struct colorbar_device *cdev, const char *phase)
{
	u32 raw = colorbar_ioread32(cdev, COLORBAR_REG_DMA_STATUS);
	u32 status = colorbar_decode_status(raw);

	dev_info(&cdev->pdev->dev,
		 "read STATUS %s: raw=0x%08x status=0x%08x version=0x%02x next_buffer=%u rc_cfg_ep=%u host_start=%u stop_pending=%u fifo_underflow=%u fifo_overflow=%u len_error=%u addr_error=%u bus_master_seen=%u len_valid=%u all_addr_valid=%u start_seen=%u arm=%u stop_ack=%u idle=%u done=%u busy=%u\n",
		 phase, raw, status, COLORBAR_STATUS_VERSION(status),
		 COLORBAR_STATUS_NEXT_BUFFER(status),
		 !!(status & COLORBAR_STATUS_RC_CFG_EP),
		 !!(status & COLORBAR_STATUS_HOST_START),
		 !!(status & COLORBAR_STATUS_STOP_PENDING),
		 !!(status & COLORBAR_STATUS_FIFO_UNDERFLOW),
		 !!(status & COLORBAR_STATUS_FIFO_OVERFLOW),
		 !!(status & COLORBAR_STATUS_LEN_ERROR),
		 !!(status & COLORBAR_STATUS_ADDR_ERROR),
		 !!(status & COLORBAR_STATUS_PCIE_DMA_ENABLE),
		 !!(status & COLORBAR_STATUS_DMA_LEN_VALID),
		 !!(status & COLORBAR_STATUS_DMA_ADDR_VALID),
		 !!(status & COLORBAR_STATUS_START_CMD_SEEN),
		 !!(status & COLORBAR_STATUS_HOST_ARM),
		 !!(status & COLORBAR_STATUS_STOP_ACK),
		 !!(status & COLORBAR_STATUS_IDLE),
		 !!(status & COLORBAR_STATUS_FRAME_DONE),
		 !!(status & COLORBAR_STATUS_BUSY));

	return status;
}

static void colorbar_get_status_snapshot_locked(struct colorbar_device *cdev,
					       struct colorbar_status_info *snapshot)
{
	u32 raw = colorbar_ioread32(cdev, COLORBAR_REG_DMA_STATUS);
	u32 report_index = cdev->started ? cdev->active_buffer_index :
		(cdev->frame_counter ? cdev->last_buffer_index : cdev->next_buffer_index);

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->status_raw = raw;
	snapshot->status = colorbar_decode_status(raw);
	snapshot->dma_addr_low = cdev->bufs_allocated ?
		lower_32_bits(cdev->bufs[report_index % COLORBAR_BUFFER_COUNT].dma_addr) : 0;
	snapshot->active_addr = colorbar_read_cmd32_quiet(cdev,
						  COLORBAR_REG_DMA_ACTIVE_ADDR);
	snapshot->active_len = colorbar_read_cmd32_quiet(cdev,
						 COLORBAR_REG_DMA_ACTIVE_LEN);
	snapshot->bytes_sent = colorbar_read_cmd32_quiet(cdev,
						  COLORBAR_REG_DMA_BYTES_SENT);
	snapshot->frame_counter = cdev->frame_counter;
	snapshot->flags = 0;
}

static bool colorbar_status_version_ok(u32 status)
{
	return COLORBAR_STATUS_VERSION(status) == COLORBAR_DMA_STATUS_VERSION;
}

static int colorbar_wait_status_locked(struct colorbar_device *cdev,
				       const char *phase, u32 required_set,
				       u32 required_clear, unsigned int timeout_ms,
				       bool fail_on_error, u32 *last_status)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	u32 status;

	do {
		status = colorbar_read_status_locked(cdev, phase);
		if (last_status)
			*last_status = status;

		if (fail_on_error && colorbar_status_version_ok(status) &&
		    (status & COLORBAR_STATUS_ERROR_MASK)) {
			dev_err(&cdev->pdev->dev,
				"%s failed: FPGA error status=0x%08x\n",
				phase, status);
			return -EIO;
		}

		if (colorbar_status_version_ok(status) &&
		    ((status & required_set) == required_set) &&
		    !(status & required_clear))
			return 0;

		msleep(10);
	} while (time_before(jiffies, deadline));

	dev_err(&cdev->pdev->dev,
		"timeout waiting %s: last_status=0x%08x required_set=0x%08x required_clear=0x%08x\n",
		phase, status, required_set, required_clear);
	return -ETIMEDOUT;
}

static void colorbar_write_cmd32(struct colorbar_device *cdev, u32 value, u32 offset)
{
	u32 written = cmd_byteswap ? swab32(value) : value;

	dev_info(&cdev->pdev->dev,
		 "program DMA command: offset=0x%03x value=0x%08x written=0x%08x cmd_byteswap=%d\n",
		 offset, value, written, cmd_byteswap);
	colorbar_iowrite32(cdev, written, offset);
}

static void colorbar_write_dma_addr(struct colorbar_device *cdev, dma_addr_t dma_addr)
{
	u32 addr = lower_32_bits(dma_addr);
	u32 value = addr_byteswap ? swab32(addr) : addr;

	dev_info(&cdev->pdev->dev,
		 "program DMA address: dma=%pad low32=0x%08x written=0x%08x addr_byteswap=%d\n",
		 &dma_addr, addr, value, addr_byteswap);
	colorbar_iowrite32(cdev, value, COLORBAR_REG_DMA_ADDR);
}

static int colorbar_verify_readback_locked(struct colorbar_device *cdev)
{
	u32 expected;
	u32 got;
	u32 status;
	u32 hw_next;
	int i;

	if (verify_readback) {
		for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
			expected = lower_32_bits(cdev->bufs[i].dma_addr);
			got = colorbar_read_cmd32(cdev,
						 COLORBAR_REG_DMA_ADDR_ECHO0 + i * sizeof(u32));
			dev_info(&cdev->pdev->dev,
				 "read ADDR_ECHO%d decoded=0x%08x expected=0x%08x\n",
				 i, got, expected);

			if (got != expected) {
				dev_err(&cdev->pdev->dev,
					"refuse to start FPGA DMA: ADDR_ECHO%d mismatch got=0x%08x expected=0x%08x\n",
					i, got, expected);
				return -EIO;
			}
		}
	}

	status = colorbar_read_status_locked(cdev, "before BusMaster/START");
	if (!colorbar_status_version_ok(status)) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: STATUS version mismatch status=0x%08x expected_version=0x%02x\n",
			status, COLORBAR_DMA_STATUS_VERSION);
		return -EIO;
	}

	if (status & COLORBAR_STATUS_ERROR_MASK) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: FPGA reports error before START status=0x%08x\n",
			status);
		return -EIO;
	}

	if (!(status & COLORBAR_STATUS_IDLE) ||
	    !(status & COLORBAR_STATUS_HOST_ARM) ||
	    !(status & COLORBAR_STATUS_DMA_ADDR_VALID) ||
	    !(status & COLORBAR_STATUS_DMA_LEN_VALID) ||
	    (status & COLORBAR_STATUS_BUSY)) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: pre-start conditions not met status=0x%08x\n",
			status);
		return -EIO;
	}

	hw_next = COLORBAR_STATUS_NEXT_BUFFER(status);
	if (hw_next != cdev->next_buffer_index) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: ring index mismatch FPGA=%u driver=%u status=0x%08x\n",
			hw_next, cdev->next_buffer_index, status);
		return -EIO;
	}

	return 0;
}

static void colorbar_hw_safe_stop(struct colorbar_device *cdev)
{
	if (!cdev->bar)
		return;

	colorbar_iowrite32(cdev, 0, COLORBAR_REG_DMA_STOP);
	wmb();
	cdev->started = false;

	dev_info(&cdev->pdev->dev,
		 "sent DMA STOP on BAR%d\n",
		 cdev->bar_index);
}

static int colorbar_stop_wait_locked(struct colorbar_device *cdev)
{
	u32 required = COLORBAR_STATUS_STOP_ACK | COLORBAR_STATUS_IDLE;
	u32 clear = COLORBAR_STATUS_BUSY | COLORBAR_STATUS_STOP_PENDING;

	colorbar_hw_safe_stop(cdev);
	return colorbar_wait_status_locked(cdev, "for STOP_ACK+IDLE", required,
					 clear, COLORBAR_STOP_TIMEOUT_MS, false, NULL);
}

static int colorbar_check_guard_locked(struct colorbar_device *cdev, u32 buffer_index)
{
	u8 *guard = cdev->bufs[buffer_index].cpu_addr + COLORBAR_FRAME_SIZE;
	size_t i;

	for (i = 0; i < COLORBAR_DMA_GUARD_SIZE; i++) {
		if (guard[i] != COLORBAR_DMA_PREFILL_PATTERN) {
			dev_err(&cdev->pdev->dev,
				"DMA buffer%u guard overwritten at +0x%zx: got=0x%02x expected=0x%02x\n",
				buffer_index, (size_t)COLORBAR_FRAME_SIZE + i, guard[i],
				COLORBAR_DMA_PREFILL_PATTERN);
			return -EIO;
		}
	}

	dev_info(&cdev->pdev->dev,
		 "DMA buffer%u guard intact: [%u, %u) size=%u pattern=0x%02x\n",
		 buffer_index, COLORBAR_FRAME_SIZE, COLORBAR_BUFFER_SIZE, COLORBAR_DMA_GUARD_SIZE,
		 COLORBAR_DMA_PREFILL_PATTERN);
	return 0;
}

static void colorbar_read_dma_progress_locked(struct colorbar_device *cdev,
					      const char *phase, u32 *active_addr,
					      u32 *active_len, u32 *bytes_sent)
{
	*active_addr = colorbar_read_cmd32(cdev, COLORBAR_REG_DMA_ACTIVE_ADDR);
	*active_len = colorbar_read_cmd32(cdev, COLORBAR_REG_DMA_ACTIVE_LEN);
	*bytes_sent = colorbar_read_cmd32(cdev, COLORBAR_REG_DMA_BYTES_SENT);

	dev_info(&cdev->pdev->dev,
		 "DMA progress %s: active_addr=0x%08x active_len=%u bytes_sent=%u bytes_sent_hex=0x%08x remaining=%u\n",
		 phase, *active_addr, *active_len, *bytes_sent, *bytes_sent,
		 *bytes_sent <= COLORBAR_FRAME_SIZE ? COLORBAR_FRAME_SIZE - *bytes_sent : 0);
}

static int colorbar_finish_frame_locked(struct colorbar_device *cdev)
{
	u32 status;
	u32 active_addr;
	u32 active_len;
	u32 bytes_sent;
	u32 buffer_index = cdev->active_buffer_index;
	u32 expected_addr = lower_32_bits(cdev->bufs[buffer_index].dma_addr);
	int ret;

	ret = colorbar_wait_status_locked(cdev, "for frame DONE+IDLE",
					COLORBAR_STATUS_FRAME_DONE | COLORBAR_STATUS_IDLE,
					COLORBAR_STATUS_BUSY | COLORBAR_STATUS_STOP_PENDING,
					frame_wait_ms, true, &status);
	if (ret) {
		colorbar_read_dma_progress_locked(cdev, "after timeout",
						      &active_addr, &active_len, &bytes_sent);
		return ret;
	}

	if (status & COLORBAR_STATUS_ERROR_MASK) {
		colorbar_read_dma_progress_locked(cdev, "after error status",
						      &active_addr, &active_len, &bytes_sent);
		return -EIO;
	}

	colorbar_read_dma_progress_locked(cdev, "after DONE",
					      &active_addr, &active_len, &bytes_sent);

	dev_info(&cdev->pdev->dev,
		 "DMA result: buffer=%u active_addr=0x%08x expected_addr=0x%08x active_len=%u bytes_sent=%u\n",
		 buffer_index, active_addr, expected_addr, active_len, bytes_sent);

	if (active_addr != expected_addr || active_len != COLORBAR_FRAME_SIZE ||
	    bytes_sent != COLORBAR_FRAME_SIZE) {
		dev_err(&cdev->pdev->dev,
			"DMA result mismatch: active_addr=0x%08x expected=0x%08x active_len=%u expected_len=%u bytes_sent=%u\n",
			active_addr, expected_addr, active_len, COLORBAR_FRAME_SIZE,
			bytes_sent);
		return -EIO;
	}

	dma_rmb();
	return colorbar_check_guard_locked(cdev, buffer_index);
}

static int colorbar_start_locked(struct colorbar_device *cdev)
{
	int ret;
	int i;
	u32 buffer_index;

	if (block_unsafe_dma) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: emergency block switch is set\n");
		return -EPERM;
	}

	if (!allow_dma_start) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: allow_dma_start=1 is not set\n");
		return -EPERM;
	}

	if (!colorbar_dma_len_valid()) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: PCIE_DMA_single_5 requires dma_len_bytes=%u, got %u\n",
			COLORBAR_FRAME_SIZE, dma_len_bytes);
		return -EINVAL;
	}

	if (!cdev->bufs_allocated)
		return -EINVAL;
	if (cdev->started || cdev->frame_ready)
		return -EBUSY;

	if (cdev->buffer_size < COLORBAR_BUFFER_SIZE) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: allocated buffer_size=%zu is smaller than required 4MiB=%u\n",
			cdev->buffer_size, COLORBAR_BUFFER_SIZE);
		return -EINVAL;
	}

	if (!cdev->ring_configured) {
		pci_clear_master(cdev->pdev);
		wmb();
		ret = colorbar_stop_wait_locked(cdev);
		if (ret) {
			pci_clear_master(cdev->pdev);
			return ret;
		}

		for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
			memset(cdev->bufs[i].cpu_addr, COLORBAR_DMA_PREFILL_PATTERN,
			       cdev->buffer_size);
		dma_wmb();

		cdev->next_buffer_index = 0;
		colorbar_write_cmd32(cdev, COLORBAR_DMA_ARM_MAGIC, COLORBAR_REG_DMA_ARM);
		wmb();
		udelay(10);
		for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
			colorbar_write_dma_addr(cdev, cdev->bufs[i].dma_addr);
			wmb();
			udelay(10);
		}
		colorbar_write_cmd32(cdev, COLORBAR_FRAME_SIZE, COLORBAR_REG_DMA_LEN);
		wmb();
		udelay(10);
	} else {
		buffer_index = cdev->next_buffer_index;
		memset(cdev->bufs[buffer_index].cpu_addr, COLORBAR_DMA_PREFILL_PATTERN,
		       cdev->buffer_size);
		dma_wmb();
	}

	ret = colorbar_verify_readback_locked(cdev);
	if (ret) {
		colorbar_stop_wait_locked(cdev);
		pci_clear_master(cdev->pdev);
		cdev->ring_configured = false;
		return ret;
	}

	pci_set_master(cdev->pdev);
	wmb();
	ret = colorbar_wait_status_locked(cdev, "for BusMaster visible",
					COLORBAR_STATUS_PCIE_DMA_ENABLE,
					COLORBAR_STATUS_BUSY | COLORBAR_STATUS_ERROR_MASK |
					COLORBAR_STATUS_STOP_PENDING,
					COLORBAR_BME_TIMEOUT_MS, true, NULL);
	if (ret) {
		colorbar_stop_wait_locked(cdev);
		pci_clear_master(cdev->pdev);
		cdev->ring_configured = false;
		return ret;
	}

	buffer_index = cdev->next_buffer_index;
	colorbar_write_cmd32(cdev, 1, COLORBAR_REG_DMA_START);
	wmb();

	cdev->ring_configured = true;
	cdev->started = true;
	cdev->frame_ready = false;
	cdev->active_buffer_index = buffer_index;

	dev_info(&cdev->pdev->dev,
		 "started PCIE_DMA_single_5 four-buffer frame RX: buffer=%u frame_len=%u buffer_size=%u guard=%u cmd_byteswap=%d addr_byteswap=%d readback_byteswap=%d timeout_ms=%u\n",
		 buffer_index, COLORBAR_FRAME_SIZE, COLORBAR_BUFFER_SIZE, COLORBAR_DMA_GUARD_SIZE,
		 cmd_byteswap, addr_byteswap, readback_byteswap, frame_wait_ms);
	return 0;
}

static void colorbar_stop_locked(struct colorbar_device *cdev)
{
	pci_set_master(cdev->pdev);
	wmb();
	udelay(10);
	colorbar_stop_wait_locked(cdev);
	pci_clear_master(cdev->pdev);
	cdev->ring_configured = false;
	cdev->started = false;
	cdev->frame_ready = false;
	cdev->next_buffer_index = 0;
	cdev->active_buffer_index = 0;
}

static long colorbar_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct colorbar_device *cdev = g_cdev;
	struct colorbar_rx_info info;
	struct colorbar_frame_info frame;
	int ret = 0;

	if (!cdev || !cdev->pdev)
		return -ENODEV;

	switch (cmd) {
	case COLORBAR_IOC_GET_INFO:
		info.width = COLORBAR_WIDTH;
		info.height = COLORBAR_HEIGHT;
		info.format = COLORBAR_FORMAT_RGB565;
		info.buffer_count = COLORBAR_BUFFER_COUNT;
		info.frame_size = COLORBAR_FRAME_SIZE;
		info.buffer_size = colorbar_requested_buffer_size();
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	case COLORBAR_IOC_ALLOC_BUFS:
		mutex_lock(&cdev->lock);
		ret = colorbar_alloc_buffers_locked(cdev);
		mutex_unlock(&cdev->lock);
		return ret;

	case COLORBAR_IOC_START:
		mutex_lock(&cdev->lock);
		ret = colorbar_start_locked(cdev);
		mutex_unlock(&cdev->lock);
		return ret;

	case COLORBAR_IOC_STOP:
		mutex_lock(&cdev->lock);
		colorbar_stop_locked(cdev);
		mutex_unlock(&cdev->lock);
		return 0;

	case COLORBAR_IOC_WAIT_FRAME:
		mutex_lock(&cdev->lock);
		if (!cdev->started) {
			ret = -EINVAL;
		} else {
			ret = colorbar_finish_frame_locked(cdev);
			if (!ret) {
				cdev->started = false;
				cdev->frame_counter++;
				cdev->last_buffer_index = cdev->active_buffer_index;
				cdev->next_buffer_index =
					(cdev->active_buffer_index + 1) % COLORBAR_BUFFER_COUNT;
				cdev->frame_ready = true;

				frame.frame_counter = cdev->frame_counter;
				frame.buffer_index = cdev->last_buffer_index;
				frame.valid_size = COLORBAR_FRAME_SIZE;
				frame.flags = 0;

				if (copy_to_user((void __user *)arg, &frame, sizeof(frame)))
					ret = -EFAULT;
			}

			if (ret)
				colorbar_stop_locked(cdev);
		}
		mutex_unlock(&cdev->lock);
		return ret;

	case COLORBAR_IOC_FREE_BUFS:
		mutex_lock(&cdev->lock);
		colorbar_stop_locked(cdev);
		colorbar_free_buffers_locked(cdev);
		mutex_unlock(&cdev->lock);
		return 0;

	case COLORBAR_IOC_SAFE_STOP:
		mutex_lock(&cdev->lock);
		colorbar_stop_locked(cdev);
		mutex_unlock(&cdev->lock);
		return 0;

	case COLORBAR_IOC_GET_STATUS: {
		struct colorbar_status_info status;

		mutex_lock(&cdev->lock);
		colorbar_get_status_snapshot_locked(cdev, &status);
		mutex_unlock(&cdev->lock);

		if (copy_to_user((void __user *)arg, &status, sizeof(status)))
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static ssize_t colorbar_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct colorbar_device *cdev = g_cdev;
	void *src;
	size_t available;
	size_t to_copy;
	ssize_t ret;

	if (!cdev || !cdev->pdev)
		return -ENODEV;

	mutex_lock(&cdev->lock);
	if (!cdev->bufs_allocated || !cdev->frame_ready) {
		ret = -EINVAL;
		goto out;
	}

	if (*ppos < 0 || *ppos >= COLORBAR_FRAME_SIZE) {
		ret = 0;
		goto out;
	}

	available = COLORBAR_FRAME_SIZE - (size_t)*ppos;
	to_copy = min(count, available);
	src = cdev->bufs[cdev->last_buffer_index].cpu_addr + *ppos;

	if (copy_to_user(buf, src, to_copy)) {
		ret = -EFAULT;
		goto out;
	}

	*ppos += to_copy;
	if (*ppos == COLORBAR_FRAME_SIZE)
		cdev->frame_ready = false;
	ret = to_copy;

out:
	mutex_unlock(&cdev->lock);
	return ret;
}

static loff_t colorbar_llseek(struct file *file, loff_t off, int whence)
{
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = off;
		break;
	case SEEK_CUR:
		newpos = file->f_pos + off;
		break;
	case SEEK_END:
		newpos = COLORBAR_FRAME_SIZE + off;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0)
		return -EINVAL;

	file->f_pos = newpos;
	return newpos;
}

static int colorbar_open(struct inode *inode, struct file *file)
{
	if (!g_cdev || !g_cdev->pdev)
		return -ENODEV;
	return 0;
}

static int colorbar_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations colorbar_fops = {
	.owner = THIS_MODULE,
	.llseek = colorbar_llseek,
	.read = colorbar_read,
	.open = colorbar_open,
	.release = colorbar_release,
	.unlocked_ioctl = colorbar_ioctl,
};

static int colorbar_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct colorbar_device *cdev;
	unsigned long bar_address;
	int ret;

	dev_info(&pdev->dev, "colorbar probe vendor=0x%x device=0x%x\n",
		 pdev->vendor, pdev->device);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
		return ret;
	}

	ret = colorbar_set_dma_mask(pdev);
	if (ret)
		goto disable_device;

	if (bar < 0 || bar > 5) {
		ret = -EINVAL;
		goto disable_device;
	}

	ret = pci_request_region(pdev, bar, COLORBAR_DEVICE_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request BAR%d: %d\n", bar, ret);
		goto disable_device;
	}

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev) {
		ret = -ENOMEM;
		goto release_region;
	}

	cdev->pdev = pdev;
	cdev->bar_index = bar;
	cdev->bar_len = pci_resource_len(pdev, bar);
	mutex_init(&cdev->lock);

	bar_address = pci_resource_start(pdev, bar);
	cdev->bar = ioremap(bar_address, cdev->bar_len);
	if (!cdev->bar) {
		ret = -ENOMEM;
		goto free_cdev;
	}

	pci_clear_master(pdev);
	wmb();
	colorbar_hw_safe_stop(cdev);

	pci_set_drvdata(pdev, cdev);
	g_cdev = cdev;

	dev_info(&pdev->dev,
		 "colorbar PCIe RX probe ok, BAR%d phys=0x%lx len=%pa, BusMaster disabled until START\n",
		 bar, bar_address, &cdev->bar_len);
	return 0;

free_cdev:
	kfree(cdev);
release_region:
	pci_release_region(pdev, bar);
disable_device:
	pci_disable_device(pdev);
	return ret;
}

static void colorbar_remove(struct pci_dev *pdev)
{
	struct colorbar_device *cdev = pci_get_drvdata(pdev);

	if (!cdev)
		return;

	mutex_lock(&cdev->lock);
	colorbar_stop_locked(cdev);
	colorbar_free_buffers_locked(cdev);
	mutex_unlock(&cdev->lock);

	if (cdev->bar)
		iounmap(cdev->bar);

	if (g_cdev == cdev)
		g_cdev = NULL;

	pci_release_region(pdev, cdev->bar_index);
	pci_clear_master(pdev);
	pci_disable_device(pdev);
	kfree(cdev);
}

static const struct pci_device_id colorbar_ids[] = {
	{ PCI_DEVICE(COLORBAR_PCI_VENDOR_ID, COLORBAR_PCI_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, colorbar_ids);

static struct pci_driver colorbar_pci_driver = {
	.name = COLORBAR_DEVICE_NAME,
	.id_table = colorbar_ids,
	.probe = colorbar_probe,
	.remove = colorbar_remove,
};

static int __init colorbar_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&g_devt, 0, 1, COLORBAR_DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&g_chardev, &colorbar_fops);
	g_chardev.owner = THIS_MODULE;
	ret = cdev_add(&g_chardev, g_devt, 1);
	if (ret)
		goto unregister_chrdev;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	g_class = class_create(COLORBAR_CLASS_NAME);
#else
	g_class = class_create(THIS_MODULE, COLORBAR_CLASS_NAME);
#endif
	if (IS_ERR(g_class)) {
		ret = PTR_ERR(g_class);
		goto del_cdev;
	}

	g_device = device_create(g_class, NULL, g_devt, NULL, COLORBAR_DEVICE_NAME);
	if (IS_ERR(g_device)) {
		ret = PTR_ERR(g_device);
		goto destroy_class;
	}

	ret = pci_register_driver(&colorbar_pci_driver);
	if (ret)
		goto destroy_device;

	pr_info("colorbar PCIe RX driver loaded for PCIE_DMA_single_5: four 4MiB buffers, 4147200-byte frames, START gated by allow_dma_start\n");
	return 0;

destroy_device:
	device_destroy(g_class, g_devt);
destroy_class:
	class_destroy(g_class);
del_cdev:
	cdev_del(&g_chardev);
unregister_chrdev:
	unregister_chrdev_region(g_devt, 1);
	return ret;
}

static void __exit colorbar_exit(void)
{
	pci_unregister_driver(&colorbar_pci_driver);
	device_destroy(g_class, g_devt);
	class_destroy(g_class);
	cdev_del(&g_chardev);
	unregister_chrdev_region(g_devt, 1);
	pr_info("colorbar PCIe RX driver unloaded\n");
}

module_init(colorbar_init);
module_exit(colorbar_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("cat_pcie_project");
MODULE_DESCRIPTION("RK3568 PCIe colorbar RGB565 receiver driver, rewritten toward verified Pango DMA style");
