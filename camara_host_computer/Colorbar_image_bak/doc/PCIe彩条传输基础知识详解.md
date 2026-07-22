# PCIe 彩条传输基础知识详解

本文面向初次接触 PCIe + FPGA + Linux 联合调试的开发者，以"FPGA 通过 PCIe 向 RK3568 Linux 传输 1080p 彩条图像"这个具体工程为例，结合实际的 FPGA Verilog 代码、Linux 驱动代码和 PCIe 协议基础，解释每个关键概念。

读完本文你将理解：数据从 FPGA 的 HDMI 输入口到 Linux 的 `frame_0000.rgb565` 文件，中间到底发生了什么。

---

## 1. 整体架构：谁是谁

### 1.1 物理拓扑

```
┌─────────────────────┐         ┌─────────────────────┐
│   RK3568 (鲁班猫2)   │         │  Pango MES50HP (FPGA) │
│                      │         │                      │
│  PCIe Root Complex   │◄──PCIe──┤  PCIe Endpoint       │
│  (RC，根复合体)       │  Gen2 x1 │  (EP，端点)           │
│                      │         │                      │
│  ARM Cortex-A55 ×4   │         │  彩条生成器           │
│  Linux 5.10/6.x      │         │  DMA 控制器           │
│                      │         │  HDMI RX + TX        │
└─────────────────────┘         └─────────────────────┘
```

**关键角色区分**：

| 角色 | 谁扮演 | 职责 |
|------|--------|------|
| **Root Complex (RC)** | RK3568 | PCIe 总线的"根"：枚举设备、分配 BAR 地址、发起配置读写 |
| **Endpoint (EP)** | FPGA | PCIe 总线的"叶子"：被 RC 发现和配置，可以发起 DMA（MWr TLP） |
| **Bus Master** | FPGA | 主动向主机内存写数据的设备。`pci_set_master(pdev)` 使能 |
| **Host Memory** | RK3568 DDR | FPGA 通过 PCIe MWR 写数据的目标 |

### 1.2 数据流向

```
FPGA 侧                                Linux 侧
───────                                ────────
HDMI 输入 (外部信号源)
  │
  ▼
vs_in/hs_in/de_in/r_in/g_in/b_in      (时序信号提取)
  │
  ▼
sync_vg.v → pattern_vg.v              (生成 1920×1080 彩条图案)
  │
  ▼
RGB888 (24bit) → RGB565 (16bit)       (像素格式转换)
  │
  ▼
hdmi_pcie_fifo (跨时钟域 FIFO)        (148.5MHz → 125MHz)
  │
  ▼
pcie_dma_ctrl.v                       (组装 PCIe TLP)
  │
  ▼
Pango PCIe IP Core                    (PHY → 串行链路)
  │                                       │
  └──────── PCIe Gen2 x1 ────────────────┘
                                          │
                                          ▼
                                    RK3568 PCIe RC
                                          │
                                          ▼
                                    colorbar_pcie_driver.ko
                                    (dma_alloc_coherent 分配的 buffer)
                                          │
                                          ▼
                                    /dev/colorbar_pcie_rx
                                    (mmap → 用户态读取)
                                          │
                                          ▼
                                    pcie_color_rx (用户态程序)
                                          │
                                          ▼
                                    frame_0000.rgb565
                                    (4,147,200 bytes 原始图像)
```

### 1.3 为什么是 FPGA 主动发数据，不是 Linux 主动读？

这是本项目与普通"Linux 用 `read()` 读外设"最大的区别：

```
传统方式（如 UART/SPI）：          本项目方式（PCIe EP DMA）：
┌──────┐  请求  ┌──────┐          ┌──────┐  直接写  ┌──────┐
│Linux │───────►│ FPGA │          │Linux │◄────────│ FPGA │
│(主)  │◄───────│(从)  │          │(被动)│  MWR TLP │(主动) │
└──────┘  数据  └──────┘          └──────┘          └──────┘
```

FPGA 是 **Bus Master**，它决定"什么时候写、写到哪个地址、写多少数据"。Linux 的角色是**提前告诉 FPGA 可以写到哪里**（提供 DMA 地址），然后**等待** FPGA 写完。

这就像：你给快递员你的收货地址（DMA buffer 地址），快递员自己把包裹送到你家门口（MWr TLP），而不是你去快递站取。

---

## 2. PCIe 协议基础（五层模型速览）

PCIe 协议分五层，但实际调试中你只需要关注这三层：

```
┌──────────────────────┐
│  Transaction Layer   │  ← TLP (Transaction Layer Packet)：读/写/配置请求
│  (事务层)             │     这是分析 DMA 问题的核心层
├──────────────────────┤
│  Data Link Layer     │  ← DLLP：ACK/NAK、流控、CRC 校验
│  (数据链路层)         │     通常由 PCIe IP Core 自动处理
├──────────────────────┤
│  Physical Layer      │  ← 串行/解串、8b/10b 编码、链路训练 (LTSSM)
│  (物理层)             │     lspci 看到的 Link up/down 就是这一层
└──────────────────────┘
```

