# Linux端PCIe DMA工程分析文档

## 1. 工程概述

### 1.1 工程定位

本工程是一个运行在Linux系统上的PCIe DMA测试工具，用于实现FPGA（端点设备）与Linux主机（根复杂设备）之间的高速数据传输。工程包含两个核心部分：

| 部分 | 路径 | 功能描述 |
|-----|------|---------|
| **Linux内核驱动** | `driver/` | 实现PCIe设备驱动、DMA地址映射、数据传输控制 |
| **用户态测试应用** | `app_pcie/` | 提供GTK图形界面，支持DMA自动测试、手动测试、PIO测试、性能测试等 |

### 1.2 硬件平台

- **FPGA端**：紫光同创 PG2L100H，PCIe Gen2 x1接口
- **Linux端**：瑞芯微RK3568（鲁班猫2开发板），作为PCIe根复杂设备
- **PCIe配置**：Vendor ID = 0x0755，Device ID = 0x0755

### 1.3 软件环境

| 组件 | 版本/要求 |
|-----|----------|
| 操作系统 | Linux（支持RK3568） |
| 内核版本 | 4.19+ |
| GUI库 | GTK2 |
| 编译工具 | gcc、make、Linux内核头文件 |

---

## 2. 工程架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                     Linux 用户空间                          │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              PCIe Test GUI (GTK)                    │   │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────┐       │   │
│  │  │ DMA Auto   │ │ DMA Manual │ │ PIO Test   │       │   │
│  │  └─────┬──────┘ └─────┬──────┘ └─────┬──────┘       │   │
│  │        │              │              │               │   │
│  │  ┌─────▼──────────────▼──────────────▼──────┐       │   │
│  │  │         ioctl 系统调用                    │       │   │
│  │  └──────────────────┬───────────────────────┘       │   │
│  └─────────────────────┼───────────────────────────────┘   │
│                        │ /dev/pango_pci_driver             │
│                        ▼                                   │
│  ┌─────────────────────────────────────────────────────┐   │
│                     Linux 内核空间                        │
│  ┌─────────────────────────────────────────────────────┐   │
│  │            pango_pci_driver.ko                      │   │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────┐       │   │
│  │  │ PCI驱动    │ │ DMA控制    │ │ 字符设备   │       │   │
│  │  │ 注册/探测  │ │ 地址映射   │ │ 文件操作   │       │   │
│  │  └─────┬──────┘ └─────┬──────┘ └─────┬──────┘       │   │
│  │        │              │              │               │   │
│  │        └──────────────┼──────────────┘               │   │
│  │                       ▼                              │   │
│  │              pci_alloc_consistent                    │   │
│  │              (分配DMA一致性内存)                      │   │
│  │                       │                              │   │
│  └───────────────────────┼─────────────────────────────┘   │
│                          │ PCIe Bus                       │
│                          ▼                                 │
└──────────────────────────┼─────────────────────────────────┘
                           │
                           ▼
              ┌───────────────────────┐
              │   FPGA Endpoint       │
              │   (PCIe_DMA_test)     │
              │   彩条生成 + DMA发送   │
              └───────────────────────┘
```

### 2.2 模块职责

| 模块 | 文件 | 核心职责 |
|-----|------|---------|
| **PCI驱动** | `pango_pci_driver.c` | PCI设备探测、BAR空间映射、DMA掩码设置 |
| **字符设备** | `pango_pci_driver.c` | 提供用户态接口（open/read/write/ioctl） |
| **DMA控制** | `pango_pci_driver.c` | DMA地址分配、读写控制、同步保护 |
| **GUI界面** | `main.c` | GTK界面、用户交互、测试流程控制 |
| **配置定义** | `pango_pci_driver.h` | 数据结构、IOCTL命令、寄存器偏移 |
| **ID配置** | `id_config.h` | Vendor ID、Device ID定义 |

---

## 3. 内核驱动分析

### 3.1 驱动整体结构

```c
// 驱动入口/出口
module_init(init_pci_pango);      // 加载时执行
module_exit(exit_pci_pango);      // 卸载时执行

// 初始化流程
init_pci_pango()
  ├── sema_init()                 // 信号量初始化
  ├── spin_lock_init()            // 自旋锁初始化
  ├── init_pango_cdev()           // 字符设备初始化
  ├── init_pango_pci_driver()     // PCI驱动注册
  └── init_pango_cdev_class()     // 设备类创建
```

### 3.2 PCI设备探测流程

**`pci_driver_probe()`** 函数负责PCI设备的初始化：

```c
int pci_driver_probe(struct pci_dev *dev, const struct pci_device_id *device_id)
{
    // 1. 使能PCI设备
    pci_enable_device(dev);
    
    // 2. 设置DMA掩码（优先64位，其次32位）
    set_dma_mask(dev);
    
    // 3. 读取PCI配置空间（Vendor ID、Device ID、BAR等）
    ReadConfig(dev);
    
    // 4. 设置为主设备模式
    pci_set_master(dev);
    
    // 5. 请求BAR资源
    pci_request_region(dev, BAR, NULL);
    
    // 6. 映射BAR空间到虚拟地址
    pci_info._pci_io = ioremap(bar_address, bar_size);
}
```

**关键数据结构**（`pango_pci_driver.h`）：

| 结构体 | 用途 | 关键字段 |
|-------|------|---------|
| `PciPango` | 驱动全局信息 | `_cdev`、`_pango_pci_driver`、`_sem` |
| `PangoPciDriver` | PCI驱动信息 | `_pci_bar`、`_pci_io`（虚拟地址）、`_pci_io_size` |
| `DMA_INFO` | DMA控制信息 | `cmd`、`addr_r`（读地址）、`addr_w`（写地址） |
| `DMA_ADDR` | DMA地址描述 | `addr`（物理地址）、`data_buf`（虚拟地址）、`lock` |

### 3.3 IOCTL命令系统

驱动通过IOCTL实现用户态与内核态的通信，支持以下命令：

| 命令 | 数值 | 功能描述 |
|-----|------|---------|
| `PCI_READ_DATA_CMD` | 0 | 读取PCI配置空间数据 |
| `PCI_WRITE_DATA_CMD` | 1 | 写入PCI配置空间数据 |
| `PCI_MAP_ADDR_CMD` | 2 | 分配DMA一致性内存并映射 |
| `PCI_WRITE_TO_KERNEL_CMD` | 3 | 将用户数据写入内核DMA缓冲区 |
| `PCI_DMA_READ_CMD` | 4 | 触发DMA读操作（FPGA → DDR） |
| `PCI_DMA_WRITE_CMD` | 5 | 触发DMA写操作（DDR → FPGA） |
| `PCI_READ_FROM_KERNEL_CMD` | 6 | 从内核DMA缓冲区读取数据到用户态 |
| `PCI_UMAP_ADDR_CMD` | 7 | 释放DMA映射地址 |
| `PCI_PERFORMANCE_START_CMD` | 8 | 启动性能测试 |
| `PCI_PERFORMANCE_END_CMD` | 9 | 结束性能测试 |

**DMA命令寄存器格式**（`DMA_CMD`联合体）：

```c
typedef union _DMA_CMD_ {
    struct _cmd1_ {
        unsigned short length    : 10;  // 传输长度
        unsigned char  reserved1 : 6;
        unsigned char  addr_type : 1;   // 地址类型：0=32位，1=64位
        unsigned char  reserved2 : 7;
        unsigned char  op_type   : 1;   // 操作类型：0=读，1=写
        unsigned char  reserved3 : 7;
    } data;
    unsigned int value;
} DMA_CMD;
```

### 3.4 DMA内存管理

驱动使用 `pci_alloc_consistent()` 分配DMA一致性内存，确保CPU和DMA控制器看到一致的数据视图：

```c
// 分配内存（读/写各一块）
data_buf_r = pci_alloc_consistent(op_dev, size, &dma_addr_r);
data_buf_w = pci_alloc_consistent(op_dev, size, &dma_addr_w);

// 设置DMA地址到FPGA寄存器
set_dma_addr(&dma_info.addr_r, pci_pango);  // 写地址低32位到0x110
set_dma_addr(&dma_info.addr_w, pci_pango);  // 写地址高32位到0x120（如果需要）

