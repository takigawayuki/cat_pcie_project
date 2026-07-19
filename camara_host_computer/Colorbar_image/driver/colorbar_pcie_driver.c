#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/swab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "colorbar_pcie_driver.h"

#define COLORBAR_DMA_PREFILL_PATTERN 0xA5

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
	bool started;
	bool frame_ready;
	size_t buffer_size;
	u32 frame_counter;
	u32 last_buffer_index;
};

static struct colorbar_device *g_cdev;
static dev_t g_devt;
static struct cdev g_chardev;
static struct class *g_class;
static struct device *g_device;

static int bar = COLORBAR_BAR_DEFAULT;
module_param(bar, int, 0444);
MODULE_PARM_DESC(bar, "FPGA BAR index for colorbar control registers");

static bool addr_byteswap = true;
module_param(addr_byteswap, bool, 0644);
MODULE_PARM_DESC(addr_byteswap, "byteswap 32-bit DMA address before writing BAR+0x110; default true for PCIE_DMA_safe_test cmd_data byte order");

static bool allow_dma_start;
module_param(allow_dma_start, bool, 0644);
MODULE_PARM_DESC(allow_dma_start, "allow START after programming the new FPGA ARM/LEN/ADDR handshake; default false");

static bool block_unsafe_dma;
module_param(block_unsafe_dma, bool, 0644);
MODULE_PARM_DESC(block_unsafe_dma, "legacy emergency block switch; keep false for new ARM/LEN/START FPGA protocol");

static uint dma_len_bytes = 64;
module_param(dma_len_bytes, uint, 0644);
MODULE_PARM_DESC(dma_len_bytes, "requested DMA byte length: default 64 for safe staged validation; use 4096 then full frame explicitly");

static uint frame_wait_ms = 100;
module_param(frame_wait_ms, uint, 0644);
MODULE_PARM_DESC(frame_wait_ms, "temporary wait time for one frame until FPGA frame-done status is available");

static bool colorbar_dma_len_valid(void)
{
	return dma_len_bytes != 0 && dma_len_bytes <= COLORBAR_FRAME_SIZE;
}

static size_t colorbar_requested_buffer_size(void)
{
	if (!colorbar_dma_len_valid())
		return 0;

	return (size_t)PAGE_ALIGN(dma_len_bytes);
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
		buffer_size = colorbar_requested_buffer_size();
	if (!buffer_size)
		buffer_size = COLORBAR_BUFFER_SIZE;

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
		if (cdev->bufs[i].cpu_addr) {
			pci_free_consistent(cdev->pdev, buffer_size,
					    cdev->bufs[i].cpu_addr,
					    cdev->bufs[i].dma_addr);
			cdev->bufs[i].cpu_addr = NULL;
			cdev->bufs[i].dma_addr = 0;
		}
	}

	cdev->bufs_allocated = false;
	cdev->buffer_size = 0;
	cdev->started = false;
	cdev->frame_ready = false;
	cdev->frame_counter = 0;
	cdev->last_buffer_index = 0;
}

