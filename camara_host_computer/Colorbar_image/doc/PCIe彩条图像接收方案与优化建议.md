# PCIe 彩条图像接收方案与优化建议

本文基于以下两份文档整理：

```text
PCIE_DMA_test_color_MES50HP_X1/doc/工程分析文档.md
pango_pcie_dma_alloc/doc/Linux端工程分析文档.md
```

目标是让 **鲁班猫2 RK3568 Linux 端** 通过 PCIe 接收 **FPGA 端生成的彩条测试图像**，并在 Linux 侧完成保存、校验或显示。

## 1. 当前目标

FPGA 端已经能生成 1080p 彩条，并通过 PCIe DMA 向主机内存写入图像数据。Linux 端现在的核心任务不是再做通用 DMA 回环测试，而是变成一个视频接收端：

```text
FPGA 彩条生成
  -> RGB565 打包
  -> PCIe MWR TLP
  -> RK3568 DMA buffer
  -> Linux 用户态 mmap 读取
  -> 校验 / 保存 / 显示
```

## 2. 从文档得到的关键事实

### 2.1 FPGA 端图像格式

```text
分辨率       1920 x 1080
像素格式     RGB565
单像素       2 bytes
单帧大小     1920 x 1080 x 2 = 4,147,200 bytes
彩条数量     8 条
单条宽度     1920 / 8 = 240 pixels
```

RGB565 的理论彩条值应为：

| 颜色 | RGB888 | RGB565 |
|---|---:|---:|
| 白 | 255,255,255 | 0xffff |
| 黄 | 255,255,0 | 0xffe0 |
| 青 | 0,255,255 | 0x07ff |
| 绿 | 0,255,0 | 0x07e0 |
| 品红 | 255,0,255 | 0xf81f |
| 红 | 255,0,0 | 0xf800 |
| 蓝 | 0,0,255 | 0x001f |
| 黑 | 0,0,0 | 0x0000 |

Linux 端第一阶段可以不急着显示，先把一帧保存成 raw 文件，再按这些 RGB565 值抽样校验。

### 2.2 FPGA 端 PCIe 行为

FPGA 端采用的是 **Endpoint 主动发起 MWR** 的模式：

```text
Linux 分配 DMA buffer
Linux 把 buffer 物理地址写入 FPGA BAR 寄存器
FPGA 自主发起 Memory Write Request
FPGA 把图像帧写入 Linux DMA buffer
```

因此 Linux 端不需要对每个包主动发起传输；Linux 端真正要做的是：

```text
1. 分配可被 FPGA 写入的 DMA buffer
2. 把 DMA 地址配置给 FPGA
3. 等待 FPGA 写完一帧
4. 从 DMA buffer 取出图像
```

### 2.3 FPGA 端寄存器

文档里给出的 FPGA 端关键寄存器为：

| BAR 偏移 | 作用 |
|---:|---|
| 0x110 | DMA 目标地址配置，FPGA 文档描述为分 4 次写入 |
| 0x130 | DMA 停止命令 |
| 0x140 | 亮部压缩参数 |
| 0x150 | 暗部提升参数 |
| 0x160 | 视频增强清除 |

这里要特别注意：当前 `pango_pcie_dma_alloc` 里的通用测试驱动使用的是：

```text
0x100  DMA 命令寄存器
0x110  DMA 地址低 32 位
0x120  DMA 地址高 32 位
```

这和彩条工程文档里的协议并不完全一致。后续实现前必须以 `PCIE_DMA_test_color_MES50HP_X1/source/pcie_dma_ctrl.v` 的真实代码为准，确认到底是：

```text
A. 只写 0x110，连续 4 次写入 4 个 32-bit 地址
B. 写 0x110/0x120 作为 64-bit 地址
C. 先写命令 0x100，再写地址
```

从已有文档看，更像是 **A：4 个 32-bit 目标地址循环**。

## 3. 现有 Linux 工程的问题

现有 `pango_pcie_dma_alloc` 更像一个 DMA 测试工具，不是视频接收程序。直接用它接 1080p 彩条会遇到几个问题。

### 3.1 单次用户态数据结构只有 4KB

当前头文件里有：

```c
#define DMA_MAX_PACKET_SIZE 4096
```

并且 `DMA_OPERATION` 内部的 `read_buf/write_buf` 是固定 4KB 数组。1080p RGB565 一帧是 4,147,200 bytes，远大于 4KB。

如果直接把 `current_len` 配成整帧大小，再走原来的 `PCI_READ_FROM_KERNEL_CMD`，会有结构体缓冲区溢出的风险。