// 设置DMA命令
set_dma_w_r(dma_info.cmd.value, pci_pango); // 写命令到0x100
```

**寄存器偏移定义**：

| 偏移地址 | 寄存器名称 | 功能 |
|---------|-----------|------|
| 0x100 | `CMD_REG_OFFSET` | DMA命令寄存器（操作类型、长度、地址类型） |
| 0x110 | `RW_ADDR_LO_OFFSET` | DMA地址低32位 |
| 0x120 | `RW_ADDR_HI_OFFSET` | DMA地址高32位 |

### 3.5 同步机制

驱动使用两种同步机制：

| 机制 | 用途 | 保护对象 |
|-----|------|---------|
| **信号量** (`_sem`) | 进程间互斥 | 整个IOCTL操作 |
| **自旋锁** (`addr_r.lock`) | 中断上下文保护 | DMA地址和缓冲区 |

---

## 4. 用户态应用分析

### 4.1 应用架构

用户态应用是一个GTK2图形界面程序，包含以下测试页面：

| 页面 | 功能描述 |
|-----|---------|
| **DMA Auto** | 自动测试，配置包大小范围，自动执行读写测试 |
| **DMA Manual** | 手动测试，手动分配内存、写入数据、触发DMA传输 |
| **PIO Test** | PIO测试，通过BAR空间直接读写FPGA寄存器 |
| **Tandem** | 位流加载，支持Tandem配置方式加载FPGA位流 |
| **Performance** | 性能测试，测试PCIe DMA读写吞吐量 |
| **Config** | 配置操作，读写PCI配置空间寄存器 |
| **Endpoint Status** | 端点状态，显示PCIe设备信息（Vendor ID、Link Speed等） |

### 4.2 GUI界面布局

界面基于GTK2的Notebook和Fixed容器构建：

```
主窗口 (770x740)
├── Notebook (左侧)
│   ├── DMA Auto      - 自动测试参数配置
│   ├── DMA Manual    - 手动测试控制
│   ├── PIO Test      - PIO测试参数
│   └── Tandem        - 位流加载
├── Performance (右上)   - 性能测试结果显示
├── Config (中)         - PCI配置空间操作
├── Endpoint Status (中下)- 设备状态显示
└── Text View (底部)     - 日志输出
```

### 4.3 测试流程

#### 4.3.1 DMA手动测试流程

```
1. 用户点击 "Start DMA"
   ├── ioctl(PCI_MAP_ADDR_CMD)      // 分配DMA内存
   └── 记录分配大小和偏移地址

2. 用户点击 "Write DDR"
   ├── 准备测试数据
   ├── ioctl(PCI_WRITE_TO_KERNEL_CMD)  // 写入内核缓冲区
   └── ioctl(PCI_DMA_WRITE_CMD)        // 触发DMA写（DDR→FPGA）

3. 用户点击 "DMA Read"
   ├── ioctl(PCI_DMA_READ_CMD)         // 触发DMA读（FPGA→DDR）
   └── ioctl(PCI_READ_FROM_KERNEL_CMD) // 读取内核数据到用户态

4. 用户点击 "Close DMA"
   └── ioctl(PCI_UMAP_ADDR_CMD)        // 释放DMA内存
```

#### 4.3.2 DMA自动测试流程

```
1. 用户设置测试参数
   ├── Test Num: 测试次数
   ├── Start/End Packet Size: 包大小范围
   ├── Packet Step: 步长
   └── 点击 "Start Test"

2. 后台线程执行循环
   ├── 分配DMA内存
   ├── 填充测试数据
   ├── DMA写操作
   ├── DMA读操作
   ├── 比较读写数据
   ├── 统计错误计数
   └── 循环直到完成或停止
```

### 4.4 关键函数

| 函数 | 功能 | 调用时机 |
|-----|------|---------|
| `open_pci_driver()` | 打开PCI驱动设备 | 程序启动时 |
| `cmd_operation()` | 执行命令操作 | 配置读写、位流加载 |
| `dma_auto_process()` | DMA自动测试处理 | 后台线程循环 |
| `dma_manual_process()` | DMA手动测试处理 | 用户点击按钮 |
| `performance_process()` | 性能测试处理 | 性能测试线程 |
| `pio_test_process()` | PIO测试处理 | PIO测试线程 |
| `print_data()` | 打印回读数据并比较 | DMA读完成后 |

### 4.5 线程模型

应用使用POSIX线程实现后台测试：

```c
// 后台处理线程
void *process_thread(void *arg) {
    while (running) {
        switch (button_info) {
            case "dma_auto":      dma_auto_process();      break;
            case "dma_manual":    dma_manual_process();    break;
            case "pio_test":      pio_test_process();      break;
            case "performance":   performance_process();   break;
        }
        usleep(1000);  // 1ms轮询
    }
}
```

---

## 5. 数据流分析

### 5.1 DMA方向说明

**重要**：驱动中的DMA方向定义如下（`pango_pci_driver.c`第445-459行）：

| 命令 | op_type | 实际方向 | 描述 |
|-----|---------|---------|------|
| `PCI_DMA_READ_CMD` | 0 | Linux → FPGA | FPGA从主机内存读取数据（FPGA读） |
| `PCI_DMA_WRITE_CMD` | 1 | FPGA → Linux | FPGA向主机内存写入数据（FPGA写） |

**FPGA自主传输模式**：根据FPGA端代码分析，FPGA采用**自主发送模式**。当Linux主机将DMA目标地址写入FPGA寄存器0x110后，FPGA会自动开始发送MWR（Memory Write Request）TLP包，将彩条数据写入主机指定的物理地址。Linux端不需要主动"触发"每一次传输。

### 5.2 DMA写操作（FPGA → Linux，PCI_DMA_WRITE_CMD）

这是彩条图像传输的主要方向：

```
FPGA彩条生成器 (1920x1080 RGB565)
        │
        ▼ 帧同步信号
FPGA DMA控制器
        │
        ▼ PCIe MWR TLP (每包64字节)
Linux DMA缓冲区 (dma_addr_w, 物理地址)
        │
        ▼ copy_to_user()
用户态内存 (read_buf)
        │
        ▼
显示模块 / 图像处理
```

**驱动层实现**（`pango_pci_driver.c`第453-459行）：

```c
case PCI_DMA_WRITE_CMD:                          /* CPU读数据 */
    spin_lock(&dma_info.addr_r.lock);
    dma_info.cmd.data.op_type = 1;               /* DMA写操作，FPGA将数据写入到DDR */
    memset(dma_info.addr_w.data_buf, 0, dma_operation.current_len*4);
    set_dma_w_r(dma_info.cmd.value, pci_pango);
    set_dma_addr(&dma_info.addr_w, pci_pango);   /* 设置FPGA写入的目标地址 */
    spin_unlock(&dma_info.addr_r.lock);
break;
```

### 5.3 DMA读操作（Linux → FPGA，PCI_DMA_READ_CMD）

用于主机向FPGA发送配置数据或控制命令：

```
用户态内存 (write_buf)
        │
        ▼ copy_from_user()
内核态DMA缓冲区 (data_buf_r)
        │
        ▼ memcpy()
FPGA寄存器 (0x100=CMD, 0x110=ADDR_LO, 0x120=ADDR_HI)
        │
        ▼ PCIe MRd TLP
FPGA内部DMA控制器
        │
        ▼
FPGA DDR3 / 寄存器
```

**驱动层实现**（`pango_pci_driver.c`第445-450行）：

```c
case PCI_DMA_READ_CMD:                           /* CPU写数据 */
    spin_lock(&dma_info.addr_r.lock);
    dma_info.cmd.data.op_type = 0;               /* DMA读操作，FPGA将数据读入到FPGA设备 */
    set_dma_w_r(dma_info.cmd.value, pci_pango);
    set_dma_addr(&dma_info.addr_r, pci_pango);   /* 设置FPGA读取的源地址 */
    spin_unlock(&dma_info.addr_r.lock);
break;
```

### 5.4 彩条图像数据格式

根据FPGA工程分析，彩条数据格式为：

| 属性 | 值 |
|-----|-----|
| 分辨率 | 1920 × 1080 |
| 像素格式 | RGB565 |
| 单像素字节 | 2字节 |
| 单帧数据量 | 1920 × 1080 × 2 = 4,147,200字节 |
| DMA包数量 | 64,801包（每包64字节） |

**RGB565位映射**（`ddr_test_top.v`第986行）：

```verilog
assign pcie_data_out = video_enhance_de_out ? 
    {video_enhance_r_out[7:3], video_enhance_g_out[7:2], video_enhance_b_out[7:3]} : 'd0;