### 2.1 关键概念：LTSSM（链路训练与状态机）

`dmesg` 中看到的日志：

```
rk-pcie 3c0000000.pcie: PCIe Linking... LTSSM is 0x3
rk-pcie 3c0000000.pcie: PCIe Link up, LTSSM is 0x30011
```

LTSSM 状态机决定了 PCIe 链路是否能通信：

| LTSSM 状态 | 含义 |
|------------|------|
| 0x0 (Detect) | 检测对端是否存在 |
| 0x1 (Polling) | 确定链路速度 |
| 0x2 (Configuration) | 确定链路宽度 |
| 0x3 (L0) | **正常工作状态**，可以发 TLP |
| 0x30011 | RK3568 dmesg 的特殊编码：L0 + Gen2 + x1 |

**调试要点**：如果看不到 `Link up`，问题在物理层——检查 REFCLK、PERST#、转接板连接。此时 TLP 根本发不出去，不是驱动问题。

---

## 3. BAR（基地址寄存器）—— 最重要也是最容易搞错的概念

### 3.1 什么是 BAR？

BAR 是 PCIe 配置空间中的一组寄存器，**由 Endpoint 告诉 RC"我需要多大的一块内存映射空间"**，**由 RC 在枚举时分配实际物理地址**。

```
FPGA EP 的配置空间 (Type 0)：
┌──────────────────────────┐
│ Vendor ID  │ Device ID   │  ← 0x0755 : 0x0755
├──────────────────────────┤
│ Command    │ Status      │
├──────────────────────────┤
│ ...                      │
├──────────────────────────┤
│ BAR0: 请求 8KB, 32-bit   │  ← FPGA 说的："我需要 8KB 空间"
│       实际映射 0xf4200000 │  ← RC 回复的："好，给你这块地址"
├──────────────────────────┤
│ BAR1: 请求 4KB, 32-bit   │
│       实际映射 0xf4204000 │
├──────────────────────────┤
│ BAR2: 请求 8KB, 64-bit   │
│       实际映射 0xf4202000 │
├──────────────────────────┤
│ ...                      │
└──────────────────────────┘
```

### 3.2 用 lspci 看 BAR

```sh
# 查看 BAR 分配结果
lspci -vv -s 01:00.0

# 或者
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

本项目的实际输出：

```
Region 0: Memory at f4200000 (32-bit, non-prefetchable) [size=8K]
Region 1: Memory at f4204000 (32-bit, prefetchable)     [size=4K]
Region 2: Memory at f4202000 (64-bit, prefetchable)     [size=8K]
```

### 3.3 为什么 BAR 选错会导致系统崩溃？

这是本项目最重要的教训。FPGA 内部的控制寄存器（DMA 地址寄存器、STOP 寄存器等）挂在**某一个 BAR** 的偏移地址上。

```
FPGA 内部地址空间（以 BAR1 为例）：
┌─────────────────────┐
│ BAR1 Base           │  ← 0xf4204000 (RC 分配)
│  +0x000: (其他)     │
│  +0x100: (其他)     │
│  +0x110: DMA_ADDR   │  ← Linux 写 4 次配置 DMA 目标地址
│  +0x130: DMA_STOP   │  ← Linux 写 1 次停止 DMA
│  +0x140: 视频增强   │
│  +0x150: 视频增强   │
│  +0x160: 视频增强   │
└─────────────────────┘
```

**Linux 通过 `pci_iomap(pdev, bar_index, 0)` 映射 BAR，然后用 `iowrite32(value, bar + offset)` 写寄存器。**

- 如果 `bar_index` 对了（比如 BAR1），`iowrite32(0, bar + 0x130)` → FPGA 收到 STOP → DMA 停止 → 安全
- 如果 `bar_index` 错了（写成 BAR0），`iowrite32(0, bar + 0x130)` → 写到 FPGA 的 BAR0 地址空间 → **FPGA 的控制逻辑根本没看到这个写操作** → DMA 不停 → 写坏内存 → 系统崩溃

**类比**：你给快递员写错了门牌号。你以为你在写"101 室"的地址，实际上你写的是"102 室"。真正的收货地址（DMA buffer）可能是对的，但 STOP 指令送到了错误的门牌号，FPGA 的控制电路根本没收到。

### 3.4 3 个 BAR 分别干什么？

从 Pango PCIe IP 代码 `ipsl_pcie_dma_rx_top.v` 分析：

| BAR | 作用（Pango 参考设计） |
|-----|----------------------|
| BAR0 | DMA 数据 RAM（FPGA 内部 256×128bit 存储），主机可以直接 PIO 读写 |
| BAR1 | **控制寄存器**（0x110 DMA地址、0x130 STOP 等），我们最关心的就是这个 |
| BAR2 | CPLD（完成包）DMA 数据 RAM |

**当前 FPGA 的控制寄存器大概率在 BAR1**（符合 Pango 参考设计惯例），但需要通过 ILA/SignalTap 或 ADDR_ECHO 寄存器最终确认。

---

## 4. DMA（直接内存访问）—— FPGA 如何"直接写"Linux 内存

### 4.1 PCIe 的 DMA 不是传统 DMA

传统 SoC 的 DMA（如 RK3568 的 DMA 引擎）：CPU 配置源地址→目标地址→长度，DMA 控制器搬数据。

**PCIe 的 DMA 是另一种模式**：Endpoint 设备作为 Bus Master，直接向 Root Complex 的内存空间发 Memory Write Request。不需要主机 CPU 参与数据传输。

```verilog
// pcie_dma_ctrl.v:322 — FPGA 把 Linux 给的物理地址直接填进 TLP Header
r_axis_s_tdata[95 : 64] <= alloc_addrl;  // MWR TLP 的目标地址
```

### 4.2 Linux 端分配 DMA Buffer

驱动中分配可以被 FPGA 写入的物理内存：

```c
// driver/colorbar_pcie_driver.c:85-87
cdev->bufs[i].cpu_addr = dma_alloc_coherent(
    &cdev->pdev->dev,           // PCI 设备
    COLORBAR_BUFFER_SIZE,       // 大小：4,149,248 bytes
    &cdev->bufs[i].dma_addr,    // 输出：物理地址（给 FPGA 用）
    GFP_KERNEL);
