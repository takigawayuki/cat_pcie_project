# Linux 端负责人实施路线

本文面向 Linux/RK3568 侧开发。当前目标是：在鲁班猫2 RK3568 上，通过 PCIe 接收 FPGA 工程 `PCIE_DMA_test_color_MES50HP_X1` 发送的彩条图像。

## 1. 你的职责边界

你主要负责 Linux 端，所以可以把系统拆成三层：

| 层 | 负责方 | 当前状态 |
|---|---|---|
| FPGA 彩条发送 | FPGA 端 | `PCIE_DMA_test_color_MES50HP_X1` 已有代码 |
| PCIe 链路和基础 DMA 验证 | Linux 端已有基础 | `pango_pcie_dma_alloc` 已验证能通信和 DMA 读写 |
| 图像接收、校验、显示 | 你现在要做 | 新建 `camara_host_computer` |

`pango_pcie_dma_alloc` 的价值是证明：

```text
PCIe link up
Linux 能枚举 FPGA
驱动能 probe
BAR 能访问
DMA 基础路径能跑通
```

但它还不是一个视频接收工程，因为它的用户态 DMA 数据结构只有 4KB，不适合直接收 1080p 整帧图像。

## 2. 已经从 FPGA 源码确认的协议

我读了：

```text
PCIE_DMA_test_color_MES50HP_X1/source/pcie_dma_ctrl.v
```

确认到几个关键点。

### 2.1 FPGA 接收 Linux 配置地址的方式

FPGA 端监听 Root Complex 发来的 MWR_32 TLP，寄存器偏移是：

```verilog
localparam DMA_CMD_L_ADDR = 12'h110;
```

当 Linux 向 BAR 偏移 `0x110` 写 1 DWORD 数据时，FPGA 依次保存为：

```text
第 1 次写 0x110 -> dma_addr0
第 2 次写 0x110 -> dma_addr1
第 3 次写 0x110 -> dma_addr2
第 4 次写 0x110 -> dma_addr3
```

也就是说，彩条工程不是现有测试驱动的这种协议：

```text
0x100 写命令
0x110 写地址低位
0x120 写地址高位
```

而更像是：

```text
连续 4 次向 0x110 写入 32-bit 主机 DMA 地址
FPGA 收齐 4 个地址后开始等待视频帧并主动 MWR 写主机内存
```

### 2.2 FPGA 只使用 32-bit 目标地址

源码里 DMA 地址寄存器是：

```verilog
reg [31:0] dma_addr0;
reg [31:0] dma_addr1;
reg [31:0] dma_addr2;
reg [31:0] dma_addr3;
```

MWR TLP 也是：

```verilog
r_axis_s_tdata[31:29] <= 3'b010; // 3DW MWr
r_axis_s_tdata[95:64] <= alloc_addrl;
```

这意味着 Linux 端必须重点保证 DMA 地址在 32-bit 地址范围内。驱动里不要优先使用 64-bit DMA mask，建议视频接收模式强制：

```c
dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
```

否则如果内核分配出高于 4GB 的 DMA 地址，FPGA 只拿低 32 位，会写错内存。

### 2.3 FPGA 使用 4 个地址循环写帧

源码中每次 VS 边沿切换 `addr_page`：

```text
addr_page 0 -> dma_addr0
addr_page 1 -> dma_addr1
addr_page 2 -> dma_addr2
addr_page 3 -> dma_addr3
```

因此 Linux 端应该分配 4 个帧 buffer，而不是 1 个 buffer。

### 2.4 图像格式和大小

```text
分辨率       1920 x 1080
格式         RGB565
单帧大小     4,147,200 bytes
每个 TLP     64 bytes
理论包数     64,800 个 64B 包
源码参数     DMA_TRAN_TIMES = 64801
```

`64801` 比理论 `64800` 多 1 包。FPGA 源码最后一包写的是 `{16{fram_cnt}}`，更像是帧计数标记包。所以 Linux buffer 最好至少预留：

```text
4,147,200 + 64 bytes
```

## 3. camara_host_computer 应该怎么定位

建议 `camara_host_computer` 不做通用 PCIe 测试，也不复制旧 GUI。