```

| 位域 | 宽度 | 来源 | 说明 |
|-----|------|------|------|
| [15:11] | 5位 | `video_enhance_r_out[7:3]` | 红色分量（高5位） |
| [10:5] | 6位 | `video_enhance_g_out[7:2]` | 绿色分量（高6位） |
| [4:0] | 5位 | `video_enhance_b_out[7:3]` | 蓝色分量（高5位） |

### 5.5 完整传输流程（FPGA彩条 → RK3568显示）

```
┌─────────────────────────────────────────────────────────────────────┐
│                           FPGA端                                   │
├─────────────────────────────────────────────────────────────────────┤
│  1. 彩条生成器生成1920x1080 RGB565图像                               │
│  2. 视频增强处理（可选）                                             │
│  3. 写入跨时钟域FIFO（148.5MHz → 125MHz）                            │
│  4. DMA控制器组装TLP包（每包64字节）                                  │
│  5. PCIe发送MWR TLP到主机                                           │
└─────────────────────────────────────────────────────────────────────┘
                              │ PCIe Bus
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│                           Linux端                                   │
├─────────────────────────────────────────────────────────────────────┤
│  1. pci_alloc_consistent()分配4MB+ DMA缓冲区                         │
│  2. 将物理地址写入FPGA寄存器0x110/0x120                              │
│  3. 等待FPGA完成一帧传输（可通过中断或轮询）                           │
│  4. 从DMA缓冲区读取数据到用户态                                      │
│  5. RGB565 → RGB888颜色空间转换                                      │
│  6. 写入帧缓冲设备/dev/fb0                                         │
│  7. RK3568 GPU显示到屏幕                                            │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. 关键修改点（适配RK3568）

### 6.1 PCIe事务层详解

#### 6.1.1 PCIe分层架构

PCIe协议分为三层，理解这些层次对于分析是否需要修改驱动至关重要：

```
┌──────────────────────────────────────────────────────────────┐
│                    PCIe分层架构                               │
├──────────────────────────────────────────────────────────────┤
│  应用层（Application Layer）                                 │
│  ├── FPGA端：彩条生成、HDMI数据采集                           │
│  └── Linux端：应用程序、显示驱动                              │
├──────────────────────────────────────────────────────────────┤
│  事务层（Transaction Layer）                                  │
│  ├── TLP（Transaction Layer Packet）                        │
│  │   ├── MWR（Memory Write Request）                        │
│  │   ├── MRd（Memory Read Request）                         │
│  │   ├── Cpl（Completion）                                  │
│  │   └── Configuration Requests                            │
│  ├── TLP组装/解析                                            │
│  ├── Flow Control                                           │
│  └── Retry Mechanism                                        │
├──────────────────────────────────────────────────────────────┤
│  数据链路层（Data Link Layer）                                │
│  ├── DLLP（Data Link Layer Packet）                         │
│  ├── CRC校验                                                │
│  ├── ACK/NAK确认                                            │
│  └── Error Detection                                        │
├──────────────────────────────────────────────────────────────┤
│  物理层（Physical Layer）                                    │
│  ├── SerDes编码/解码（8b/10b）                               │
│  ├── Lane Management                                         │
│  ├── Clock Recovery                                          │
│  └── Electrical signaling                                   │
└──────────────────────────────────────────────────────────────┘
```

#### 6.1.2 FPGA端事务层实现

根据 `pcie_dma_ctrl.v` 的分析，FPGA端的事务层实现如下：

**TLP类型**：MWR（Memory Write Request）

```verilog
localparam MWR_32    = 8'h40;  // Fmt=010, Type=00000
```

**TLP格式**（3DW MWR）：

```
┌──────────────────────────────────────────────────────────────┐
│                    MWR TLP 结构                              │
├──────────────────────────────────────────────────────────────┤
│  DW0: Fmt[31:29]=010, Type[28:24]=00000, TC[23:20], Attr   │
│       Length[9:0], AT[11:10], Attr[13:12], EP[14], TD[15]  │
├──────────────────────────────────────────────────────────────┤
│  DW1: First DW BE[35:32], Last DW BE[39:36], Tag[47:40]    │
│       Requester ID[63:48] = {Bus, Device, Function}         │
├──────────────────────────────────────────────────────────────┤
│  DW2: Address[95:64] - 32位地址（低30位有效）                │
├──────────────────────────────────────────────────────────────┤
│  DW3+: Data Payload（0-1024 DWORD）                          │
└──────────────────────────────────────────────────────────────┘
```

**FPGA端关键参数**：

| 参数 | 值 | 说明 |
|-----|-----|------|
| **TLP_LENGTH** | 16 DWORD | 每次TLP携带64字节数据 |
| **DMA_TRAN_TIMES** | 64801 | 一帧需要发送的TLP数量 |
| **帧大小** | 1920×1080×2 = 4,147,200字节 | RGB565格式 |
| **地址数量** | 4个（dma_addr0-3） | 循环缓冲区地址 |

**FPGA端发送流程**：

```verilog
// pcie_dma_ctrl.v 第247-375行
// 状态机：MWR_IDLE → MWR_TLP_HEADER → MWR_TLP_DATA → MWR_IDLE

// 1. 接收VS信号（帧同步）
if(!vs_in_d0 && vs_in_d1 && rc_cfg_ep_flag) begin
    fram_start <= 'd1;  // 帧开始
end

// 2. 构建TLP头部
r_axis_s_tdata[31:29] <= 3'b010;        // Fmt: 3DW MWR
r_axis_s_tdata[9:0]   <= TLP_LENGTH;    // 16 DWORD
r_axis_s_tdata[95:64] <= alloc_addrl;   // 目标地址

// 3. 发送数据payload
r_axis_s_tdata <= pcie_dma_data;        // 从FIFO读取的RGB565数据

// 4. 地址递增
alloc_addrl <= alloc_addrl + TLP_LENGTH*4;  // 每次+64字节
```

#### 6.1.3 Linux端事务层处理

Linux内核的PCIe事务层由**内核PCI子系统**处理，应用程序和驱动不需要关心：

| 层次 | 处理方 | 说明 |
|-----|--------|------|
| **物理层** | 硬件 + 内核PCIe驱动 | SerDes、Lane管理 |
| **数据链路层** | 内核PCIe驱动 | CRC、ACK/NAK |
| **事务层** | 内核PCIe驱动 | TLP解析、Flow Control |
| **应用层** | 用户驱动 + 应用程序 | 数据处理、显示 |

**结论：PCIe事务层不需要修改！**

原因：
1. Linux内核的PCIe子系统已经完整实现了事务层协议
2. FPGA端使用标准的MWR TLP，Linux内核可以正确处理
3. 驱动只需处理应用层的数据传递，不需要关心TLP格式

### 6.2 当前代码的限制

当前代码是为通用Linux环境编写的，在RK3568上使用时需要注意以下问题：

| 问题 | 原因 | 影响 |
|-----|------|-----|
| **64位DMA支持** | RK3568支持64位DMA | 当前代码已有支持，但需确认内核配置 |
| **PCIe时钟** | RK3568 PCIe Gen2 x1 | 需确保设备树配置正确 |
| **GPU显示** | 需要将图像数据传给RK3568的GPU | 当前代码没有显示功能 |
| **DMA缓冲区大小** | `DMA_MAX_PACKET_SIZE=4096` | 不足以存储完整帧（4MB） |
| **FPGA自主传输** | FPGA端采用自主发送模式 | Linux端只需设置地址，无需触发 |
| **用户态缓冲区限制** | `DMA_DATA.read_buf[4096]` | 用户态单次最多读取4KB数据 |
| **地址配置方式** | FPGA需要4个地址 | 当前驱动只配置1个地址 |

### 6.3 驱动是否需要修改？

#### 6.3.1 核心问题分析

**结合FPGA端代码重新分析**：

根据 `pcie_dma_ctrl.v` 的分析，FPGA端使用4个32位地址（dma_addr0-3）进行循环传输：

```verilog
// pcie_dma_ctrl.v 第157-180行
case(alloc_addr_state)
    ALLOC_ADDR_1: dma_addr0 <= {axis_master_tdata_d0[7:0],...};
    ALLOC_ADDR_2: dma_addr1 <= {axis_master_tdata_d0[7:0],...};
    ALLOC_ADDR_3: dma_addr2 <= {axis_master_tdata_d0[7:0],...};
    ALLOC_ADDR_4: dma_addr3 <= {axis_master_tdata_d0[7:0],...};
endcase

// 每帧使用一个地址，循环切换
if(addr_page == 0) alloc_addrl <= dma_addr0;
if(addr_page == 1) alloc_addrl <= dma_addr1;
if(addr_page == 2) alloc_addrl <= dma_addr2;
if(addr_page == 3) alloc_addrl <= dma_addr3;
```

