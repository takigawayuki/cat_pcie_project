#ifndef COLORBAR_PCIE_RX_H
#define COLORBAR_PCIE_RX_H

#include <stdint.h>
#include <sys/ioctl.h>

#define COLORBAR_DEVICE_PATH "/dev/colorbar_pcie_rx"

#define COLORBAR_WIDTH 1920u
#define COLORBAR_HEIGHT 1080u
#define COLORBAR_BYTES_PER_PIXEL 2u
#define COLORBAR_FRAME_SIZE (COLORBAR_WIDTH * COLORBAR_HEIGHT * COLORBAR_BYTES_PER_PIXEL)
#define COLORBAR_DMA_BUFFER_SIZE (4u * 1024u * 1024u)
#define COLORBAR_DMA_GUARD_SIZE (COLORBAR_DMA_BUFFER_SIZE - COLORBAR_FRAME_SIZE)
#define COLORBAR_DMA_MAX_BYTES COLORBAR_FRAME_SIZE
#define COLORBAR_BUFFER_COUNT 4u
#define COLORBAR_BUFFER_SIZE COLORBAR_DMA_BUFFER_SIZE

#define COLORBAR_FORMAT_RGB565 0x00005651u

#define COLORBAR_IOCTL_MAGIC 'C'

struct colorbar_rx_info {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t buffer_count;
    uint32_t frame_size;
    uint32_t buffer_size;
};

struct colorbar_frame_info {
    uint32_t buffer_index;
    uint32_t frame_counter;
    uint32_t valid_size;
    uint32_t flags;
};

struct colorbar_status_info {
    uint32_t status_raw;
    uint32_t status;
    uint32_t dma_addr_low;
    uint32_t active_addr;
    uint32_t active_len;
    uint32_t bytes_sent;
    uint32_t frame_counter;
    uint32_t flags;
};

#define COLORBAR_IOC_GET_INFO _IOR(COLORBAR_IOCTL_MAGIC, 0x00, struct colorbar_rx_info)
#define COLORBAR_IOC_ALLOC_BUFS _IO(COLORBAR_IOCTL_MAGIC, 0x01)
#define COLORBAR_IOC_START _IO(COLORBAR_IOCTL_MAGIC, 0x02)
#define COLORBAR_IOC_STOP _IO(COLORBAR_IOCTL_MAGIC, 0x03)
#define COLORBAR_IOC_WAIT_FRAME _IOR(COLORBAR_IOCTL_MAGIC, 0x04, struct colorbar_frame_info)
#define COLORBAR_IOC_FREE_BUFS _IO(COLORBAR_IOCTL_MAGIC, 0x05)
#define COLORBAR_IOC_SAFE_STOP _IO(COLORBAR_IOCTL_MAGIC, 0x06)
#define COLORBAR_IOC_GET_STATUS _IOR(COLORBAR_IOCTL_MAGIC, 0x07, struct colorbar_status_info)

#endif