它只做一件事：

```text
从 /dev/pango_pci_driver 或后续视频专用驱动接口拿到 FPGA 写入的 RGB565 帧，完成保存、校验和显示。
```

推荐目录：

```text
camara_host_computer/
  doc/
    PCIe彩条图像接收方案与优化建议.md
    Linux端负责人实施路线.md
  include/
    pcie_color_rx.h
  src/
    pcie_color_rx.c
  Makefile
```

第一版先做命令行程序，不做 GUI。

## 4. 最小可行版本 MVP

第一版目标不要直接“实时显示视频”，而是先接收并验证一帧。

### 4.1 MVP 输入

```text
PCIe Endpoint 已枚举
pango_pci_driver.ko 已加载
/dev/pango_pci_driver 存在
FPGA 彩条 bitstream 已运行
```

### 4.2 MVP 输出

```text
frame_0000.rgb565
```

要求：

```text
文件大小 = 4,147,200 bytes
或者 = 4,147,264 bytes，最后 64 bytes 是 frame counter 标记包
抽样点颜色符合 8 色彩条
```

### 4.3 MVP 程序流程

```text
1. open /dev/pango_pci_driver
2. ioctl 分配 4 个 DMA coherent frame buffer
3. mmap 映射 4 个 buffer 到用户态
4. ioctl 把 4 个 DMA 地址依次写到 FPGA 0x110
5. 等待一帧时间，或等待驱动返回 frame ready
6. 从某个 buffer 保存 4,147,200 bytes 到 frame_0000.rgb565
7. 抽样校验 8 色彩条
8. ioctl 写 0x130 停止 FPGA DMA
9. 释放 buffer，退出
```

## 5. 驱动应该怎么改

建议不要在现有 `DMA_OPERATION` 上硬扩展，因为它带固定 4KB 数组。应该新增视频接收专用接口。

### 5.1 新增专用数据结构

建议在驱动头文件里新增类似结构：

```c
#define PCIE_COLOR_WIDTH        1920
#define PCIE_COLOR_HEIGHT       1080
#define PCIE_COLOR_BPP          2
#define PCIE_COLOR_FRAME_SIZE   (PCIE_COLOR_WIDTH * PCIE_COLOR_HEIGHT * PCIE_COLOR_BPP)
#define PCIE_COLOR_MARK_SIZE    64
#define PCIE_COLOR_BUF_COUNT    4
#define PCIE_COLOR_BUF_SIZE     PAGE_ALIGN(PCIE_COLOR_FRAME_SIZE + PCIE_COLOR_MARK_SIZE)

struct pcie_color_buf_info {
    __u32 width;
    __u32 height;
    __u32 format;      // RGB565
    __u32 buf_count;   // 4
    __u32 frame_size;  // 4147200
    __u32 buf_size;    // PAGE_ALIGN(frame_size + 64)
};
```

### 5.2 新增 ioctl

建议新增：

```text
PCIE_COLOR_GET_INFO     获取图像格式和 buffer 信息
PCIE_COLOR_ALLOC_BUFS   分配 4 个 coherent buffer
PCIE_COLOR_START        连续 4 次向 BAR+0x110 写入 DMA 地址
PCIE_COLOR_STOP         向 BAR+0x130 写入停止命令
PCIE_COLOR_WAIT_FRAME   等待一帧完成，第一版可先返回当前估算页
PCIE_COLOR_FREE_BUFS    释放 buffer
```

不要用旧的：

```text
PCI_READ_FROM_KERNEL_CMD
DMA_OPERATION.data.read_buf[4096]
```

来读整帧。

### 5.3 mmap

驱动需要支持 mmap，让用户态直接访问 DMA buffer：

```text
用户态 mmap 长度 = 4 * PCIE_COLOR_BUF_SIZE
buffer0 偏移 = 0 * PCIE_COLOR_BUF_SIZE
buffer1 偏移 = 1 * PCIE_COLOR_BUF_SIZE
buffer2 偏移 = 2 * PCIE_COLOR_BUF_SIZE
buffer3 偏移 = 3 * PCIE_COLOR_BUF_SIZE
```

内核侧建议使用：