**结论：驱动需要修改，有三种使用方式**

| 方式 | 是否需要修改驱动 | 优缺点 |
|-----|-----------------|-------|
| **方式A：分块传输** | 不需要 | 用户态分多次读取，每次4KB，但无法支持4地址循环 |
| **方式B：mmap映射** | 需要 | 用户态直接访问DMA缓冲区，性能最优 |
| **方式C：支持4地址循环** | 需要 | 完全匹配FPGA端的4地址循环模式 |

**关键代码分析**：

```c
// pango_pci_driver.h 第151-155行
typedef struct _DMA_DATA_
{
    unsigned char read_buf[DMA_MAX_PACKET_SIZE];  // 固定4KB
    unsigned char write_buf[DMA_MAX_PACKET_SIZE]; // 固定4KB
}DMA_DATA;

// pango_pci_driver.c 第404-405行
data_buf_r = pci_alloc_consistent(op_dev, dma_operation.current_len*4, &dma_addr_r);
data_buf_w = pci_alloc_consistent(op_dev, dma_operation.current_len*4, &dma_addr_w);
```

**分析**：
- 内核中的 `pci_alloc_consistent` 可以分配任意大小的内存（只要内存足够）
- 但是用户态通过 `DMA_OPERATION.data.read_buf` 读取数据时，受限于4KB的固定缓冲区
- `PCI_READ_FROM_KERNEL_CMD` 使用 `memcpy(dma_operation.data.read_buf, dma_info.addr_w.data_buf, dma_operation.current_len*4)` 复制数据
- 如果 `current_len*4 > 4096`，会发生缓冲区溢出！
- 当前驱动只支持1个地址配置，而FPGA端需要4个地址

#### 6.3.2 方案对比

**方案一：分块传输（驱动不改）**

```
用户态逻辑：
1. ioctl(PCI_MAP_ADDR_CMD, 4MB)  // 分配4MB DMA缓冲区
2. ioctl(PCI_DMA_WRITE_CMD)      // 设置FPGA目标地址
3. 循环1024次：
   - ioctl(PCI_READ_FROM_KERNEL_CMD, offset=0~4MB-4KB)  // 每次读4KB
   - 将数据拼接成完整帧
4. 显示图像
```

**优点**：驱动无需修改，风险低
**缺点**：需要1024次ioctl调用，效率低，延迟高

**方案二：添加mmap支持（驱动需要修改）**

```
用户态逻辑：
1. ioctl(PCI_MAP_ADDR_CMD, 4MB)  // 分配4MB DMA缓冲区
2. ioctl(PCI_DMA_WRITE_CMD)      // 设置FPGA目标地址
3. mmap(fd, 4MB, ...)            // 直接映射DMA缓冲区到用户态
4. 等待FPGA传输完成
5. 直接从mmap地址读取4MB数据
6. 显示图像
```

**优点**：只需1次映射，效率高，零拷贝
**缺点**：驱动需要添加mmap实现

**方案三：支持4地址循环（推荐，完全匹配FPGA端）**

```c
// 修改 pango_pci_driver.h
// 添加4个DMA地址支持
#define DMA_BUFFER_COUNT      4
#define DMA_FRAME_SIZE        (1920 * 1080 * 2)  // 4MB

typedef struct _DMA_INFO_
{
    DMA_CMD cmd;
    DMA_ADDR addr_r[DMA_BUFFER_COUNT];  // 4个读地址
    DMA_ADDR addr_w[DMA_BUFFER_COUNT];  // 4个写地址
    unsigned int current_buffer;        // 当前缓冲区索引
}DMA_INFO;

// 添加新的IOCTL命令
#define PCI_MAP_ADDR_CMD_0    _IOWR(TYPE, 2, int)   // 映射缓冲区0
#define PCI_MAP_ADDR_CMD_1    _IOWR(TYPE, 13, int)  // 映射缓冲区1
#define PCI_MAP_ADDR_CMD_2    _IOWR(TYPE, 14, int)  // 映射缓冲区2
#define PCI_MAP_ADDR_CMD_3    _IOWR(TYPE, 15, int)  // 映射缓冲区3
#define PCI_GET_FRAME_CMD     _IOWR(TYPE, 16, int)  // 获取当前帧缓冲区
```

**驱动修改要点**：
1. 分配4个独立的4MB DMA缓冲区
2. 分别设置4个地址到FPGA（通过BAR空间）
3. 添加帧同步机制，通知应用程序哪个缓冲区有新数据

**优点**：完全匹配FPGA端的4地址循环模式，实现真正的双缓冲/四缓冲
**缺点**：需要分配16MB连续内存，对内存要求较高

#### 6.3.3 推荐方案

**推荐方案三（4地址循环 + mmap方式）**，原因：
1. **完全匹配FPGA**：与FPGA端的4地址循环模式完全兼容
2. **零拷贝**：使用mmap直接访问物理内存
3. **高性能**：支持真正的双缓冲，帧率可达30fps
4. **标准方式**：Linux设备驱动的标准做法

### 6.4 驱动层面修改

#### 6.4.1 增加4地址循环支持（推荐方案）

**修改文件**：`driver/pango_pci_driver.c` 和 `driver/pango_pci_driver.h`

**步骤1：修改数据结构，支持4地址循环**

```c
// pango_pci_driver.h 修改
#define DMA_BUFFER_COUNT      4
#define DMA_FRAME_SIZE        (1920 * 1080 * 2)  // 4MB

typedef struct _DMA_INFO_
{
    DMA_CMD cmd;
    DMA_ADDR addr_r[DMA_BUFFER_COUNT];  // 4个读地址
    DMA_ADDR addr_w[DMA_BUFFER_COUNT];  // 4个写地址
    unsigned int current_buffer;        // 当前缓冲区索引
    spinlock_t buffer_lock;             // 缓冲区锁
}DMA_INFO;

// 添加新的IOCTL命令
#define PCI_MAP_ADDR_CMD_0    _IOWR(TYPE, 2, int)   // 映射缓冲区0
#define PCI_MAP_ADDR_CMD_1    _IOWR(TYPE, 13, int)  // 映射缓冲区1
#define PCI_MAP_ADDR_CMD_2    _IOWR(TYPE, 14, int)  // 映射缓冲区2
#define PCI_MAP_ADDR_CMD_3    _IOWR(TYPE, 15, int)  // 映射缓冲区3
#define PCI_GET_FRAME_CMD     _IOWR(TYPE, 16, int)  // 获取当前帧缓冲区
```

**步骤2：修改DMA内存分配逻辑**

```c
// pango_pci_driver.c 修改
// 修改PCI_MAP_ADDR_CMD处理逻辑，支持4个缓冲区
case PCI_MAP_ADDR_CMD:
    // 分配4个4MB的DMA缓冲区
    int i;
    for(i = 0; i < DMA_BUFFER_COUNT; i++) {
        dma_info.addr_w[i].data_buf = pci_alloc_consistent(op_dev, DMA_FRAME_SIZE, 
                                                           &dma_info.addr_w[i].addr);
        if(!dma_info.addr_w[i].data_buf) {
            // 失败时释放已分配的缓冲区
            for(j = 0; j < i; j++) {
                pci_free_consistent(op_dev, DMA_FRAME_SIZE, 
                                   dma_info.addr_w[j].data_buf, 
                                   dma_info.addr_w[j].addr);
            }
            ret = -ENOMEM;
            break;
        }
        dma_info.addr_w[i].addr_size = (dma_info.addr_w[i].addr >> 32) > 0 ? 1 : 0;
        printk("DMA buffer %d: addr=0x%llx\n", i, dma_info.addr_w[i].addr);
    }
break;
```

**步骤3：修改地址设置逻辑，设置4个地址到FPGA**

```c
// pango_pci_driver.c 修改
// 修改PCI_DMA_WRITE_CMD处理逻辑，设置4个地址
case PCI_DMA_WRITE_CMD:
    dma_info.cmd.data.op_type = 1;  // DMA写操作
    
    // 设置4个地址到FPGA的BAR空间
    // 地址偏移：0x110, 0x114, 0x118, 0x11C
    for(i = 0; i < DMA_BUFFER_COUNT; i++) {
        iowrite32((u32)(dma_info.addr_w[i].addr & 0xFFFFFFFF), 
                  pci_pango->_pango_pci_driver._pci_io + 0x110 + i*4);
    }
    
    // 设置命令寄存器
    set_dma_w_r(dma_info.cmd.value, pci_pango);
break;
```