```

**关键区别**：

| 指针 | 类型 | 谁来用 |
|------|------|--------|
| `cpu_addr` (void *) | 内核虚拟地址 | Linux 内核（memset、mmap） |
| `dma_addr` (dma_addr_t) | **物理地址**（总线地址） | FPGA（填进 MWR TLP Header） |

**FPGA 只能用 `dma_addr`！** 如果把 `cpu_addr`（内核虚拟地址）写给 FPGA，FPGA 会向一个毫无意义的物理地址发 MWR —— 又是一次系统崩溃。

### 4.3 为什么用 4 个 Buffer（不是 1 个）？

```
Buffer 0 ──── FPGA 写第 0 帧 ──── Linux 读第 0 帧
Buffer 1 ──── FPGA 写第 1 帧 ──── Linux 读第 1 帧
Buffer 2 ──── FPGA 写第 2 帧 ──── Linux 读第 2 帧
Buffer 3 ──── FPGA 写第 3 帧 ──── Linux 读第 3 帧
     └── 循环 ──┘
```

4 个 buffer 是为了 **"乒乓"缓冲**：FPGA 写 buffer N 的时候，Linux 可以同时读 buffer N-1（上一帧），避免读写冲突导致图像撕裂。

即使当前只取一帧（`--once`），4 个 buffer 也是必须的——因为 FPGA 代码要求 4 个地址全部非零才允许 DMA 启动（`pcie_dma_ctrl.v:221`）。

### 4.4 32-bit DMA 地址限制

```c
// driver/colorbar_pcie_driver.c:321
dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
```

FPGA 的 DMA 地址寄存器是 32-bit（`pcie_dma_ctrl.v:77-80`），只能表示 0x00000000 ~ 0xFFFFFFFF（4GB 以内）。

RK3568 有 4GB 或更多 DDR，`dma_alloc_coherent` 可能分配到超过 4GB 的地址。如果不限制 32-bit mask，FPGA 拿到的高 32 位会被截断，实际写地址变成错误的低 32 位。

### 4.5 dma_alloc_coherent vs kmalloc + dma_map

| 方法 | 一致性 | 适用场景 |
|------|--------|---------|
| `dma_alloc_coherent` | 保证 CPU 和 Device 看到的数据一致（无需 cache flush） | **本项目使用**。适合长期占用的小 buffer（~4MB × 4） |
| `kmalloc` + `dma_map_single` | 需要手动 sync（`dma_sync_single_for_device/cpu`） | 适合临时、大量、流式 buffer |

`dma_alloc_coherent` 的代价是在内核的 coherent pool 中分配，数量有限。4 × 4MB = 16MB 在 RK3568 上通常没问题，但如果将来要分配 32 个 buffer，需要注意 coherent pool 大小限制。

---

## 5. TLP（事务层包）—— 数据在 PCIe 链路上长什么样

### 5.1 TLP 类型

本项目只用到了 **MWr (Memory Write Request)**，即 FPGA 向主机内存写数据。PCIe 协议中常见的 TLP 类型：

| TLP 类型 | 方向 | 用途 | 本项目 |
|----------|------|------|--------|
| **MWr (Memory Write)** | EP → RC | 写主机内存 | ✅ FPGA 发图像数据 |
| MRd (Memory Read) | EP → RC | 读主机内存 | ❌ 本项目不用 |
| CfgRd/CfgWr | RC → EP | 读写 EP 配置空间 | ❌ 系统自动 |
| CplD (Completion with Data) | RC → EP | MRd 的返回数据 | ❌ 本项目不用 |
| Msg (Message) | 双向 | 中断、错误、PM 等 | ❌ 本项目不用 |

### 5.2 MWR TLP 的二进制结构（3DW Header）

FPGA 每次发 64 bytes 数据，使用的 TLP 格式是 **3DW Header + 16DW Data = 19DW = 128 bits on AXI-S**。

```
DW0 (Byte 0-3):
  [31:29] Fmt    = 010 (3DW header, with data)
  [28:24] Type   = 00000 (Memory Write Request)
  [9:0]   Length = 16 (数据载荷：16 DW = 64 bytes)

