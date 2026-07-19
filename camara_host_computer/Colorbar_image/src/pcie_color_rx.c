#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "colorbar_pcie_rx.h"

struct sample_point {
    uint32_t x;
    uint32_t y;
    uint16_t expected;
    const char *name;
};

static const struct sample_point k_samples[] = {
    {120, 100, 0xffff, "white"},
    {360, 100, 0xffe0, "yellow"},
    {600, 100, 0x07ff, "cyan"},
    {840, 100, 0x07e0, "green"},
    {1080, 100, 0xf81f, "magenta"},
    {1320, 100, 0xf800, "red"},
    {1560, 100, 0x001f, "blue"},
    {1800, 100, 0x0000, "black"},
};

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --info\n", prog);
    printf("  %s --validate <frame.rgb565>\n", prog);
    printf("  %s --once --output <frame.rgb565> [--device %s]\n", prog, COLORBAR_DEVICE_PATH);
}

static void print_info(void)
{
    printf("Colorbar RX constants:\n");
    printf("  resolution      : %ux%u\n", COLORBAR_WIDTH, COLORBAR_HEIGHT);
    printf("  format          : RGB565\n");
    printf("  frame_size      : %u bytes\n", COLORBAR_FRAME_SIZE);
    printf("  mark_size       : %u bytes\n", COLORBAR_MARK_SIZE);
    printf("  buffer_count    : %u\n", COLORBAR_BUFFER_COUNT);
    printf("  buffer_size     : %u bytes\n", COLORBAR_BUFFER_SIZE);
    printf("  default device  : %s\n", COLORBAR_DEVICE_PATH);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int validate_buffer(const uint8_t *data, size_t size)
{
    size_t i;
    int errors = 0;

    if (size < COLORBAR_FRAME_SIZE) {
        fprintf(stderr, "file too small: %zu bytes, need at least %u bytes\n",
                size, COLORBAR_FRAME_SIZE);
        return -1;
    }

    if (size != COLORBAR_FRAME_SIZE && size != COLORBAR_FRAME_SIZE + COLORBAR_MARK_SIZE) {
        printf("warning: file size is %zu bytes; expected %u or %u bytes\n",
               size, COLORBAR_FRAME_SIZE, COLORBAR_FRAME_SIZE + COLORBAR_MARK_SIZE);
    }

    for (i = 0; i < sizeof(k_samples) / sizeof(k_samples[0]); i++) {
        const struct sample_point *sample = &k_samples[i];
        size_t offset = ((size_t)sample->y * COLORBAR_WIDTH + sample->x) * COLORBAR_BYTES_PER_PIXEL;
        uint16_t actual = read_le16(data + offset);

        printf("sample %-8s at (%4u,%3u): actual=0x%04x expected=0x%04x %s\n",
               sample->name, sample->x, sample->y, actual, sample->expected,
               actual == sample->expected ? "OK" : "MISMATCH");

        if (actual != sample->expected) {
            errors++;
        }
    }

    if (errors) {
        fprintf(stderr, "validation failed: %d mismatched sample(s)\n", errors);
        return -1;
    }

    printf("validation passed\n");
    return 0;
}

static int validate_file(const char *path)
{
    int fd;
    struct stat st;
    uint8_t *data;
    int ret;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open input");
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }

    data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap input");
        close(fd);
        return -1;
    }

    ret = validate_buffer(data, (size_t)st.st_size);

    munmap(data, (size_t)st.st_size);
    close(fd);
    return ret;
}

static int write_all(int fd, const uint8_t *data, size_t len)
{
    while (len > 0) {
        ssize_t written = write(fd, data, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += written;
        len -= (size_t)written;
    }
    return 0;
}

static int capture_once(const char *device, const char *output)
{
    int dev_fd = -1;
    int out_fd = -1;
    uint8_t *buffers[COLORBAR_BUFFER_COUNT];
    struct colorbar_frame_info frame = {0};
    const uint8_t *frame_data;
    int ret = -1;
    unsigned int i;

    for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
        buffers[i] = MAP_FAILED;
    }

    dev_fd = open(device, O_RDWR);
    if (dev_fd < 0) {
        perror("open device");
        return -1;
    }

    if (ioctl(dev_fd, COLORBAR_IOC_ALLOC_BUFS) < 0) {
        perror("COLORBAR_IOC_ALLOC_BUFS");
        fprintf(stderr, "the currently loaded driver probably does not implement Colorbar RX ioctls yet\n");
        goto out;
    }

    for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
        off_t offset = (off_t)i * COLORBAR_BUFFER_SIZE;
        buffers[i] = mmap(NULL, COLORBAR_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, dev_fd, offset);
        if (buffers[i] == MAP_FAILED) {
            perror("mmap DMA buffer");
            goto out_unmap;
        }
    }

    if (ioctl(dev_fd, COLORBAR_IOC_START) < 0) {
        perror("COLORBAR_IOC_START");
        goto out_unmap;
    }

    if (ioctl(dev_fd, COLORBAR_IOC_WAIT_FRAME, &frame) < 0) {
        perror("COLORBAR_IOC_WAIT_FRAME");
        fprintf(stderr, "frame wait is not available; driver-side frame-ready support is required\n");
        goto out_stop;
    }

    if (frame.buffer_index >= COLORBAR_BUFFER_COUNT) {
        fprintf(stderr, "invalid frame buffer index: %u\n", frame.buffer_index);
        goto out_stop;
    }

    out_fd = open(output, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd < 0) {
        perror("open output");
        goto out_stop;
    }

    frame_data = buffers[frame.buffer_index];
    if (write_all(out_fd, frame_data, COLORBAR_FRAME_SIZE) < 0) {
        perror("write output");
        goto out_close_output;
    }

    printf("captured frame_counter=%u buffer=%u to %s\n",
           frame.frame_counter, frame.buffer_index, output);
    ret = validate_buffer(frame_data, COLORBAR_FRAME_SIZE);

out_close_output:
    if (out_fd >= 0) {
        close(out_fd);
    }
out_stop:
    if (ioctl(dev_fd, COLORBAR_IOC_STOP) < 0) {
        perror("COLORBAR_IOC_STOP");
    }
out_unmap:
    for (i = 0; i < COLORBAR_BUFFER_COUNT; i++) {
        if (buffers[i] != MAP_FAILED) {
            munmap(buffers[i], COLORBAR_BUFFER_SIZE);
        }
    }
    if (ioctl(dev_fd, COLORBAR_IOC_FREE_BUFS) < 0) {
        perror("COLORBAR_IOC_FREE_BUFS");
    }
out:
    close(dev_fd);
    return ret;
}

int main(int argc, char **argv)
{
    const char *device = COLORBAR_DEVICE_PATH;
    const char *validate_path = NULL;
    const char *output_path = NULL;
    bool show_info = false;
    bool once = false;
    int i;

    if (argc == 1) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--info")) {
            show_info = true;
        } else if (!strcmp(argv[i], "--validate") && i + 1 < argc) {
            validate_path = argv[++i];
        } else if (!strcmp(argv[i], "--once")) {
            once = true;
        } else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
            output_path = argv[++i];
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            device = argv[++i];
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (show_info) {
        print_info();
    }

    if (validate_path) {
        return validate_file(validate_path) == 0 ? 0 : 1;
    }

    if (once) {
        if (!output_path) {
            fprintf(stderr, "--once requires --output <frame.rgb565>\n");
            return 1;
        }
        return capture_once(device, output_path) == 0 ? 0 : 1;
    }

    return show_info ? 0 : 1;
}