#### 6.4.2 增加帧同步支持

**添加新的IOCTL命令**（`pango_pci_driver.h`）：

```c
#define PCI_DMA_FRAME_START_CMD     _IOWR(TYPE, 10, int)  // 开始帧传输
#define PCI_DMA_FRAME_WAIT_CMD      _IOWR(TYPE, 11, int)  // 等待帧完成
#define PCI_DMA_FRAME_STOP_CMD      _IOWR(TYPE, 12, int)  // 停止帧传输
```

**实现帧等待逻辑**（`pango_pci_driver.c`）：

```c
case PCI_DMA_FRAME_WAIT_CMD:
    // 轮询FPGA状态寄存器或等待中断
    // 这里使用简单的轮询方式
    unsigned int timeout = 100000;  // 100ms超时
    while(timeout--) {
        unsigned int status = ioread32(pci_pango->_pango_pci_driver._pci_io + STATUS_REG_OFFSET);
        if(status & FRAME_COMPLETE_FLAG) {
            break;
        }
        udelay(1);
    }
break;
```

#### 6.4.3 添加mmap实现（关键修改）

**修改文件**：`driver/pango_pci_driver.c`

```c
// 在 file_operations 结构体中添加 mmap
static const struct file_operations pci_fops = {
    .owner   = THIS_MODULE,
    .open    = pango_cdev_open,
    .release = pango_cdev_release,
    .read    = pango_cdev_read,
    .write   = pango_cdev_write,
    .llseek  = pango_cdev_llseek,
    .unlocked_ioctl = pango_cdev_ioctl,
    .mmap    = pango_cdev_mmap,  // 添加mmap
};

// 实现mmap函数
static int pango_cdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct PciPango *pci_pango = filp->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    
    // 使用dma_info.addr_w作为DMA缓冲区（FPGA写入）
    if(size > dma_operation.current_len * 4) {
        return -EINVAL;
    }
    
    // 设置页面保护属性
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    
    // 映射物理地址到用户空间
    if(remap_pfn_range(vma, vma->vm_start, 
        dma_info.addr_w.addr >> PAGE_SHIFT,
        size, vma->vm_page_prot)) {
        return -EAGAIN;
    }
    
    return 0;
}
```

### 6.4 应用层面修改

#### 6.4.1 创建彩条显示应用

**创建新文件**：`app_pcie/sources/display_app.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <unistd.h>
#include "../includes/config_gui.h"

#define FRAME_WIDTH  1920
#define FRAME_HEIGHT 1080
#define FRAME_SIZE   (FRAME_WIDTH * FRAME_HEIGHT * 2)  // RGB565

// 帧缓冲信息
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
unsigned char *fb_buffer = NULL;
int fb_fd = -1;

// PCIe驱动文件描述符
int pci_fd = -1;

// RGB565转RGB888
void rgb565_to_rgb888(unsigned char *src, unsigned char *dst, int width, int height) {
    int i, j;
    unsigned short rgb565;
    unsigned char r, g, b;
    
    for(i = 0; i < height; i++) {
        for(j = 0; j < width; j++) {
            rgb565 = *(unsigned short *)(src + (i * width + j) * 2);
            r = ((rgb565 >> 11) & 0x1F) << 3;
            g = ((rgb565 >> 5) & 0x3F) << 2;
            b = (rgb565 & 0x1F) << 3;
            
            int dst_offset = (i * width + j) * 3;
            dst[dst_offset] = r;
            dst[dst_offset + 1] = g;
            dst[dst_offset + 2] = b;
        }
    }
}

// 打开帧缓冲
int open_framebuffer() {
    fb_fd = open("/dev/fb0", O_RDWR);
    if(fb_fd < 0) {
        perror("open /dev/fb0 failed");
        return -1;
    }
    
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
    
    fb_buffer = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if(fb_buffer == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }
    
    printf("Framebuffer: %dx%d, %d bits/pixel\n", 
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    printf("Framebuffer size: %ld bytes\n", finfo.smem_len);
    
    return 0;
}

// 显示一帧图像
void display_frame(unsigned char *rgb565_data) {
    if(vinfo.bits_per_pixel == 16) {
        // 直接写入RGB565格式
        memcpy(fb_buffer, rgb565_data, FRAME_SIZE);
    } else if(vinfo.bits_per_pixel == 24) {
        // RGB565转RGB888
        rgb565_to_rgb888(rgb565_data, fb_buffer, FRAME_WIDTH, FRAME_HEIGHT);
    } else if(vinfo.bits_per_pixel == 32) {
        // RGB565转RGB888（带Alpha）
        int i, j;
        unsigned short rgb565;
        unsigned char r, g, b;
        
        for(i = 0; i < FRAME_HEIGHT; i++) {
            for(j = 0; j < FRAME_WIDTH; j++) {
                rgb565 = *(unsigned short *)(rgb565_data + (i * FRAME_WIDTH + j) * 2);
                r = ((rgb565 >> 11) & 0x1F) << 3;
                g = ((rgb565 >> 5) & 0x3F) << 2;
                b = (rgb565 & 0x1F) << 3;
                
                int dst_offset = (i * FRAME_WIDTH + j) * 4;
                fb_buffer[dst_offset] = b;        // B
                fb_buffer[dst_offset + 1] = g;    // G
                fb_buffer[dst_offset + 2] = r;    // R
                fb_buffer[dst_offset + 3] = 0xFF; // Alpha
            }
        }
    }
}

// 使用mmap方式读取DMA缓冲区（推荐）
int main(int argc, char *argv[]) {
    DMA_OPERATION dma_oper;
    unsigned char *dma_buffer = NULL;  // mmap映射的DMA缓冲区
    
    // 打开帧缓冲
    if(open_framebuffer() < 0) {
        return -1;
    }
    
    // 打开PCIe驱动
    pci_fd = open(PCIE_DRIVER_FILE_PATH, O_RDWR);
    if(pci_fd < 0) {
        perror("open PCIe driver failed");
        return -1;
    }
    
    // 初始化DMA操作（注意：current_len单位是dword）
    dma_oper.current_len = FRAME_SIZE / 4;  // 4MB / 4 = 1,036,800 dword
    dma_oper.offset_addr = 0;
    
    // 1. 分配DMA缓冲区（内核分配4MB物理内存）
    ioctl(pci_fd, PCI_MAP_ADDR_CMD, &dma_oper);
    printf("DMA buffer allocated: %d bytes\n", FRAME_SIZE);
    
    // 2. 设置FPGA DMA目标地址（触发FPGA开始传输）
    ioctl(pci_fd, PCI_DMA_WRITE_CMD, &dma_oper);
    printf("FPGA DMA write started\n");
    
    // 3. mmap映射DMA缓冲区到用户空间（零拷贝）
    dma_buffer = mmap(NULL, FRAME_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, pci_fd, 0);
    if(dma_buffer == MAP_FAILED) {
        perror("mmap DMA buffer failed");
        return -1;
    }
    printf("DMA buffer mapped to user space: %p\n", dma_buffer);
    
    // 4. 循环接收并显示帧
    while(1) {
        // 等待一帧完成（这里简化为固定延时，实际应使用中断或轮询状态寄存器）
        usleep(100000);  // 100ms
        
        // 直接从mmap地址读取数据（零拷贝，无需ioctl）
        // dma_buffer中已经是FPGA写入的RGB565数据
        
        // 显示图像
        display_frame(dma_buffer);
        
        printf("Frame displayed\n");
        
        // 可以在这里添加退出条件
        // if(quit) break;
    }
    
    // 5. 释放资源
    munmap(dma_buffer, FRAME_SIZE);
    ioctl(pci_fd, PCI_UMAP_ADDR_CMD, &dma_oper);
    close(pci_fd);
    munmap(fb_buffer, finfo.smem_len);
    close(fb_fd);
    
    return 0;
}
```

**修改Makefile**：

```makefile
# 添加新目标
display_app:
    $(CC) -o build/display_app sources/display_app.c $(CFLAGS) $(LDFLAGS)
```

#### 6.4.2 修改现有GUI应用（可选）

在现有GUI中添加一个新的"Display"页面，用于显示从FPGA接收的彩条图像。

### 6.5 设备树配置（RK3568）

**重要结论：设备树不需要修改！**

根据现有文档 `linux_host_operation_after_link_up.md` 的分析，RK3568的PCIe已经成功枚举FPGA设备，说明设备树配置已经正确：