DW1 (Byte 4-7):
  [35:32] First DW BE = 1111 (第一个数据DW全部有效)
  [39:36] Last DW BE  = 1111 (最后一个数据DW全部有效)
  [47:40] Tag         = 0x01
  [63:48] Requester ID = {Bus, Device, Function}

DW2 (Byte 8-11):
  [95:64] Address = alloc_addrl (Linux DMA buffer 物理地址)
                    ↑ 这是最关键的字段：FPGA 向这个地址写数据

DW3-DW18 (Byte 12-75):
  数据载荷 64 bytes (16 DW)
  来自 hdmi_pcie_fifo → pcie_dma_data → r_axis_s_tdata
```

**对应 FPGA 代码** (`pcie_dma_ctrl.v:297-326`)：

```verilog
// MWR_TLP_HEADER 状态：组装 TLP Header
r_axis_s_tdata[9  :  0] <= TLP_LENGTH;           // Length = 16 DW
r_axis_s_tdata[31 : 29] <= 3'b010;                // Fmt = 3DW with Data
r_axis_s_tdata[28 : 24] <= 5'b00000;              // Type = Memory Write
r_axis_s_tdata[35 : 32] <= 4'hf;                  // First DW BE
r_axis_s_tdata[39 : 36] <= 4'hf;                  // Last DW BE
r_axis_s_tdata[95 : 64] <= alloc_addrl;            // ← 目标物理地址
r_axis_s_tdata[127: 96] <= 32'd0;                 // 3DW header 无高32位地址
```

### 5.3 为什么是 3DW 而不是 4DW Header？

- **3DW Header**：地址位宽 32-bit（本项目的 FPGA 只支持 32-bit DMA 地址）
- **4DW Header**：地址位宽 64-bit（PCIe 支持超过 4GB 的地址空间）

FPGA 的 MWr 地址寄存器是 32-bit (`pcie_dma_ctrl.v:73`)，所以只能用 3DW Header。

### 5.4 一个完整帧的 TLP 数量

```
一帧 RGB565 1080p = 1920 × 1080 × 2 = 4,147,200 bytes
每个 MWR TLP 携带 = 64 bytes 有效数据
理论 TLP 数 = 4,147,200 / 64 = 64,800

