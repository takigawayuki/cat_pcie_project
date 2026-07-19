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

#include "colorbar_pcie_driver.h"

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
	u32 frame_counter;
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
MODULE_PARM_DESC(addr_byteswap, "byteswap 32-bit DMA address before writing BAR+0x110");

static bool allow_dma_start;
module_param(allow_dma_start, bool, 0644);
MODULE_PARM_DESC(allow_dma_start, "allow COLORBAR_IOC_START to program FPGA DMA addresses; default is false for safety");

static uint frame_wait_ms = 100;
module_param(frame_wait_ms, uint, 0644);
MODULE_PARM_DESC(frame_wait_ms, "temporary wait time for one frame until FPGA frame-done status is available");

static void colorbar_free_buffers_locked(struct colorbar_device *cdev)
{
	int i;

	if (!cdev->bufs_allocated)
		return;

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
		if (cdev->bufs[i].cpu_addr) {
			dma_free_coherent(&cdev->pdev->dev, COLORBAR_BUFFER_SIZE,
					  cdev->bufs[i].cpu_addr,
					  cdev->bufs[i].dma_addr);
			cdev->bufs[i].cpu_addr = NULL;
			cdev->bufs[i].dma_addr = 0;
		}
	}

	cdev->bufs_allocated = false;
	cdev->started = false;
	cdev->frame_counter = 0;
}

static int colorbar_alloc_buffers_locked(struct colorbar_device *cdev)
{
	int i;

	if (cdev->bufs_allocated)
		return 0;

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
		cdev->bufs[i].cpu_addr = dma_alloc_coherent(&cdev->pdev->dev,
							    COLORBAR_BUFFER_SIZE,
							    &cdev->bufs[i].dma_addr,
							    GFP_KERNEL);
		if (!cdev->bufs[i].cpu_addr)
			goto fail;

		if (upper_32_bits(cdev->bufs[i].dma_addr)) {
			dev_err(&cdev->pdev->dev,
				"DMA address above 32-bit range: buffer %d dma=%pad\n",
				i, &cdev->bufs[i].dma_addr);
			goto fail;
		}

		memset(cdev->bufs[i].cpu_addr, 0, COLORBAR_BUFFER_SIZE);
		dev_info(&cdev->pdev->dev, "buffer%d cpu=%p dma=%pad size=%u\n",
			 i, cdev->bufs[i].cpu_addr, &cdev->bufs[i].dma_addr,
			 COLORBAR_BUFFER_SIZE);
	}

	cdev->bufs_allocated = true;
	return 0;

fail:
	colorbar_free_buffers_locked(cdev);
	return -ENOMEM;
}

static void colorbar_write_dma_addr(struct colorbar_device *cdev, dma_addr_t dma_addr)
{
	u32 addr = lower_32_bits(dma_addr);
	u32 value = addr_byteswap ? swab32(addr) : addr;

	dev_info(&cdev->pdev->dev,
		 "program DMA address: dma=%pad low32=0x%08x written=0x%08x addr_byteswap=%d\n",
		 &dma_addr, addr, value, addr_byteswap);
	iowrite32(value, cdev->bar + COLORBAR_REG_DMA_ADDR);
}


static void colorbar_hw_safe_stop(struct colorbar_device *cdev)
{
	int i;

	if (!cdev->bar)
		return;

	iowrite32(0, cdev->bar + COLORBAR_REG_DMA_STOP);
	wmb();
	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
		iowrite32(0, cdev->bar + COLORBAR_REG_DMA_ADDR);
	wmb();
	cdev->started = false;

	dev_info(&cdev->pdev->dev,
		 "sent DMA STOP + cleared dma_addr0..3 on BAR%d\n",
		 cdev->bar_index);
}

static int colorbar_start_locked(struct colorbar_device *cdev)
{
	int i;

	if (!cdev->bufs_allocated)
		return -EINVAL;

	if (!allow_dma_start) {
		dev_err(&cdev->pdev->dev,
			"refuse to start FPGA DMA: reload with allow_dma_start=1 after BAR/address byte order is verified\n");
		return -EPERM;
	}

	/* Keep Bus Master disabled while programming DMA addresses. */
	pci_clear_master(cdev->pdev);

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
		memset(cdev->bufs[i].cpu_addr, 0, COLORBAR_BUFFER_SIZE);

	for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
		colorbar_write_dma_addr(cdev, cdev->bufs[i].dma_addr);

	wmb();
	pci_set_master(cdev->pdev);
	wmb();
	cdev->started = true;
	cdev->frame_counter = 0;

	dev_info(&cdev->pdev->dev,
		 "started colorbar RX, addr_byteswap=%d, allow_dma_start=%d, wait_ms=%u, BusMaster enabled only for capture window\n",
		 addr_byteswap, allow_dma_start, frame_wait_ms);
	return 0;
}