```text
dmesg | grep PCIe
rk-pcie 3c0000000.pcie: PCIe Link up, LTSSM is 0x130011
rk-pcie 3c0000000.pcie: PCI host bridge to bus 0000:00
pci 0000:01:00.0: [0755:0755] type 00 class 0x058000
pci 0000:01:00.0: BAR 0: assigned [mem 0xf4200000-0xf4201fff] (8KB)
pci 0000:01:00.0: BAR 1: assigned [mem 0xf4204000-0xf4204fff] (4KB)
pci 0000:01:00.0: BAR 2: assigned [mem 0xf4202000-0xf4203fff 64bit] (8KB)
```

**验证命令**：
```bash
lspci -nn
# 应显示：01:00.0 Memory controller [0580]: Device [0755:0755]
```

**为什么不需要修改设备树？**

| 原因 | 说明 |
|-----|------|
| **PCIe链路已建立** | `PCIe Link up` 说明物理链路正常 |
| **设备已枚举** | `lspci` 能看到FPGA设备（0755:0755） |
| **BAR空间已分配** | BAR0/BAR1/BAR2都已分配地址 |
| **驱动能probe** | `pci_driver_probe()` 已成功调用 |
| **字符设备已创建** | `/dev/pango_pci_driver` 已存在 |

**设备树配置回顾**（仅供参考，不需要修改）：

```dts
pcie30_phy: pcie-phy@fe6c0000 {
    status = "okay";
};

pcie30: pcie@fe6c0000 {
    status = "okay";
    num-lanes = <1>;
    max-link-speed = <2>;  // Gen2
    bus-range = <0x00 0xff>;
    ranges = <0x82000000 0x0 0x00000000 0x0 0x00000000 0x0 0x10000000>;
};
```

### 6.6 修改验证步骤

| 步骤 | 操作 | 验证方法 |
|-----|------|---------|
| 1 | 编译驱动 | `make` 生成 `pango_pci_driver.ko` |
| 2 | 加载驱动 | `insmod pango_pci_driver.ko`，检查 `dmesg` |
| 3 | 确认设备 | `lspci -nn` 看到 `0755:0755` |
| 4 | 编译显示应用 | `make display_app` |
| 5 | 运行显示应用 | `./build/display_app` |
| 6 | 验证显示 | RK3568屏幕显示彩条图像 |

### 6.7 性能优化建议

| 优化项 | 说明 |
|-----|------|
| **使用中断** | 替换轮询方式，使用PCIe中断通知帧完成 |
| **双缓冲** | 使用两个DMA缓冲区交替接收，减少延迟 |
| **MMAP方式** | 直接映射DMA缓冲区到用户态，避免数据拷贝 |
| **DMA地址连续** | 确保DMA缓冲区物理地址连续 |
| **GPU加速** | 使用RK3568的GPU进行颜色空间转换 |

---

## 7. .ko文件构成与系统架构详解

### 7.1 什么是.ko文件

`.ko` 文件是 **Linux Kernel Object** 的缩写，即 Linux 内核模块。它是一种可以动态加载到 Linux 内核中运行的二进制文件，无需重新编译整个内核。

**.ko 文件的特点**：

| 特性 | 说明 |
|-----|------|
| **运行位置** | 内核态（Ring 0），拥有最高权限 |
| **加载方式** | `insmod` / `modprobe` 命令 |
| **卸载方式** | `rmmod` 命令 |
| **文件格式** | ELF（Executable and Linkable Format） |
| **依赖关系** | 依赖内核版本和符号表 |

### 7.2 .ko文件的构成

`pango_pci_driver.ko` 由以下部分构成：

```
┌─────────────────────────────────────────────────────────┐
│                    pango_pci_driver.ko                  │
├─────────────────────────────────────────────────────────┤
│  1. ELF Header（ELF头部）                                │
│     - 魔数、文件类型、机器架构、入口地址                   │
├─────────────────────────────────────────────────────────┤
│  2. Program Headers（程序段头）                          │
│     - 描述如何加载到内存                                 │
├─────────────────────────────────────────────────────────┤
│  3. Sections（段）                                       │
│     ├── .text        → 驱动代码（probe、remove、ioctl等） │
│     ├── .data        → 全局变量（dma_info等）             │
│     ├── .bss         → 未初始化全局变量                  │
│     ├── .rodata      → 只读数据（字符串常量）             │
│     ├── .modinfo     → 模块信息（版本、依赖、许可证）      │
│     └── .gnu.linkonce.this_module → 模块描述符          │
├─────────────────────────────────────────────────────────┤
│  4. Section Headers（段头）                              │
│     - 描述各个段的位置和大小                             │
└─────────────────────────────────────────────────────────┘
```

### 7.3 .ko文件包含的核心组件

**模块描述符**（`struct module`）：

```c
static struct pci_driver pango_pci_driver = {
    .name     = "pango_pci_driver",      // 模块名称
    .id_table = pci_ids,                 // PCI设备ID列表
    .probe    = pci_driver_probe,        // 设备探测函数
    .remove   = pci_driver_remove,       // 设备移除函数
#ifdef CONFIG_PM
    .suspend  = pci_driver_suspend,     // 挂起函数
    .resume   = pci_driver_resume,       // 恢复函数
#endif
};
```

**内核接口**（.ko提供给内核的接口）：

| 接口 | 函数名 | 作用 |
|-----|--------|------|
| **设备探测** | `pci_driver_probe()` | 发现PCI设备时调用 |
| **设备移除** | `pci_driver_remove()` | 移除PCI设备时调用 |
| **字符设备操作** | `pci_fops` | open/read/write/ioctl |
| **模块入口** | `module_init()` | 模块加载时执行 |
| **模块出口** | `module_exit()` | 模块卸载时执行 |

**用户态接口**（.ko提供给用户态的接口）：

| 接口 | 说明 |
|-----|------|
| `/dev/pango_pci_driver` | 字符设备文件 |
| `ioctl`命令 | PCI_MAP_ADDR_CMD、PCI_DMA_WRITE_CMD等 |
| 设备文件权限 | 由驱动在probe中设置 |

### 7.4 .ko文件的编译过程

**驱动Makefile**（`driver/Makefile`）：

```makefile
CONFIG_MODULE_SIG=n                    # 禁用模块签名
ifeq ($(KERNELRELEASE),)              # 第一次执行
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    PWD := $(shell pwd)
    modules:
        $(MAKE) -C $(KERNELDIR) M=$(PWD) modules  # 调用内核Makefile
    modules_install:
        $(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
    clean:
        rm -rf *.o *.ko *.mod.c .tmp_versions
else                                   # 内核Makefile调用时
    obj-m := pango_pci_driver.o        # 指定要编译的目标文件
endif
```

**编译流程**：

```
pango_pci_driver.c
        ↓
gcc -c -DMODULE -D__KERNEL__ -O2 -Wall pango_pci_driver.c
        ↓
pango_pci_driver.o（目标文件）
        ↓
ld -r -o pango_pci_driver.ko pango_pci_driver.o
        ↓
pango_pci_driver.ko（内核模块）
```

**编译依赖**：

- **内核源码**：必须使用与目标系统相同版本的内核源码
- **内核配置**：必须启用 `CONFIG_MODULES` 等相关配置
- **交叉编译器**：在x86主机上编译ARM64驱动时需要使用交叉编译器

### 7.5 app（用户态程序）与.ko（内核模块）的关系

**架构层次图**：

```
┌─────────────────────────────────────────────────────────────────┐
│                    用户态（User Space）                          │
│  ┌─────────────────────┐  ┌─────────────────────────────┐       │
│  │     app（GTK GUI）  │  │  display_app（图像显示）     │       │
│  │  - 测试界面         │  │  - 帧缓冲操作               │       │
│  │  - DMA参数配置      │  │  - RGB565→RGB888转换        │       │
│  │  - 数据验证         │  │  - 图像渲染                 │       │
│  └─────────┬───────────┘  └──────────────┬──────────────┘       │
│            │                             │                      │
│            ▼                             ▼                      │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    /dev/pango_pci_driver                 │   │
│  │              字符设备文件（用户态与内核态的桥梁）           │   │
│  └─────────────────────────────┬────────────────────────────┘   │
│                                │ ioctl / read / write           │
└────────────────────────────────┼────────────────────────────────┘
                                 │
┌────────────────────────────────▼────────────────────────────────┐
│                    内核态（Kernel Space）                        │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │              pango_pci_driver.ko                         │   │
│  │  ┌─────────┐  ┌─────────┐  ┌───────────────┐            │   │
│  │  │ PCI驱动 │  │ DMA引擎 │  │ 字符设备接口  │            │   │
│  │  │ -probe  │  │ -内存分配│  │ -open/ioctl  │            │   │
│  │  │ -BAR映射│  │ -地址映射│  │ -read/write  │            │   │
│  │  │ -中断处理│  │ -传输控制│  │ -mmap        │            │   │
│  │  └─────────┘  └─────────┘  └───────────────┘            │   │
│  └──────────────────────────────────────────────────────────┘   │
└────────────────────────────────┬────────────────────────────────┘
                                 │ PCIe总线
┌────────────────────────────────▼────────────────────────────────┐
│                    FPGA（PCIe Endpoint）                       │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  PCIe Core  │  DMA Controller  │  DDR Memory  │ 彩条生成 │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

**交互流程**：

```
用户操作 → app → ioctl命令 → .ko → PCIe → FPGA
                                     ↓