FPGA 实际发送 = 64,801 个 TLP
第 64801 个 TLP 的内容是帧标记（非图像数据），多出的 64 bytes
```

`DMA_TRAN_TIMES = 64801` 定义在 `pcie_dma_ctrl.v:56`。

---

## 6. FPGA 端完整数据流（从 HDMI 到 PCIe）

### 6.1 视频时序（sync_vg.v + pattern_vg.v）

**sync_vg.v** 生成 1080p 标准时序（V_TOTAL=1125, H_TOTAL=2200）：
- `vs_out`：垂直同步信号（每帧开始一个脉冲）
- `hs_out`：水平同步信号（每行开始一个脉冲）
- `de_out`：数据有效信号（只在 1920×1080 有效区域内为高）
- `x_act, y_act`：当前像素的 (X, Y) 坐标

**pattern_vg.v** 用 `x_act` 坐标生成 8 色彩条：

```verilog
// pattern_vg.v:63-110 — 8 条竖彩条，每条 240 像素宽
if(act_x < H_ACT/8)          {r,g,b} = {8'hff, 8'hff, 8'hff}  // 白
else if(act_x < 2*H_ACT/8)   {r,g,b} = {8'hff, 8'hff, 8'h00}  // 黄
else if(act_x < 3*H_ACT/8)   {r,g,b} = {8'h00, 8'hff, 8'hff}  // 青
else if(act_x < 4*H_ACT/8)   {r,g,b} = {8'h00, 8'hff, 8'h00}  // 绿
else if(act_x < 5*H_ACT/8)   {r,g,b} = {8'hff, 8'h00, 8'hff}  // 品红
else if(act_x < 6*H_ACT/8)   {r,g,b} = {8'hff, 8'h00, 8'h00}  // 红
else if(act_x < 7*H_ACT/8)   {r,g,b} = {8'h00, 8'h00, 8'hff}  // 蓝
else                           {r,g,b} = {8'h00, 8'h00, 8'h00}  // 黑
```

### 6.2 RGB888 → RGB565 转换

在 `ddr_test_top_pcie_fixed.v:964`：

```verilog
// R 取高5位, G 取高6位, B 取高5位 → 正好 16-bit RGB565
assign pcie_data_out = video_enhance_de_out ?
    {video_enhance_r_out[7:3],    // R: 5 bits
     video_enhance_g_out[7:2],    // G: 6 bits
     video_enhance_b_out[7:3]} :  // B: 5 bits
    16'd0;
```

RGB565 像素值的理论值（用于 `--validate` 验证）：

| 颜色 | RGB888 | R[7:3] | G[7:2] | B[7:3] | RGB565 |
|------|--------|--------|--------|--------|--------|
| 白 | (255,255,255) | 11111 | 111111 | 11111 | **0xFFFF** |
| 黄 | (255,255,0) | 11111 | 111111 | 00000 | **0xFFE0** |
| 青 | (0,255,255) | 00000 | 111111 | 11111 | **0x07FF** |
| 绿 | (0,255,0) | 00000 | 111111 | 00000 | **0x07E0** |
| 品红 | (255,0,255) | 11111 | 000000 | 11111 | **0xF81F** |
| 红 | (255,0,0) | 11111 | 000000 | 00000 | **0xF800** |
| 蓝 | (0,0,255) | 00000 | 000000 | 11111 | **0x001F** |
| 黑 | (0,0,0) | 00000 | 000000 | 00000 | **0x0000** |

### 6.3 跨时钟域 FIFO（hdmi_pcie_fifo）

视频像素时钟 (`pix_clk_out` = 148.5MHz for 1080p) 与 PCIe 时钟 (`pclk_div2` = 125MHz for Gen2) 不同。

`hdmi_pcie_fifo` 是一个异步 FIFO：
- **写侧**：`pix_clk_out` (148.5MHz)，写入 16-bit RGB565 像素
- **读侧**：`clk` (125MHz)，读出 128-bit 数据块（一次读 8 个像素，凑够 128 bits）
- **wr_en** = `de_in && fram_start`（只在有效像素区域内写入）
- **rd_en** = `pcie_rd_en`（由 DMA 控制状态机管理）

### 6.4 DMA 状态机（pcie_dma_ctrl.v）

这是 FPGA 端最核心的逻辑。状态机有 3 个状态：

```
           ┌──────────────────────────────────┐
           │                                  │
           ▼                                  │
      MWR_IDLE ──► MWR_TLP_HEADER ──► MWR_TLP_DATA ──┘
         │              │                   │
         │ 等 FIFO       │ 组装 128-bit     │ 发 16 DW 数据
         │ 水位够了      │ TLP Header       │ 64 bytes
         │              │                  │
         └── 水位条件 ──┘                  └── TLAST → 回 IDLE
```

**启动条件**（三者同时满足）：
1. `fram_start = 1`（VS 下降沿触发）
2. `dma_start = 1`（FIFO 水位 ≥ 4 个 128-bit 数据块）
3. `rc_cfg_ep_flag = 1`（4 个 DMA 地址已全部配置且非零）

**地址递增**：每发完一个 TLP，`alloc_addrl += 16*4 = 64`（每个 TLP 64 bytes），指向 buffer 的下一个 64 字节位置。

**帧切换**：VS 上升沿时，`addr_page` 递增（0→1→2→3→0...），下一帧自动切换到下一个 DMA buffer。

---

## 7. Linux 端完整数据流（从 probe 到 raw 文件）

### 7.1 驱动加载 (insmod)

```
insmod colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1
           │
           ▼
    colorbar_init()                           ← 创建 /dev/colorbar_pcie_rx
           │
           ▼
    pci_register_driver()                     ← 注册 PCI 驱动
           │
           ▼
    (内核检测到 0755:0755，调用 probe)
           │
           ▼
    colorbar_probe()
      ├─ pci_enable_device_mem()              ← 使能 PCIe 内存空间
      ├─ dma_set_mask_and_coherent(32bit)     ← 限制 32-bit DMA 地址
      ├─ pci_set_master()                     ← 允许 FPGA 做主控 DMA
      ├─ pci_request_region(bar=1)            ← 申请 BAR1 使用权
      ├─ pci_iomap(bar=1)                     ← 映射 BAR1 到内核虚拟地址
      ├─ colorbar_hw_safe_stop()              ← ★ 安全清除旧 DMA 配置
      └─ 设置 g_cdev                          ← 暴露给 fops
```

### 7.2 用户态采集一帧 (--once)

```
./pcie_color_rx --once --output frame_0000.rgb565
           │
           ▼
    capture_once()
      ├─ open("/dev/colorbar_pcie_rx")        ← 打开设备
      ├─ ioctl(COLORBAR_IOC_ALLOC_BUFS)       ← 分配 4 个 DMA buffer
      │    └─ dma_alloc_coherent() × 4
      │
      ├─ mmap() × 4                           ← 把 DMA buffer 映射到用户态
      │    └─ dma_mmap_coherent() per buffer
      │
      ├─ ioctl(COLORBAR_IOC_START)            ← 把 DMA 地址写给 FPGA
      │    └─ for i in 0..3:
      │         iowrite32(swab32(dma_addr_i), bar+0x110)
      │       FPGA 收到 4 个地址后 rc_cfg_ep_flag=1
      │       下一个 VS → fram_start → DMA 开始
      │
      ├─ ioctl(COLORBAR_IOC_WAIT_FRAME)       ← 等待 frame_wait_ms 毫秒
      │    └─ msleep(100) // 临时方案
      │
      ├─ write_all(fd, buffer[frame.buffer_index], 4147200)
      │                                         ← 保存到文件
      │
      ├─ validate_buffer()                     ← 抽样校验 8 个彩条点
      │
      ├─ ioctl(COLORBAR_IOC_STOP)             ← 写 0x130 STOP
      │
      ├─ ioctl(COLORBAR_IOC_FREE_BUFS)        ← 释放 DMA buffer
      └─ close(dev_fd)
```

### 7.3 mmap 原理

为什么用户态程序可以直接读写 DMA buffer，而不用 `read()` 系统调用？

```
┌───────────────────────────────────────────────┐
│              物理内存 (DDR)                     │
│  ┌──────────────────────────────────┐         │
│  │ DMA coherent buffer (4,149,248 B)│         │
│  │ 物理地址: dma_addr (如 0xF4200000)│         │
│  └──────────┬───────────────────────┘         │
│             │                                   │
│    ┌────────┴────────┐                         │
│    │                 │                         │
│    ▼ 内核虚拟地址    ▼ 用户态虚拟地址            │
│  cpu_addr          mmap 返回的指针              │
│  (给内核用)        (给 pcie_color_rx 用)       │
│                                                 │
│  两者指向同一块物理内存！                         │
└───────────────────────────────────────────────┘
```

`dma_mmap_coherent()` 的作用：在用户进程的页表中建立映射，让用户态指针也能访问同一块 coherent DMA 内存。

**优点**：用户态直接读 DMA buffer，零拷贝（不需要 `copy_to_user`），不消耗额外的 CPU 时间。

---

## 8. 寄存器协议详解

### 8.1 完整交互时序

```
时间线 ──────────────────────────────────────────►

FPGA 上电    Linux insmod    Linux --once START    VS 边沿  一帧结束
   │            │                │                  │         │
   ▼            ▼                ▼                  ▼         ▼
dma_addr0=0   probe STOP    写 0x110×4          fram_start  DMA 传输完成
dma_addr1=0   清空地址      dma_addr0=A0       = 1          dma_cnt
dma_addr2=0                 dma_addr1=A1       开始发       = 64801
dma_addr3=0                 dma_addr2=A2       MWR TLP      自动停止
                            dma_addr3=A3
                                │
                                ▼
                         rc_cfg_ep_flag = 1
                         (4 地址非零，允许 DMA)

下一帧 VS 上升沿:
  addr_page 0→1, alloc_addrl = dma_addr1
  自动写入 buffer 1

用户调用 STOP:
  iowrite32(0, bar+0x130) → FPGA dma_stop_flag=1
  → dma_addr0..3=0, rc_cfg_ep_flag=0
  → DMA 彻底停止
```

### 8.2 寄存器表

| BAR 偏移 | 访问方向 | 名称 | FPGA 内部变量 | 说明 |
|----------|---------|------|--------------|------|
| 0x110 | RC→EP (MWr) | DMA_ADDR | dma_addr0..3 | 连续写 4 次，每次 32-bit 地址 |
| 0x130 | RC→EP (MWr) | DMA_STOP | dma_stop_flag | 写任意值即可停止 DMA |
| 0x140 | RC→EP (MWr) | 亮部压缩 | video_enhance_lightdown | 视频增强参数（可选） |
| 0x150 | RC→EP (MWr) | 暗部提升 | video_enhance_darkup | 视频增强参数（可选） |
| 0x160 | RC→EP (MWr) | 增强清除 | - | 关闭视频增强 |

**注意**：当前 FPGA 没有可读的状态寄存器（如 frame_done、current_page）。Linux 端只能靠固定延时 (`frame_wait_ms`) 猜测一帧是否完成。

### 8.3 寄存器访问的 PCIe 总线视角

Linux `iowrite32(value, bar + 0x110)` 产生的 PCIe 事务：

```
CPU 执行 iowrite32(0x000020F4, 0xf4204110)
  ↓
ARM MMU 将 0xf4204110 翻译为 PCIe 总线地址
  ↓
RK3568 PCIe RC 发起 MWR TLP:
  Fmt = 3DW with Data
  Type = Memory Write Request
  Length = 1 DW
  Address = 0xf4204110 (BAR1 的物理基址 + 0x110)
  Data = 0x000020F4
  ↓
FPGA PCIe EP Core 接收 TLP
  ↓
bar_hit = 2'b01 (命中 BAR1)
  ↓
axis_master_tdata 上出现 TLP 数据
  ↓
pcie_dma_ctrl.v 解码:
  tlp_fmt = 3'b010, tlp_type = 5'b00000 → MWR_32
  cmd_reg_addr = 0x110 → DMA_CMD_L_ADDR
  ↓
dma_addr0 = byte_reverse(0x000020F4) = 0xF4200000 ✓
```

---

## 9. 字节序——为什么需要 addr_byteswap

### 9.1 问题的来源

PCIe 总线是小端序（Little-Endian）。ARM Linux 也是小端序。但 FPGA 代码里对收到的数据做了字节反转。

**根源在 `pcie_dma_ctrl.v:161`**：

```verilog
dma_addr0 <= {axis_master_tdata_d0[7:0],   // 原 Byte 0 → 新 Byte 3
              axis_master_tdata_d0[15:8],   // 原 Byte 1 → 新 Byte 2
              axis_master_tdata_d0[23:16],  // 原 Byte 2 → 新 Byte 1
              axis_master_tdata_d0[31:24]}; // 原 Byte 3 → 新 Byte 0
```

这是**完整的 32-bit 字节反转**（不是大小端转换！）。

### 9.2 为什么 FPGA 要这样做？

FPGA 代码注释提到"地址字节重排"。这是因为 Pango PCIe IP 在 AXI-S 接口上呈现的 TLP 数据字节顺序，与 FPGA 内部期望的 32-bit 地址格式不一致。IP 核以某种字节顺序输出 TLP 字段，FPGA 开发者用字节反转来纠正。

**关键：这不是一个"bug"，而是 Pango IP 的约定。Linux 必须配合这个约定。**

### 9.3 数据路径上的字节序

FPGA 不仅在地址上做了字节反转，在图像数据上也做了字节重排（`pcie_dma_ctrl.v:332-336`）：

```verilog
// 128-bit 数据块内，每字节位置都重新排列
r_axis_s_tdata <= {
    pcie_dma_data[103:96], // Byte 12 → 15
    pcie_dma_data[111:104],// Byte 13 → 14
    ...
    pcie_dma_data[7:0],    // Byte 0  → 3
    pcie_dma_data[15:8],   // Byte 1  → 2
    ...
};
```

这意味着 Linux 端收到的 RGB565 像素，其字节顺序取决于这层重排 + FIFO 的输出顺序 + PCIe 线序。最简单的验证方法是：收到第一帧后，用 `--validate` 看采样像素的实际值，如果字节反了就加一层 `swab16()`。

### 9.4 addr_byteswap 数学验证

```
假设 Linux dma_addr = 0x_F420_0000

path A: addr_byteswap = 0 (不交换)
  iowrite32(0xF4200000)
  → TLP data = 0xF4200000 (LE: byte0=00, byte1=00, byte2=20, byte3=F4)
  → FPGA byte_reverse: {00,00,20,F4} = 0x_0000_20F4  ← 错误!

path B: addr_byteswap = 1 (swab32)
  swab32(0xF4200000) = 0x000020F4
  iowrite32(0x000020F4)
  → TLP data = 0x000020F4
  → FPGA byte_reverse: {F4,20,00,00} = 0x_F420_0000  ← 正确! ✓
```

**结论：`addr_byteswap=1`（即默认值 `true`）是正确的。**

当前代码已将默认值设为 `true`（`driver/colorbar_pcie_driver.c:43`）。

---

## 10. 调试技巧速查

### 10.1 常用诊断命令

```sh
# PCIe 设备是否被识别
lspci -nn
lspci -vv -s 01:00.0

# BAR 物理地址分配
cat /sys/bus/pci/devices/0000:01:00.0/resource

# PCIe 链路状态（速度、宽度）
lspci -vv -s 01:00.0 | grep -E "LnkSta|LnkCap"

# 内核 PCIe 日志
dmesg | grep -iE "pcie|pci|colorbar"

# 当前加载的 PCIe 相关驱动
lsmod | grep -E "pcie|pango|colorbar"

# 设备节点
ls -l /dev/colorbar_pcie_rx

# 谁在占用设备
sudo fuser -v /dev/colorbar_pcie_rx

# DMA buffer 地址（从 dmesg）
dmesg | grep -E "buffer[0-9]|dma="

# PCIe 配置空间原始值
sudo lspci -xxx -s 01:00.0
```

### 10.2 常见故障排查

| 现象 | 最可能原因 | 排查步骤 |
|------|-----------|---------|
| `lspci` 看不到 0755:0755 | PCIe 链路未训练 | 检查 REFCLK、PERST#、FPGA 是否加载了含 PCIe IP 的 bitstream |
| Link up 但 probe 不触发 | Vendor/Device ID 不匹配 | 检查 `lspci -nn` 输出的 ID 是否对 |
| probe 成功但 `--once` 在 START 处失败 | `allow_dma_start=0`（正常保护） | 确认 BAR 正确后加 `allow_dma_start=1` |
| frame_0000.rgb565 全零 | DMA 没写到正确地址 | 检查 BAR、addr_byteswap、FPGA VS 信号 |
| frame 有数据但校验全错 | 字节序或地址偏移错误 | 用已知测试图案确认字节序 |
| 文件大小不对 | frame_wait_ms 太短 | 增大 frame_wait_ms（200/500ms） |
| **系统崩溃** | DMA 写到错误物理地址 | **立即断电 FPGA**，不要盲试 BAR/addr_byteswap |

### 10.3 安全守则（再次强调）

```
1. FPGA 断电 > 改参数盲试
2. BAR 正确是安全的唯一前提
3. 不确定 BAR → 先确认 → 再 allow_dma_start=1
4. 每次 FPGA 重新上电 = DMA 地址自动清零 = 多一层保护
5. 先跑 --safe-stop，确认设备通信正常，再跑 --once
```

---

## 11. 关键术语速查表

| 术语 | 缩写 | 一句话解释 |
|------|------|-----------|
| Root Complex | RC | PCIe 总线树形结构的"根"，在本项目是 RK3568 |
| Endpoint | EP | PCIe 总线树形结构的"叶子"，在本项目是 FPGA |
| Base Address Register | BAR | EP 告诉 RC 需要多大的 MMIO 空间，RC 分配物理地址 |
| Memory Write Request | MWr | EP 向 RC 内存写数据的 TLP 类型 |
| Transaction Layer Packet | TLP | PCIe 事务层的传输单元，包含 Header + Data |
| Direct Memory Access | DMA | 设备（FPGA）不经 CPU 直接访问主机内存 |
| Bus Master | - | 能主动发起 PCIe 事务的设备角色 |
| dma_addr_t | - | Linux 内核中的 DMA 物理地址（总线地址），FPGA 用的就是这个 |
| coherent DMA | - | CPU 和设备看到同一块物理内存的数据一致，无需手动 flush cache |
| Memory-Mapped I/O | MMIO | 把设备寄存器映射到主机内存地址空间，用 `iowrite32/ioread32` 访问 |
| iowrite32 | - | Linux 内核函数：向 MMIO 地址写 32-bit 值 |
| pci_iomap | - | 把 PCI BAR 的物理地址映射到内核虚拟地址空间 |
| mmap | - | 把内核 buffer 映射到用户态进程的虚拟地址空间 |
| swab32 | - | Linux 内核函数：32-bit 字节反转（不是大小端转换） |
| VS (Vertical Sync) | - | 视频垂直同步信号，每帧开始/结束的标记 |
| DE (Data Enable) | - | 视频数据有效信号，只在有效像素区域内为高 |
| FIFO | - | 先进先出缓冲，本项目用于跨时钟域（148.5MHz ↔ 125MHz） |
| Link Training | LTSSM | PCIe 链路建立连接的协商过程 |
| Gen2 x1 | - | PCIe 2.0 单通道，理论带宽 500 MB/s，实际约 350-400 MB/s |
| RGB565 | - | 16-bit 像素格式：R 5-bit, G 6-bit, B 5-bit |

---

## 12. 延伸阅读

- **PCIe 基础规范**：MindShare 的《PCI Express Technology 3.0》——最完整的 PCIe 体系书籍
- **Linux DMA API**：`Documentation/DMA-API.txt`（内核源码树内）——`dma_alloc_coherent` 的权威文档
- **Linux PCI 驱动**：`Documentation/PCI/pci.txt`——`pci_enable_device`、`pci_iomap` 等函数的官方说明
- **Pango PCIe IP 手册**：紫光同创 FPGA PCIe Hard Core 的 IP 文档——解释 BAR 配置、AXI-S 接口
- **[本项目] PCIe 调试安全指南**：`doc/PCIe调试安全指南与优化建议.md`——解释为什么系统会崩以及如何避免
- **[本项目] Linux 端实施路线**：`doc/Linux端负责人实施路线.md`——当前工程进度和已验证信息