结论：**不能用原 GUI 的 4KB read_buf 方式接整帧。**

### 3.2 当前驱动只按一个 DMA 地址工作

彩条 FPGA 文档描述 FPGA 使用 4 个 DMA 地址轮转，每帧切换地址页。现有驱动主要按一个 `addr_w` 写地址，这不适合连续视频流。

推荐改成：

```text
4 个 DMA frame buffer
每个 buffer 至少 4,147,200 bytes
Linux 把 4 个 buffer 的 DMA 地址依次写给 FPGA
FPGA 每帧写入一个 buffer，循环使用
```

### 3.3 32 位地址风险

FPGA 文档里的 MWR 是 3DW Memory Write，目标地址看起来是 32-bit 地址。如果 FPGA 只接收低 32 位地址，而 Linux 分配到的 DMA 地址超过 4GB，FPGA 会写错地址。

RK3568 平台上要重点确认：

```text
DMA 地址是否低于 0x1_0000_0000
FPGA 是否支持 64-bit MWR
驱动是否强制使用 32-bit DMA mask
```

如果 FPGA 端只有 32-bit 地址，Linux 驱动建议使用：

```c
dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
```

不要优先使用 64-bit DMA mask。

### 3.4 帧完成机制不明确

文档里提到可以轮询或中断等待帧完成，但 FPGA 寄存器表里没有明确的“帧完成/当前页/帧计数”状态寄存器。

如果没有帧完成标志，Linux 只能靠 `usleep()` 猜时间，这对于调试可以接受，但不适合稳定显示。

推荐 FPGA 增加状态寄存器：

| 偏移 | 建议字段 |
|---:|---|
| 0x170 | frame_counter |
| 0x174 | current_page |
| 0x178 | frame_done / fifo_overflow / dma_busy |
| 0x17c | frame_size_bytes |

更好的方式是 FPGA 每写完一帧触发 MSI/INTx 中断，Linux 驱动用 `wait_event_interruptible()` 等待。

## 4. 推荐总体方案

推荐新建一个独立的主机接收程序，不要直接把现有 GTK DMA 测试 GUI 改成视频显示程序。原因是 GUI 目前以 4KB DMA 测试为中心，强行改会混在一起。

推荐结构：

```text
camara_host_computer/
  doc/
    PCIe彩条图像接收方案与优化建议.md
  src/
    pcie_color_rx.c        # 后续新增：命令行接收/保存/显示程序
  include/
    pcie_color_rx.h        # 后续新增：帧格式和 ioctl 定义
  Makefile                 # 后续新增
```

驱动仍然可以基于 `pango_pcie_dma_alloc/driver` 改，但建议新增视频接收专用 ioctl，不要继续复用 `DMA_OPERATION` 里的 4KB 固定数组。

## 5. 驱动优化建议

### 5.1 使用帧级 DMA buffer

建议定义：

```c
#define PCIE_COLOR_WIDTH        1920
#define PCIE_COLOR_HEIGHT       1080
#define PCIE_COLOR_BPP          2
#define PCIE_COLOR_FRAME_SIZE   (PCIE_COLOR_WIDTH * PCIE_COLOR_HEIGHT * PCIE_COLOR_BPP)
#define PCIE_COLOR_BUF_COUNT    4
#define PCIE_COLOR_GUARD_SIZE   64
#define PCIE_COLOR_BUF_SIZE     PAGE_ALIGN(PCIE_COLOR_FRAME_SIZE + PCIE_COLOR_GUARD_SIZE)
```

这里加 64 字节 guard 是因为文档里有一个需要复核的点：

```text
4,147,200 / 64 = 64,800
```

但 FPGA 文档写的是：

```text
DMA_TRAN_TIMES = 64801
```

如果 FPGA 真的发 64,801 个 64B TLP，那会比一帧多写 64B。建议优先回到 FPGA 代码确认 `DMA_TRAN_TIMES` 是否应为 `64800`。在确认之前，Linux buffer 多留 64B 保护区更稳。

### 5.2 使用 dma_alloc_coherent / dma_mmap_coherent

内核 4.19 下建议优先使用 DMA API：

```c
void *cpu_addr = dma_alloc_coherent(&pdev->dev, size, &dma_handle, GFP_KERNEL);
```

用户态 mmap 时建议使用：

```c
dma_mmap_coherent(&pdev->dev, vma, cpu_addr, dma_handle, size);
```

不要自己随便 `remap_pfn_range()`，除非已经确认页属性和 cache 属性都正确。