FPGA响应 → PCIe → .ko → ioctl返回 → app → 显示结果
```

**通信方式**：

| 方式 | 函数 | 用途 |
|-----|------|-----|
| **ioctl** | `ioctl(fd, cmd, arg)` | 发送控制命令（映射、DMA等） |
| **read** | `read(fd, buf, len)` | 从内核读取数据 |
| **write** | `write(fd, buf, len)` | 向内核写入数据 |
| **mmap** | `mmap(NULL, len, ..., fd, 0)` | 映射内核内存到用户态 |

### 7.6 run.sh脚本解析

**run.sh的作用**：自动化编译和运行整个工程

**脚本流程**：

```
┌─────────────────────────────────────────────────────┐
│                   run.sh 执行流程                    │
├─────────────────────────────────────────────────────┤
│  1. 检查当前用户是否为root                            │
│     └── 非root → 提示切换用户 → 退出                  │
│                                                      │
│  2. 检查驱动是否已加载                                │
│     ├── 已加载 → 跳过编译和加载                       │
│     └── 未加载 → 编译驱动 → insmod加载                │
│                                                      │
│  3. 检查PCI设备是否枚举                               │
│     └── 未枚举 → 提示检查FPGA → 退出                  │
│                                                      │
│  4. 编译应用程序                                      │
│     └── cd app_pcie → make clean → make              │
│                                                      │
│  5. 运行应用程序                                      │
│     └── cd build → ./app                             │
│                                                      │
│  6. 退出                                              │
└─────────────────────────────────────────────────────┘
```

**脚本关键代码解析**：

```bash
# 检查PCI设备是否枚举
check_pci_endpoint() {
    for dev in /sys/bus/pci/devices/*
    do
        [ -e "$dev/vendor" ] || continue
        vendor=$(cat "$dev/vendor")
        device=$(cat "$dev/device")
        if [ "$vendor" = "$pci_vendor" ] && [ "$device" = "$pci_device" ]
        then
            return 0  # 找到设备
        fi
    done
    return 1  # 未找到设备
}

# 编译和加载驱动
cd ./driver
make clean || exit 1
make || exit 1
insmod $driver.ko  # 加载内核模块

# 编译和运行应用
cd ./app_pcie
make clean || exit 1
make || exit 1
cd ./build
./$target  # 运行用户态程序
```

### 7.7 display_app.c的使用方式

**display_app.c 是用户态程序，不是内核模块！**

| 项目 | display_app | pango_pci_driver.ko |
|-----|-------------|---------------------|
| **类型** | 用户态可执行程序 | 内核模块 |
| **运行位置** | 用户态（Ring 3） | 内核态（Ring 0） |
| **编译方式** | `gcc` 直接编译 | 内核Makefile编译 |
| **运行方式** | 直接执行 `./display_app` | `insmod` 加载 |
| **权限要求** | 需要root权限（访问/dev/fb0） | 需要root权限 |
| **依赖关系** | 依赖.ko已加载 | 依赖内核版本 |

**display_app的工作流程**：

```
1. 打开帧缓冲设备 /dev/fb0
2. mmap映射帧缓冲内存
3. 打开PCIe驱动 /dev/pango_pci_driver
4. ioctl分配DMA缓冲区（4MB）
5. ioctl设置FPGA DMA目标地址
6. 循环：
   - 等待FPGA传输完成
   - ioctl读取DMA缓冲区数据
   - RGB565→RGB888转换
   - 写入帧缓冲显示
7. 释放资源
```

**编译和运行步骤**：

```bash
# 1. 确保驱动已加载
insmod pango_pci_driver.ko

# 2. 编译display_app（需要先修改Makefile添加目标）
cd app_pcie
make display_app

# 3. 运行display_app（需要root权限）
./build/display_app
```

### 7.8 完整的系统启动流程

```
┌──────────────────────────────────────────────────────────────────┐
│                      RK3568启动流程                              │
├──────────────────────────────────────────────────────────────────┤
│  1. 上电启动                                                      │
│     └── U-Boot → Linux内核启动                                    │
│                                                                  │
│  2. PCIe初始化                                                    │
│     └── 内核枚举PCIe设备 → 发现FPGA（Vendor ID: 0x0755）           │
│                                                                  │
│  3. 加载驱动（手动或run.sh）                                       │
│     └── insmod pango_pci_driver.ko                               │
│         ├── pci_driver_probe() 被调用                            │
│         ├── 映射BAR空间                                           │
│         ├── 分配DMA内存                                            │
│         ├── 创建字符设备 /dev/pango_pci_driver                    │
│         └── 打印设备信息到dmesg                                   │
│                                                                  │
│  4. 运行应用（手动或run.sh）                                       │
│     └── ./app 或 ./display_app                                   │
│         ├── open("/dev/pango_pci_driver")                        │
│         ├── ioctl() 设置DMA参数                                   │
│         ├── ioctl() 触发DMA传输                                   │
│         └── 显示测试结果或图像                                     │
│                                                                  │
│  5. FPGA自主传输（彩条模式）                                       │
│     └── FPGA持续发送彩条数据到RK3568的DMA缓冲区                    │
│         └── display_app循环读取并显示                             │
└──────────────────────────────────────────────────────────────────┘
```

---

## 8. 编译与部署

### 8.1 驱动编译

```bash
# 在RK3568开发板上或交叉编译环境中
cd driver/
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# 生成 pango_pci_driver.ko
```

**Makefile关键配置**：

```makefile
obj-m := pango_pci_driver.o
KDIR := /path/to/rk3568/kernel/source
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
```

### 8.2 应用编译

```bash
cd app_pcie/
make

# 生成 build/app
```

**Makefile关键配置**：

```makefile
CC = aarch64-linux-gnu-gcc
CFLAGS = -Wall `pkg-config --cflags gtk+-2.0`
LDFLAGS = `pkg-config --libs gtk+-2.0` -lpthread

all:
    $(CC) -o build/app sources/main.c $(CFLAGS) $(LDFLAGS)
```

### 8.3 部署步骤

```bash
# 1. 加载驱动
insmod pango_pci_driver.ko

# 2. 检查设备是否识别
lspci | grep Pango

# 3. 运行应用
./build/app

# 4. 如果需要显示，确保帧缓冲可用
ls /dev/fb0
```

---

## 9. 文件清单

### 9.1 驱动文件

| 文件 | 说明 |
|-----|------|
| `pango_pci_driver.c` | 主驱动实现 |
| `pango_pci_driver.h` | 驱动头文件（数据结构、宏定义） |
| `id_config.h` | Vendor ID/Device ID配置 |
| `Makefile` | 驱动编译脚本 |
| `pango_pci_driver.ko` | 编译后的内核模块 |

### 9.2 应用文件

| 文件 | 说明 |
|-----|------|
| `sources/main.c` | 主应用实现（GTK界面、测试逻辑） |
| `includes/config_gui.h` | 应用配置和数据结构定义 |
| `includes/color.h` | 终端颜色定义 |
| `Makefile` | 应用编译脚本 |
| `build/app` | 编译后的可执行文件 |

### 9.3 文档文件

| 文件 | 说明 |
|-----|------|
| `PCIe测试使用说明_v1.0.pdf` | PCIe测试工具使用手册 |
| `linux_host_operation_after_link_up.md` | Linux主机操作指南 |
| `pango_pcie_test_gui_dma_guide.md` | GUI测试指南 |
| `pcie_driver_link_debug_summary.md` | 驱动调试总结 |

---

## 10. 总结

### 10.1 当前功能

本工程实现了一个完整的PCIe DMA测试工具，支持：

- ✅ PCI设备探测和配置空间读写
- ✅ DMA内存分配和地址映射（32位/64位）
- ✅ DMA读写数据传输
- ✅ PIO寄存器读写测试
- ✅ 性能测试（吞吐量计算）
- ✅ Tandem位流加载
- ✅ GTK图形界面操作

### 10.2 待完善功能（适配RK3568）

为了在RK3568上实现FPGA彩条图像显示，需要：

| 功能 | 优先级 | 说明 |
|-----|-------|------|
| 增加大缓冲区支持 | 高 | 需要支持4MB以上的帧缓冲 |
| 实现图像显示 | 高 | 使用fbdev或DRM显示RGB565图像 |
| 连续帧接收 | 中 | 支持视频流传输 |
| 中断支持 | 低 | 使用中断替代轮询 |
| 设备树配置 | 高 | 确保RK3568 PCIe正确配置 |

### 10.3 扩展方向

1. **视频流传输**：支持连续帧传输，实现实时视频显示
2. **多通道支持**：支持多路视频数据同时传输
3. **JPEG压缩**：在FPGA端压缩后传输，降低带宽需求
4. **网络转发**：将图像数据通过网络传输到其他设备

---

## 11. RK端适配FPGA彩条显示完整步骤总结

### 11.1 整体流程概览

```
┌─────────────────────────────────────────────────────────────────┐
│                   RK3568适配FPGA彩条显示                        │
├─────────────────────────────────────────────────────────────────┤
│  FPGA端                          RK3568端                        │
│  ┌──────────────────────┐       ┌──────────────────────────┐    │
│  │ 彩条生成 → HDMI采集  │──────→│ PCIe接收 → 帧缓冲显示    │    │
│  │ → FIFO → MWR TLP    │       │ → UI界面显示            │    │
│  └──────────────────────┘       └──────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

### 11.2 步骤详解

#### 步骤1：确认PCIe链路状态（无需修改设备树）

**设备树已正确配置，PCIe链路已建立！**

**验证命令**：
```bash
lspci -nn
# 应显示：01:00.0 Memory controller [0580]: Device [0755:0755]

dmesg | grep PCIe
# 应显示：PCIe Link up, LTSSM is 0x130011
```

**确认条件**：
| 检查项 | 预期结果 |
|-------|---------|
| PCIe链路 | `PCIe Link up` |
| 设备枚举 | `lspci` 看到 `0755:0755` |
| BAR空间 | BAR0/BAR1/BAR2已分配 |
| 驱动probe | `dmesg` 看到 `pci_driver_probe` |

#### 步骤2：修改内核驱动（支持4地址循环 + mmap）

**文件**：`driver/pango_pci_driver.c` 和 `driver/pango_pci_driver.h`

**修改内容**：

| 修改项 | 说明 |
|-------|------|
| **数据结构** | `DMA_INFO` 改为4个地址数组 |
| **内存分配** | 分配4个4MB DMA缓冲区 |
| **地址设置** | 设置4个地址到FPGA BAR空间（0x110-0x11C） |
| **mmap支持** | 添加 `pango_cdev_mmap` 函数 |
| **帧同步** | 添加 `PCI_DMA_FRAME_WAIT_CMD` |

**编译**：
```bash
cd driver/
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

#### 步骤3：加载驱动

```bash
# 加载驱动
insmod pango_pci_driver.ko

# 验证驱动加载
lsmod | grep pango_pci_driver

# 验证设备枚举
lspci -nn | grep 0755:0755

# 验证字符设备
ls /dev/pango_pci_driver
```

#### 步骤4：创建显示应用（display_app.c）

**文件**：`app_pcie/sources/display_app.c`

**核心功能**：

```c
// 1. 打开帧缓冲
int fb_fd = open("/dev/fb0", O_RDWR);
fb_buffer = mmap(NULL, finfo.smem_len, ...);

// 2. 打开PCIe驱动
int pci_fd = open("/dev/pango_pci_driver", O_RDWR);

// 3. 分配4个DMA缓冲区（每个4MB）
ioctl(pci_fd, PCI_MAP_ADDR_CMD, &dma_oper);

// 4. 设置FPGA目标地址
ioctl(pci_fd, PCI_DMA_WRITE_CMD, &dma_oper);

// 5. mmap映射DMA缓冲区
unsigned char *dma_buf = mmap(NULL, DMA_FRAME_SIZE*4, ...);

// 6. 循环显示
while(1) {
    // 等待帧完成
    ioctl(pci_fd, PCI_DMA_FRAME_WAIT_CMD, &frame_info);
    
    // 获取当前帧缓冲区索引
    ioctl(pci_fd, PCI_GET_FRAME_CMD, &frame_info);
    
    // 直接从mmap地址读取数据（零拷贝）
    display_frame(dma_buf + frame_info.buffer_idx * DMA_FRAME_SIZE);
}
```

**编译**：
```bash
cd app_pcie/
make display_app
```

#### 步骤5：创建上位机UI界面（可选但推荐）

**方案一：基于现有GTK界面扩展**

修改 `sources/main.c`，添加一个"Display"页面：

```c
// 添加新页面
GtkWidget *display_page = gtk_notebook_append_page(notebook, display_vbox, 
                                                    gtk_label_new("彩条显示"));

// 添加图像显示区域
GtkWidget *image_area = gtk_drawing_area_new();
gtk_widget_set_size_request(image_area, 1920, 1080);
gtk_container_add(GTK_CONTAINER(display_vbox), image_area);

// 添加控制按钮
GtkWidget *start_btn = gtk_button_new_with_label("开始显示");
g_signal_connect(start_btn, "clicked", G_CALLBACK(start_display), NULL);
gtk_container_add(GTK_CONTAINER(display_vbox), start_btn);
```

**方案二：基于Qt创建新界面**

创建新的Qt项目，使用QImage显示图像：

```c++
// 读取DMA缓冲区数据
QImage image(dma_buffer, FRAME_WIDTH, FRAME_HEIGHT, 
             FRAME_WIDTH*2, QImage::Format_RGB16);

// 转换为RGB888
QImage rgb888_image = image.convertToFormat(QImage::Format_RGB888);

// 显示到QLabel
ui->imageLabel->setPixmap(QPixmap::fromImage(rgb888_image));
```

**方案三：基于fbdev直接显示（无UI）**

使用framebuffer直接写入，最简单但无交互界面：

```c
// 直接写入帧缓冲
memcpy(fb_buffer, rgb565_data, FRAME_SIZE);
```

#### 步骤6：运行测试

```bash
# 1. 确保FPGA已加载位流
# 2. 加载驱动
insmod pango_pci_driver.ko

# 3. 运行显示应用
./build/display_app

# 或运行带UI的应用
./build/app
```

### 11.3 验证检查清单

| 检查项 | 命令/方法 | 预期结果 |
|-------|----------|---------|
| **PCIe链路** | `dmesg | grep PCIe` | `PCIe Link up, LTSSM is 0x130011` |
| **设备枚举** | `lspci -nn` | 显示 `[0755:0755]` |
| **驱动加载** | `lsmod | grep pango` | 显示 `pango_pci_driver` |
| **字符设备** | `ls /dev/pango*` | 显示 `/dev/pango_pci_driver` |
| **帧缓冲** | `ls /dev/fb0` | 显示 `/dev/fb0` |
| **图像显示** | 观察屏幕 | 显示彩条图像 |

### 11.4 常见问题及解决方案

| 问题 | 原因 | 解决方案 |
|-----|------|---------|
| **PCIe未枚举** | FPGA未加载位流 | 确保FPGA已加载PCIe Endpoint位流 |
| **驱动加载失败** | 内核版本不匹配 | 使用正确版本的内核源码编译 |
| **图像花屏** | 颜色格式不匹配 | 检查RGB565→RGB888转换逻辑 |
| **图像卡顿** | 缓冲区不够 | 增加缓冲区数量或使用双缓冲 |
| **内存分配失败** | 连续内存不足 | 配置大页内存或使用dma_alloc_coherent |

### 11.5 推荐配置

| 配置项 | 推荐值 | 说明 |
|-------|-------|------|
| **DMA缓冲区** | 4×4MB | 匹配FPGA端4地址循环 |
| **帧格式** | RGB565 | FPGA端输出格式 |
| **分辨率** | 1920×1080 | 全高清显示 |
| **刷新率** | 30fps | PCIe Gen2 x1带宽限制 |
| **显示方式** | fbdev + GTK/Qt | 兼顾性能和UI交互 |

---

**文档版本**：V1.0  
**创建日期**：2026-07-17  
**适用工程**：cat_pcie_project/pango_pcie_dma_alloc
