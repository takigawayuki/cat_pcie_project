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
    printf("  %s --safe-stop [--device %s]\n", prog, COLORBAR_DEVICE_PATH);
    printf("  %s --once --output <frame.rgb565> [--device %s]\n", prog, COLORBAR_DEVICE_PATH);
}

static void print_info(void)
{
    printf("Colorbar RX constants:\n");
    printf("  resolution      : %ux%u\n", COLORBAR_WIDTH, COLORBAR_HEIGHT);
    printf("  format          : RGB565\n");
    printf("  frame_size      : %u bytes\n", COLORBAR_FRAME_SIZE);
    printf("  valid_frame     : %u bytes\n", COLORBAR_DMA_MAX_BYTES);
    printf("  dma_guard       : %u bytes\n", COLORBAR_DMA_GUARD_SIZE);
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

    if (size != COLORBAR_FRAME_SIZE) {
        printf("warning: file size is %zu bytes; expected %u bytes\n",
               size, COLORBAR_FRAME_SIZE);
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

static void dump_prefix(const uint8_t *data, size_t size)
{
    size_t i;
    size_t limit = size < 64 ? size : 64;

    printf("first %zu byte(s):", limit);
    for (i = 0; i < limit; i++) {
        if ((i % 16) == 0) {
            printf("\n  %04zx:", i);
        }
        printf(" %02x", data[i]);
    }
    printf("\n");
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

static int safe_stop_device(const char *device)
{
    int dev_fd = open(device, O_RDWR);

    if (dev_fd < 0) {
        perror("open device");
        return -1;
    }

    if (ioctl(dev_fd, COLORBAR_IOC_SAFE_STOP) < 0) {
        perror("COLORBAR_IOC_SAFE_STOP");
        close(dev_fd);
        return -1;
    }

    close(dev_fd);
    printf("sent safe stop to %s\n", device);
    return 0;
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

static int read_full(int fd, uint8_t *data, size_t len)
{
    while (len > 0) {
        ssize_t got = read(fd, data, len);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (got == 0) {
            errno = EIO;
            return -1;
        }
        data += got;
        len -= (size_t)got;
    }
    return 0;
}

static int capture_once(const char *device, const char *output)
{
    int dev_fd = -1;
    int out_fd = -1;
    uint8_t *frame_data = NULL;
    struct colorbar_frame_info frame = {0};
    int ret = -1;

    dev_fd = open(device, O_RDWR);
    if (dev_fd < 0) {
        perror("open device");
        return -1;
    }

    if (ioctl(dev_fd, COLORBAR_IOC_ALLOC_BUFS) < 0) {
        perror("COLORBAR_IOC_ALLOC_BUFS");
        fprintf(stderr, "DMA buffer allocation failed; PCIE_DMA_single_1 needs one 4MiB coherent DMA buffer and dma_len_bytes=4147200\n");
        goto out;
    }

    if (ioctl(dev_fd, COLORBAR_IOC_START) < 0) {
        perror("COLORBAR_IOC_START");
        goto out_free;
    }

    if (ioctl(dev_fd, COLORBAR_IOC_WAIT_FRAME, &frame) < 0) {
        perror("COLORBAR_IOC_WAIT_FRAME");
        fprintf(stderr, "frame wait is not available; driver-side frame-ready support is required\n");
        goto out_stop;
    }

    if (frame.valid_size == 0 || frame.valid_size > COLORBAR_DMA_MAX_BYTES) {
        fprintf(stderr, "invalid frame valid_size: %u\n", frame.valid_size);
        goto out_stop;
    }

    frame_data = malloc(frame.valid_size);
    if (!frame_data) {
        perror("malloc frame buffer");
        goto out_stop;
    }

    if (lseek(dev_fd, 0, SEEK_SET) < 0) {
        perror("lseek device");
        goto out_stop;
    }

    if (read_full(dev_fd, frame_data, frame.valid_size) < 0) {
        perror("read frame from driver");
        goto out_stop;
    }

    out_fd = open(output, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd < 0) {
        perror("open output");
        goto out_stop;
    }

    if (write_all(out_fd, frame_data, frame.valid_size) < 0) {
        perror("write output");
        goto out_close_output;
    }

    printf("captured frame_counter=%u buffer=%u bytes=%u to %s\n",
           frame.frame_counter, frame.buffer_index, frame.valid_size, output);

    if (frame.valid_size >= COLORBAR_FRAME_SIZE) {
        ret = validate_buffer(frame_data, frame.valid_size);
    } else {
        dump_prefix(frame_data, frame.valid_size);
        printf("partial DMA capture saved; skip full-frame colorbar validation\n");
        ret = 0;
    }

out_close_output:
    if (out_fd >= 0) {
        close(out_fd);
    }
out_stop:
    if (ioctl(dev_fd, COLORBAR_IOC_STOP) < 0) {
        perror("COLORBAR_IOC_STOP");
    }
out_free:
    if (ioctl(dev_fd, COLORBAR_IOC_FREE_BUFS) < 0) {
        perror("COLORBAR_IOC_FREE_BUFS");
    }
out:
    free(frame_data);
    close(dev_fd);
    return ret;
}

int main(int argc, char **argv)
{
    const char *device = COLORBAR_DEVICE_PATH;
    const char *validate_path = NULL;
    const char *output_path = NULL;
    bool show_info = false;
    bool safe_stop = false;
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
        } else if (!strcmp(argv[i], "--safe-stop")) {
            safe_stop = true;
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

    if (safe_stop) {
        return safe_stop_device(device) == 0 ? 0 : 1;
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