### 5.3 4 个 buffer 不要求物理连续

FPGA 支持 4 个目标地址，因此 Linux 可以分配 4 块独立 coherent buffer：

```text
buffer0 -> dma_addr0
buffer1 -> dma_addr1
buffer2 -> dma_addr2
buffer3 -> dma_addr3
```

这比一次申请 16MB 连续 coherent 内存更容易成功。每个 buffer 只需要能容纳一帧。

### 5.4 新增视频接收 ioctl

建议新增专用 ioctl，和旧的 4KB DMA 测试接口分开：

| ioctl | 作用 |
|---|---|
| `PCIE_COLOR_ALLOC_BUFS` | 分配 4 个帧 DMA buffer |
| `PCIE_COLOR_FREE_BUFS` | 释放 buffer |
| `PCIE_COLOR_START` | 写入 4 个 DMA 地址并启动 FPGA 发送 |
| `PCIE_COLOR_STOP` | 写 0x130 停止 FPGA DMA |
| `PCIE_COLOR_WAIT_FRAME` | 等待一帧完成，返回 buffer index/frame counter |
| `PCIE_COLOR_GET_INFO` | 返回 width/height/format/frame_size/buf_count |

不要用 `DMA_OPERATION.data.read_buf[4096]` 传整帧。

### 5.5 BAR 选择需要复核

当前 `pango_pci_driver.c` 默认使用：

```c
._pci_bar = 1
```

但彩条 FPGA 工程里控制寄存器到底挂在哪个 BAR，需要实测确认。建议启动后打印每个 BAR 地址和大小，并用 PIO 方式读写一个无副作用寄存器确认。

如果寄存器实际在 BAR0，而驱动映射 BAR1，Linux 写寄存器会“看起来成功”，但 FPGA 不会响应。

## 6. 用户态程序优化建议

### 6.1 第一版先做 CLI，不做 GUI

建议第一版 `camara_host_computer` 只做命令行工具：

```text
1. 打开 /dev/pango_pci_driver
2. ioctl 分配 4 个 frame buffer
3. mmap 映射 DMA buffer
4. ioctl 启动 FPGA DMA
5. 等一帧
6. 保存 frame0.rgb565
7. 抽样校验彩条像素
8. 可选写入 /dev/fb0 显示
```

先把“数据正确”跑通，再考虑 GTK/Qt 界面。

### 6.2 保存 raw 文件用于验证

第一阶段建议输出：

```text
frame_0000.rgb565
```

然后用脚本或工具转换成 PNG/PPM。这样可以把 PCIe 接收问题和显示问题分开。

### 6.3 RGB565 抽样校验

每条彩条中间位置抽样，例如第 100 行：

| x 坐标 | 期望颜色 |
|---:|---|
| 120 | 白 0xffff |
| 360 | 黄 0xffe0 |
| 600 | 青 0x07ff |
| 840 | 绿 0x07e0 |
| 1080 | 品红 0xf81f |
| 1320 | 红 0xf800 |
| 1560 | 蓝 0x001f |
| 1800 | 黑 0x0000 |

如果颜色反了或错位，优先检查：

```text
16-bit 小端/大端解释
RGB565 位域是否按 R[15:11] G[10:5] B[4:0]
每行 stride 是否正好 1920*2
FPGA 是否多发/少发 TLP
```

### 6.4 显示建议

第一版显示可以用 `/dev/fb0`，但必须处理这些问题：

```text
fb0 分辨率可能不是 1920x1080
fb0 bits_per_pixel 可能是 16/24/32
fb0 line_length 可能大于 xres * bytes_per_pixel
32bpp 常见格式可能是 BGRA/XRGB，不一定是 RGB 顺序
```

如果 fb0 是 16bpp 且分辨率 1920x1080，可以直接 memcpy RGB565。

如果 fb0 是 32bpp，需要 RGB565 转 XRGB8888，并按 `finfo.line_length` 逐行写。

后续更推荐 DRM dumb buffer 或 SDL2 显示，但第一版用 fbdev 更容易验证。

## 7. 推荐开发路线

### 阶段 0：确认链路和 BAR

```sh
lspci -nn
lspci -vv -s 01:00.0
dmesg | grep -i pcie
```

确认：

```text
PCIe Link up
0755:0755 已枚举
pango_pci_driver probe 成功
BAR0/BAR1/BAR2 地址和大小正常
```

### 阶段 1：确认 FPGA 寄存器协议

必须从 `pcie_dma_ctrl.v` 或实测确认：