static int colorbar_alloc_buffers_locked(struct colorbar_device *cdev)
{
	size_t buffer_size = colorbar_requested_buffer_size();
	int i;

	if (!buffer_size) {
		dev_err(&cdev->pdev->dev,
			"refuse to allocate DMA buffers: invalid dma_len_bytes=%u, max=%u\n",
			dma_len_bytes, COLORBAR_FRAME_SIZE);
		return -EINVAL;
	}

	if (cdev->bufs_allocated) {
		if (cdev->buffer_size == buffer_size)
			return 0;
		colorbar_free_buffers_locked(cdev);
	}

	cdev->buffer_size = buffer_size;

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
		cdev->bufs[i].cpu_addr = pci_alloc_consistent(cdev->pdev,
							 buffer_size,
							 &cdev->bufs[i].dma_addr);
		if (!cdev->bufs[i].cpu_addr)
			goto fail;

		if (upper_32_bits(cdev->bufs[i].dma_addr)) {
			dev_err(&cdev->pdev->dev,
				"DMA address above 32-bit range: buffer %d dma=%pad\n",
				i, &cdev->bufs[i].dma_addr);
			goto fail;
		}

		memset(cdev->bufs[i].cpu_addr, COLORBAR_DMA_PREFILL_PATTERN, buffer_size);
		dev_info(&cdev->pdev->dev, "buffer%d cpu=%p dma=%pad size=%zu requested_len=%u\n",
			 i, cdev->bufs[i].cpu_addr, &cdev->bufs[i].dma_addr,
			 buffer_size, dma_len_bytes);
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

static void colorbar_write_cmd32(struct colorbar_device *cdev, u32 value, u32 offset)
{
	u32 written = swab32(value);

	dev_info(&cdev->pdev->dev,
		 "program DMA command: offset=0x%03x value=0x%08x written=0x%08x\n",
		 offset, value, written);
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

static void colorbar_hw_safe_stop(struct colorbar_device *cdev)
{
	int i;

	if (!cdev->bar)
		return;

	colorbar_iowrite32(cdev, 0, COLORBAR_REG_DMA_STOP);
	wmb();
	colorbar_iowrite32(cdev, 0, COLORBAR_REG_DMA_START);
	colorbar_iowrite32(cdev, 0, COLORBAR_REG_DMA_ARM);
	colorbar_iowrite32(cdev, 0, COLORBAR_REG_DMA_LEN);
	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
		colorbar_iowrite32(cdev, 0, COLORBAR_REG_DMA_ADDR);
	wmb();
	cdev->started = false;

	dev_info(&cdev->pdev->dev,
		 "sent DMA STOP + cleared ARM/START/LEN/dma_addr0..3 on BAR%d\n",
		 cdev->bar_index);
}

static int colorbar_start_locked(struct colorbar_device *cdev)
{
	int i;

	if (block_unsafe_dma) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: Pango-style rewrite keeps colorbar stream blocked until protocol is revalidated\n");
		return -EPERM;
	}

	if (!allow_dma_start) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: allow_dma_start=1 is not set\n");
		return -EPERM;
	}

	if (!colorbar_dma_len_valid()) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: invalid dma_len_bytes=%u, max=%u\n",
			dma_len_bytes, COLORBAR_FRAME_SIZE);
		return -EINVAL;
	}

	if (!cdev->bufs_allocated)
		return -EINVAL;

	if (cdev->buffer_size < colorbar_requested_buffer_size()) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: allocated buffer_size=%zu is smaller than requested len=%u\n",
			cdev->buffer_size, dma_len_bytes);
		return -EINVAL;
	}

	pci_clear_master(cdev->pdev);
	colorbar_hw_safe_stop(cdev);

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
		memset(cdev->bufs[i].cpu_addr, COLORBAR_DMA_PREFILL_PATTERN, cdev->buffer_size);

	colorbar_write_cmd32(cdev, COLORBAR_DMA_ARM_MAGIC, COLORBAR_REG_DMA_ARM);
	colorbar_write_cmd32(cdev, dma_len_bytes, COLORBAR_REG_DMA_LEN);
	wmb();

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
		colorbar_write_dma_addr(cdev, cdev->bufs[i].dma_addr);

	wmb();
	pci_set_master(cdev->pdev);
	wmb();
	colorbar_write_cmd32(cdev, 1, COLORBAR_REG_DMA_START);
	wmb();

	cdev->started = true;
	cdev->frame_ready = false;
	cdev->frame_counter = 0;
	cdev->last_buffer_index = 0;

	dev_info(&cdev->pdev->dev,
		 "started colorbar RX with ARM/LEN/ADDR/START handshake, len=%u, addr_byteswap=%d, wait_ms=%u\n",
		 dma_len_bytes, addr_byteswap, frame_wait_ms);
	return 0;
}

static void colorbar_stop_locked(struct colorbar_device *cdev)
{
	pci_clear_master(cdev->pdev);
	colorbar_hw_safe_stop(cdev);
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
			mutex_unlock(&cdev->lock);
			msleep(frame_wait_ms);
			mutex_lock(&cdev->lock);

			cdev->frame_counter++;
			cdev->last_buffer_index = (cdev->frame_counter - 1) % COLORBAR_BUFFER_COUNT;
			cdev->frame_ready = true;

			frame.frame_counter = cdev->frame_counter;
			frame.buffer_index = cdev->last_buffer_index;
			frame.valid_size = dma_len_bytes;
			frame.flags = 0;

			colorbar_stop_locked(cdev);

			if (copy_to_user((void __user *)arg, &frame, sizeof(frame)))
				ret = -EFAULT;
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

	if (*ppos < 0 || *ppos >= dma_len_bytes) {
		ret = 0;
		goto out;
	}

	available = dma_len_bytes - (size_t)*ppos;
	to_copy = min(count, available);
	src = cdev->bufs[cdev->last_buffer_index].cpu_addr + *ppos;

	if (copy_to_user(buf, src, to_copy)) {
		ret = -EFAULT;
		goto out;
	}

	*ppos += to_copy;
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
		newpos = dma_len_bytes + off;
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

	pr_info("colorbar PCIe RX driver loaded, new FPGA ARM/LEN/ADDR/START handshake, START gated by allow_dma_start\n");
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