static void colorbar_stop_locked(struct colorbar_device *cdev)
{
	/* Block further PCIe Memory Writes first; MMIO STOP does not require Bus Master. */
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
		info.buffer_size = COLORBAR_BUFFER_SIZE;
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
			frame.frame_counter = cdev->frame_counter;
			frame.buffer_index = (cdev->frame_counter - 1) % COLORBAR_BUFFER_COUNT;
			frame.valid_size = COLORBAR_FRAME_SIZE;
			frame.flags = 0;

			/* One-shot safety: close the DMA window before userspace copies data. */
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
		colorbar_hw_safe_stop(cdev);
		mutex_unlock(&cdev->lock);
		return 0;

	default:
		return -ENOTTY;
	}
}

static int colorbar_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct colorbar_device *cdev = g_cdev;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned int index;
	unsigned long in_buf_offset;
	int ret;

	if (!cdev || !cdev->pdev)
		return -ENODEV;

	mutex_lock(&cdev->lock);
	if (!cdev->bufs_allocated) {
		ret = -EINVAL;
		goto out;
	}

	index = offset / COLORBAR_BUFFER_SIZE;
	in_buf_offset = offset % COLORBAR_BUFFER_SIZE;
	if (index >= COLORBAR_BUFFER_COUNT ||
	    in_buf_offset + size > COLORBAR_BUFFER_SIZE ||
	    (in_buf_offset & (PAGE_SIZE - 1))) {
		ret = -EINVAL;
		goto out;
	}

	vma->vm_pgoff = in_buf_offset >> PAGE_SHIFT;
	ret = dma_mmap_coherent(&cdev->pdev->dev, vma,
				cdev->bufs[index].cpu_addr,
				cdev->bufs[index].dma_addr,
				COLORBAR_BUFFER_SIZE);
out:
	mutex_unlock(&cdev->lock);
	return ret;
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
	.open = colorbar_open,
	.release = colorbar_release,
	.unlocked_ioctl = colorbar_ioctl,
	.mmap = colorbar_mmap,
};

static int colorbar_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct colorbar_device *cdev;
	int ret;

	ret = pci_enable_device_mem(pdev);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "failed to set 32-bit DMA mask: %d\n", ret);
		goto disable_device;
	}

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
	cdev->bar = pci_iomap(pdev, bar, 0);
	if (!cdev->bar) {
		ret = -ENOMEM;
		goto free_cdev;
	}

	/*
	 * 安全保护：驱动加载阶段保持 Bus Master 关闭。
	 * pci_enable_device_mem() 允许主机 MMIO 写 BAR，但不需要 FPGA
	 * 具备主动 DMA 能力。probe 只做 STOP/清旧地址，真正 START
	 * 时才短暂打开 Bus Master，降低错误 DMA 持续写内存的风险。
	 */
	pci_clear_master(pdev);
	colorbar_hw_safe_stop(cdev);

	pci_set_drvdata(pdev, cdev);
	g_cdev = cdev;

	dev_info(&pdev->dev, "colorbar PCIe RX probe ok, BAR%d len=%pa, BusMaster disabled until START\n",
		 bar, &cdev->bar_len);
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
	pci_clear_master(pdev);
	colorbar_free_buffers_locked(cdev);
	mutex_unlock(&cdev->lock);

	if (cdev->bar)
		pci_iounmap(pdev, cdev->bar);

	if (g_cdev == cdev)
		g_cdev = NULL;

	pci_release_region(pdev, cdev->bar_index);
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

	g_class = class_create(THIS_MODULE, COLORBAR_CLASS_NAME);
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

	pr_info("colorbar PCIe RX driver loaded\n");
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
MODULE_DESCRIPTION("RK3568 PCIe colorbar RGB565 receiver driver");