```text
4 个 DMA 地址如何写入
是否只有低 32 位地址
是否有 start/stop/status 寄存器
寄存器在哪个 BAR
每帧到底发 64800 还是 64801 个 64B 包
```

这个阶段决定驱动怎么写，不能跳过。

### 阶段 2：驱动支持 4 个 frame buffer

实现：

```text
4 x dma_alloc_coherent
强制 32-bit DMA mask，除非 FPGA 已确认支持 64-bit
mmap 暴露给用户态
start/stop/wait_frame ioctl
```

### 阶段 3：命令行接收一帧并保存

实现最小工具：

```sh
sudo ./pcie_color_rx --once --output frame.rgb565
```

验证：

```text
文件大小 = 4,147,200 bytes
抽样点颜色符合 8 色彩条
连续多帧 frame counter 正常增加
```

### 阶段 4：显示到 RK3568 屏幕

先 fbdev：

```sh
sudo ./pcie_color_rx --display fb0
```

跑通后再考虑 DRM/SDL/Qt。

### 阶段 5：连续帧和性能优化

优化方向：

```text
中断替代 usleep 轮询
4 buffer 防止读写冲突
统计 dropped frame / overflow
降低 CPU 颜色转换开销
必要时启用 NEON 或 RGA/GPU
```

## 8. 需要优先修正或复核的问题

### 8.1 文档里的带宽单位需要修正

FPGA 文档写：

```text
1920 * 1080 * 3 * 60 = 373.248 Mbps
```

这个单位不对。如果按 RGB888：

```text
1920 * 1080 * 3 bytes * 60 = 373.248 MB/s
约 2.986 Gbps
```

而当前 PCIe 传输是 RGB565：

```text
1920 * 1080 * 2 bytes * 60 = 248.832 MB/s
约 1.991 Gbps
```

PCIe Gen2 x1 理论 500 MB/s，实际约 350~400 MB/s，传 RGB565 1080p60 理论上够，但余量不算特别大。

### 8.2 DMA_TRAN_TIMES 需要复核

一帧 RGB565 1080p 的 64B 包数量应为：

```text
4,147,200 / 64 = 64,800
```

文档记录 `DMA_TRAN_TIMES = 64801`。这可能是 off-by-one，也可能是 FPGA 额外发了一个包。必须回到代码或抓取一帧验证。

### 8.3 现有 Linux 端方向描述容易混淆

现有 GUI 里的按钮名：

```text
DMA Read  = FPGA 从主机读
DMA Write = FPGA 写主机
```

对彩条接收来说，真正需要的是：

```text
FPGA -> Linux，也就是 FPGA Memory Write 到主机 DMA buffer
```

文档、代码和 UI 里建议统一命名为：

```text
host_rx / fpga_mwr / video_rx
```

不要再用 `read/write` 这种容易反的词作为核心接口名。

## 9. 建议的新工程职责

`camara_host_computer` 建议定位为 Linux 端图像接收工程：

```text
只负责：
- 打开 PCIe 视频接收驱动
- 配置 DMA frame buffer
- mmap 读取帧
- 校验彩条
- 保存 raw/ppm/png
- 显示到屏幕

不负责：
- FPGA bitstream 生成
- 通用 DMA 回环测试
- Tandem 加载
- PCIe 链路 bring-up 文档
```

这样它和 `pango_pcie_dma_alloc` 的职责就分开了：

| 工程 | 职责 |
|---|---|
| `pango_pcie_dma_alloc` | PCIe 链路、驱动、DMA 基础测试 |
| `PCIE_DMA_test_color_MES50HP_X1` | FPGA 彩条源和 PCIe MWR 发送 |
| `camara_host_computer` | RK3568 接收、校验、显示图像 |

## 10. 当前最推荐的下一步

不要立刻写 GUI。建议按下面顺序推进：

```text
1. 回到 FPGA pcie_dma_ctrl.v，确认 0x110 地址写入协议和 DMA_TRAN_TIMES
2. 在 Linux 驱动里新增 video_rx 专用 4 buffer + mmap 接口
3. 在 camara_host_computer 里写 CLI 接收程序
4. 先保存一帧 frame.rgb565
5. 抽样校验 8 色彩条
6. 再做 fb0/DRM 显示
7. 最后做连续帧和性能优化
```

这条路线能把问题拆开：

```text
PCIe 链路问题
DMA 地址问题
帧数据正确性问题
显示格式问题
性能问题
```

每一层都能单独验证，不容易一上来卡在“屏幕没显示，不知道哪里错”。