```c
dma_alloc_coherent()
dma_mmap_coherent()
```

这样比 copy_to_user/copy_from_user 整帧拷贝更适合视频流。

## 6. Linux 端你需要向 FPGA 端确认的问题

你不需要懂所有 FPGA 细节，但这几个协议点必须拿到确定答案：

```text
1. 控制寄存器在哪个 BAR？BAR0 还是 BAR1？
2. 0x110 是否就是连续写 4 次 32-bit DMA 地址？
3. 地址写入是否必须小端字节序？当前 FPGA 代码看起来做了字节反转。
4. FPGA 是否只支持 32-bit MWR 地址？目前源码看起来是。
5. 0x130 写任意值是否停止 DMA？停止后是否清空 4 个地址？
6. DMA_TRAN_TIMES = 64801 是否意味着每帧最后多 64B frame counter？
7. 是否有状态寄存器能读 frame_counter/current_page/frame_done/fifo_overflow？
8. 如果没有状态寄存器，是否能加一个？
9. 是否支持 MSI/INTx 中断通知帧完成？
```

这些问题不确认，Linux 端也能先做“等待固定时间后保存 buffer”，但后续稳定显示会比较困难。

## 7. 推荐实现顺序

### 阶段 1：只复用 pango_pcie_dma_alloc 的基础能力

确认：

```sh
lspci -nn
lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver
dmesg | tail -n 100
```

目标：确认 Linux/PCIe 基础仍然正常。

### 阶段 2：驱动新增最小 video_rx 接口

只做这些：

```text
分配 4 个 buffer
打印 4 个 dma_addr
连续写 4 次 BAR+0x110
mmap 暴露 buffer
写 BAR+0x130 停止
```

先不要做复杂中断。

### 阶段 3：camara_host_computer 保存一帧

命令示例：

```sh
sudo ./pcie_color_rx --once --output frame_0000.rgb565
```

程序内部：

```text
start DMA
sleep 100ms
保存 buffer0 或当前页 buffer
stop DMA
```

这一步即使没有 frame_done 寄存器也能先验证方向。

### 阶段 4：抽样校验彩条

抽样点：

| x | y | 期望 RGB565 |
|---:|---:|---:|
| 120 | 100 | 0xffff |
| 360 | 100 | 0xffe0 |
| 600 | 100 | 0x07ff |
| 840 | 100 | 0x07e0 |
| 1080 | 100 | 0xf81f |
| 1320 | 100 | 0xf800 |
| 1560 | 100 | 0x001f |
| 1800 | 100 | 0x0000 |

如果颜色不对，按顺序查：

```text
buffer 选错页
大小端解释错
RGB565 位域错
每帧多 64B 导致偏移错
FPGA 写地址错误
BAR 选错
DMA 地址超过 32-bit
```

### 阶段 5：再做显示

确认 raw 数据正确后，再做：

```text
fb0 显示
DRM dumb buffer
SDL/Qt GUI
```

不要一开始就做显示，否则屏幕不对时不知道是 DMA 错还是显示格式错。

## 8. 你现在最应该做的 3 件事

```text
1. 在驱动中增加 video_rx 专用 4 buffer + mmap 接口
2. 在 camara_host_computer 写 pcie_color_rx 命令行工具，先保存一帧 raw
3. 向 FPGA 端确认 BAR、0x110、0x130、64801 最后一包、状态寄存器这几个协议点
```

做到这三件事后，Linux 端就从“能通信”推进到“能接收图像帧”。

## 9. 不建议现在做的事

```text
不要直接改旧 GTK GUI 来显示视频
不要用 4KB DMA_OPERATION 接整帧
不要还没保存 raw 成功就调屏幕显示
不要默认 DMA 地址一定低于 4GB
不要默认 64801 一定等于一帧有效像素
不要在不知道 BAR 的情况下乱写寄存器
```

## 10. 一句话结论

`pango_pcie_dma_alloc` 已经证明 PCIe 和基础 DMA 通了；`camara_host_computer` 应该作为新的 Linux 图像接收工程，第一步不是做漂亮界面，而是做 **4 个 4MB 级 DMA buffer + mmap + 保存 RGB565 raw + 抽样校验彩条**。
