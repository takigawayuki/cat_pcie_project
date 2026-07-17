# Pango PCIe Test v1.0 界面与 DMA 测试使用说明

本文说明 `Pango PCIe Test v1.0` GTK 图形界面的使用方法，重点覆盖 PCIe 基础确认、Config/PIO 测试、DMA Manual、DMA Auto 和 Performance 测试。

该界面对应源码：

```text
pango_pcie_dma_alloc/app_pcie/sources/main.c
pango_pcie_dma_alloc/app_pcie/includes/config_gui.h
```

应用默认打开驱动节点：

```text
/dev/pango_pci_driver
```

因此运行 GUI 前必须先满足：

```text
1. Linux 已经通过 lspci 枚举到 FPGA PCIe Endpoint
2. pango_pci_driver.ko 已加载并 probe 成功
3. /dev/pango_pci_driver 已创建
4. 当前系统有图形桌面或可用 DISPLAY
```

## 1. 启动前检查

建议先在终端确认：

```sh
lspci -nn
lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver
```

期望看到：

```text
01:00.0 Memory controller [0580]: Device [0755:0755]
pango_pci_driver ...
/dev/pango_pci_driver
```

如果驱动还没加载，运行：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc
sudo ./run.sh
```

`run.sh` 会自动编译驱动、加载驱动、检查 PCIe Endpoint、编译应用并启动 GUI。

如果应用已经编译好，也可以手动运行：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/app_pcie/build
sudo ./app
```

如果通过 SSH 启动 GUI，需要额外处理 `DISPLAY` / X11 权限；在鲁班猫桌面终端里运行最简单。

## 2. 界面整体结构

主窗口标题：

```text
Pango PCIe Test v1.0
```

界面主要分为这些区域：

```text
Endpoint Info              显示 PCIe 设备信息、BAR、MPS/MRRS
Config Operation           PCI 配置空间读写
DMA Auto                   自动 DMA 循环测试
DMA Manual                 手动 DMA 分步测试
PIO Test                   BAR 空间 PIO 读写测试
PCIe DMA Performance Test  性能测试
Tandem Load                Tandem 位流加载，当前 DMA 验证可先不使用
Print Info                 日志输出区域
Clean                      清空日志
```

应用启动后会自动读取一次 PCIe 信息并填充 Endpoint Info 区域。如果这里 Vendor ID、Device ID、BAR 信息不正确，先不要做 DMA。

## 3. Endpoint Info 区域

该区域用于确认 PCIe 枚举和 BAR 信息，常见字段包括：

```text
Vendor ID
Device ID
Link Status
Link Speed
Link Width
Bar0 ~ Bar5
Size0 ~ Size5
MPS
MRRS
```

对当前工程，期望至少看到：

```text
Vendor ID = 0755
Device ID = 0755
Link Status = Up
Bar0 / Bar1 / Bar2 有有效地址和大小
```

当前 Linux 日志显示 FPGA BAR 分配为：

```text
BAR0: 0xf4200000 - 0xf4201fff，大小 8 KB
BAR2: 0xf4202000 - 0xf4203fff，大小 8 KB，64bit
BAR1: 0xf4204000 - 0xf4204fff，大小 4 KB
```

如果 GUI 里 BAR 都是 0，说明驱动读取信息或 PCIe 枚举有问题。

## 4. Config Operation 使用

Config Operation 用于读写 PCI 配置空间。

控件：

```text
Mode Switch        Write / Read
Addr Offset(hex)   配置空间偏移，十六进制输入
Data(hex)          写入数据或读出数据显示
按钮               Write 或 Read
```

安全建议：

```text
优先只做 Read
不要随意 Write PCI_COMMAND、BAR、Capability 等关键寄存器
```

推荐读几个只读或低风险寄存器：

```text
Addr Offset = 0x00  读取 Vendor ID / Device ID
Addr Offset = 0x08  读取 Class Code / Revision
Addr Offset = 0x10  读取 BAR0 配置寄存器
```

使用步骤：

```text
1. Mode Switch 选择 Read
2. Addr Offset(hex) 输入 0
3. 点击 Read
4. 看 Data(hex) 和 Print Info 输出
```

如果读配置空间正常，说明字符设备 ioctl 和 PCI config access 基本工作。

## 5. PIO Test 使用

PIO Test 用于通过 `/dev/mem` 映射 BAR 空间，并对 BAR 寄存器做 PIO 读写。

控件：

```text
Bar Addr Switch      Bar0 ~ Bar5
Read/Write Mode      Write / Read / W & R
Addr Offset(hex)     BAR 内偏移，十六进制
Data(hex)            写入数据或读出数据
Repeat Cnt           重复次数，0 表示可能循环
Operation Delay(ns)  操作间隔
Start Test           开始/停止测试
```

建议先从低风险读测试开始：

```text
1. Bar Addr Switch 选择 Bar0
2. Read/Write Mode 选择 Read
3. Addr Offset(hex) 输入 0
4. Repeat Cnt 输入 1
5. Operation Delay(ns) 保持默认 1000
6. 点击 Start Test
```

如果 FPGA BAR0 有可读状态寄存器，应该能在 Print Info 看到读出的值。

只有确认 FPGA BAR 寄存器定义允许写入时，才使用 Write 或 W & R。不要对未知寄存器随便写。

## 6. DMA 测试前准备

DMA 测试需要选择一个写入数据文件，应用会把文件内容读入用户态缓冲区，然后通过 ioctl 传给内核 DMA 缓冲区。

工程里已有测试文件：

```text
pango_pcie_dma_alloc/app_pcie/test.txt
```

该文件大小约 4412 字节，内容是 ASCII pattern。DMA 单次最大长度是 4096 字节，所以可以直接作为测试输入文件。

DMA 长度限制来自代码：

```text
DMA_MIN_PACKET_SIZE = 4 bytes
DMA_MAX_PACKET_SIZE = 4096 bytes
```

应用会把 DMA 大小、offset、length 向下对齐到 4 字节。因此建议所有长度和 offset 都使用 4 字节对齐值。

## 7. DMA Manual 测试

DMA Manual 是最适合第一次调 DMA 的模式，因为每一步都可以单独观察日志和硬件状态。

### 7.1 控件说明

DMA Manual 区域控件：

```text
Allocate Mem Size(bytes)  分配 DMA 缓冲区大小，默认 128
Offset Addr(hex)          DMA 地址偏移，默认 0
Data Length(bytes)        有效传输长度，默认 4
Start DMA                 选择文件并申请 DMA 缓冲区
Write DDR                 把测试文件数据写入内核 DMA 源缓冲区
DMA Read                  触发 FPGA 从主机 DMA 源缓冲区读取数据
DMA Write                 触发 FPGA 把数据写回主机 DMA 目的缓冲区
Read DDR                  从内核 DMA 目的缓冲区读回并和写入数据比较
Close DMA                 释放 DMA 缓冲区
```

按钮名容易混淆，可以按驱动 ioctl 理解：

```text
Write DDR  -> PCI_WRITE_TO_KERNEL_CMD   用户数据复制到内核 DMA 源缓冲区
DMA Read   -> PCI_DMA_READ_CMD          FPGA/DMA 从主机源缓冲区读走数据
DMA Write  -> PCI_DMA_WRITE_CMD         FPGA/DMA 写数据回主机目的缓冲区
Read DDR   -> PCI_READ_FROM_KERNEL_CMD  用户态读取目的缓冲区并比较
Close DMA  -> PCI_UMAP_ADDR_CMD         释放 DMA 缓冲区
```

### 7.2 推荐第一次测试参数

第一次建议用最小风险参数：

```text
Allocate Mem Size(bytes) = 128
Offset Addr(hex)         = 0
Data Length(bytes)       = 4 或 16
```

跑通后再逐步扩大：

```text
32 -> 64 -> 128 -> 256 -> 512 -> 1024 -> 4096
```

### 7.3 推荐操作顺序

严格按这个顺序操作：

```text
1. 在 DMA Manual 区域填参数
2. 点击 Start DMA
3. 弹出文件选择框，选择 app_pcie/test.txt
4. 点击文件选择框 OK
5. 等 Print Info 出现 Start DMA Manual Test / PCI_MAP_ADDR_CMD
6. 点击 Write DDR
7. 点击 DMA Read
8. 等 0.5 秒以上
9. 点击 DMA Write
10. 等 0.5 秒以上
11. 点击 Read DDR
12. 查看 Print Info 是否出现 Write data is not equal to read data
13. 点击 Close DMA 释放缓冲区
```

基本期望结果：

```text
Read DDR 后打印每个 DW 的 write_data / read_data
没有 Write data is not equal to read data !!!
```

更具体地说，DMA Manual 对了以后，`Print Info` 里应该能看到类似流程：

```text
Start DMA Manual Test (PCI_MAP_ADDR_CMD)
Write DDR (PCI_WRITE_TO_KERNEL_CMD)
DMA  Read (PCI_DMA_READ_CMD)
DMA Write (PCI_DMA_WRITE_CMD)
Read  DDR (PCI_READ_FROM_KERNEL_CMD)
dw_cnt = 1; write_data = 0x....; read_data = 0x....
dw_cnt = 2; write_data = 0x....; read_data = 0x....
```

判断标准：

```text
每一行 write_data 和 read_data 完全一致
不出现 [ ERROR ] Write data is not equal to read data !!!
内核 dmesg 不出现 DMA 分配失败、ioctl 失败、kernel oops 等异常
```

例如 `Data Length = 4` 时，只会比较 1 个 DW：

```text
dw_cnt = 1; write_data = 0x31323334; read_data = 0x31323334
```

只要同一行里的 `write_data` 和 `read_data` 相同，就表示这一笔主机到 FPGA、再从 FPGA 回主机的数据闭环是正确的。

如果出现：

```text
Write data is not equal to read data !!!
```

说明回读数据和写入数据不一致，需要检查 FPGA DMA 回环逻辑、DMA 方向、地址、长度、offset 和缓存区使用。

### 7.4 Manual 测试常见参数错误

应用会做这些检查：

```text
Allocate Mem Size < 4      自动改为 4
Allocate Mem Size > 4096   自动改为 4096
Data Length < 4            自动改为 4
Data Length > 4096         自动改为 4096
Data Length > Allocate Mem Size  报错
Offset Addr > Allocate Mem Size - Data Length  报错
```

所以必须满足：

```text
0 <= Offset Addr <= Allocate Mem Size - Data Length
Data Length <= Allocate Mem Size
所有值最好 4 字节对齐
```

## 8. DMA Auto 测试

DMA Auto 会自动执行完整 DMA 流水线：

```text
PCI_MAP_ADDR_CMD
PCI_WRITE_TO_KERNEL_CMD
PCI_DMA_READ_CMD
等待 500 ms
PCI_DMA_WRITE_CMD
等待 500 ms
PCI_READ_FROM_KERNEL_CMD
PCI_UMAP_ADDR_CMD
数据比较
```

### 8.1 控件说明

DMA Auto 区域控件：

```text
Test Num                  测试次数，0 表示循环测试
Start Packet Size(bytes)  起始包大小，默认 128
End Packet Size(bytes)    结束包大小
Packet Step(bytes)        步进，目前下拉框只有 0
Write Packet Count        写计数显示
Read Packet Count         读计数显示
Error Packet Count        错误计数显示
Start Test                开始/停止测试
```

注意：当前源码里 `temp_end = temp_start;`，所以 Auto 实际只按 `Start Packet Size` 这一种长度测试，`End Packet Size` 和 Step 的作用被弱化了。第一次使用可以把 Start/End 都当成同一个 packet size。

### 8.2 推荐第一次 Auto 参数

```text
Test Num = 1
Start Packet Size(bytes) = 128
End Packet Size(bytes) = 128
Packet Step(bytes) = 0
```

操作步骤：

```text
1. 填 Test Num = 1
2. Start Packet Size 填 128
3. 点击 Start Test
4. 弹出文件选择框，选择 app_pcie/test.txt
5. 点击 OK
6. 等待测试完成
7. 查看 Write Packet Count / Read Packet Count / Error Packet Count
```

期望：

```text
Write Packet Count = 1
Read Packet Count = 1
Error Packet Count = 0
```

DMA Auto 正确时，重点看界面计数：

```text
Write Packet Count 和 Read Packet Count 相同
Error Packet Count 始终为 0
Print Info 不出现 Write data is not equal to read data !!!
```

如果 `Test Num = 100`，正确结果应类似：

```text
Write Packet Count = 100
Read Packet Count = 100
Error Packet Count = 0
```

跑通后再把 Test Num 改为更大值，例如：

```text
10
100
1000
```

不要一开始就填 0 做无限循环。无限循环需要手动再次点击 Start Test 停止。

## 9. Performance 测试

Performance 区域用于持续测 DMA 吞吐率，依赖 FPGA 端性能测试寄存器设计。应用会映射 BAR0，并访问这些 offset：

```text
0x00  Performance 状态寄存器
0x04  DMA 写计数
0x08  DMA 读计数
0x0c  错误计数
0x10  数据包计数
```

控件：

```text
DMA Read/Write Mode        Write / Read / W & R
Test Packet Size(bytes)    测试包大小，默认 128
Write Throughput(MB/s)     写吞吐显示
Read Throughput(MB/s)      读吞吐显示
Write Bandwidth Utilization(%)
Read Bandwidth Utilization(%)
Start Test                 开始/停止性能测试
```

推荐步骤：

```text
1. 确认 DMA Manual 或 DMA Auto 已经能正确回环
2. DMA Read/Write Mode 先选 Write 或 Read，不要一开始选 W & R
3. Test Packet Size 填 128
4. 点击 Start Test
5. 观察吞吐、带宽利用率、Print Info 错误计数
6. 再次点击 Start Test 停止
```

性能测试会每秒更新一次。若 Print Info 出现数据不一致或错误计数增加，先回到 DMA Manual 小长度测试定位。

Performance 正确时，重点看三类结果：

```text
吞吐率/带宽利用率不是一直为 0
DMA write of error cnt : 0
DMA read of error cnt  : 0
```

`Print Info` 里可能看到类似：

```text
DMA write total number of performance tests : 10
DMA read total number of performance tests  : 10
DMA write of error cnt : 0
DMA read of error cnt  : 0
```

如果出现下面任意一种，都不算 DMA 完全正确：

```text
DMA Write data is not equal to DMA read data !!!
DMA Read data is not equal to DDR Memory data
DMA write of error cnt 持续增加
DMA read of error cnt 持续增加
吞吐率一直为 0
```

Performance 是压力/吞吐测试，不适合作为第一步验证。必须先让 DMA Manual 小长度数据一致，再看 Performance。

## 10. 推荐完整验证路线

第一次使用 GUI 时，推荐按下面路线走：

```text
1. 启动 GUI，确认 Endpoint Info 正常
2. Config Operation 读 0x00，确认配置空间访问正常
3. PIO Test 对 Bar0 做一次只读测试
4. DMA Manual：4 bytes，手动完整流程
5. DMA Manual：16 / 128 / 512 / 4096 bytes
6. DMA Auto：Test Num = 1，Packet Size = 128
7. DMA Auto：Test Num = 100，Packet Size = 128 或 1024
8. Performance：Write 或 Read，Packet Size = 128
9. Performance：扩大到 1024 / 4096
```

只要某一步失败，就停在当前层排查，不要继续扩大测试规模。

## 11. 常见问题

### 11.1 启动 GUI 提示 PCIe Device Open Fail

说明应用打开 `/dev/pango_pci_driver` 失败。检查：

```sh
lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver
dmesg | tail -n 120
```

### 11.2 DMA 点击 Start DMA 后没有继续

`Start DMA` 会弹出文件选择框，必须选择一个输入文件并点击 OK。建议选择：

```text
/home/cat/cat_pcie_project/pango_pcie_dma_alloc/app_pcie/test.txt
```

如果取消文件选择，DMA 测试会停止。

### 11.3 Data Length greater than Allocate Mem Size

说明传输长度大于申请的 DMA 缓冲区。把 `Allocate Mem Size` 调大，或者把 `Data Length` 调小。

### 11.4 Offset Addr greater than Allocate Mem Size subtract Data Length

说明 offset 太大。必须满足：

```text
Offset Addr + Data Length <= Allocate Mem Size
```

### 11.5 Write data is not equal to read data

说明 DMA 回读数据和写入数据不同。优先检查：

```text
FPGA DMA 方向是否和按钮流程一致
FPGA 是否真的把 DMA Read 收到的数据回写到 DMA Write 通道
current_len 单位是否按 DW/4 字节理解
offset 是否 4 字节对齐
BAR0 控制寄存器 offset 是否和驱动一致
```

### 11.6 Performance 有吞吐但错误计数增加

先不要调性能，回到 DMA Manual：

```text
Data Length = 4 / 16 / 128
Offset = 0
```

先保证小包数据完全一致，再继续性能测试。

## 12. 关键源码对应关系

GUI 按钮和 ioctl 对应关系：

```text
Start DMA    -> PCI_MAP_ADDR_CMD
Write DDR    -> PCI_WRITE_TO_KERNEL_CMD
DMA Read     -> PCI_DMA_READ_CMD
DMA Write    -> PCI_DMA_WRITE_CMD
Read DDR     -> PCI_READ_FROM_KERNEL_CMD
Close DMA    -> PCI_UMAP_ADDR_CMD
```

DMA 最大长度：

```text
DMA_MAX_PACKET_SIZE = 4096
DMA_MIN_PACKET_SIZE = 4
```

驱动控制寄存器 offset：

```text
CMD_REG_OFFSET     = 0x100
RW_ADDR_LO_OFFSET  = 0x110
RW_ADDR_HI_OFFSET  = 0x120
```

性能测试寄存器 offset：

```text
PEFORMANCE_STATUS_OFFSET     = 0x00
PEFORMANCE_WRITE_CNT_OFFSET  = 0x04
PEFORMANCE_READ_CNT_OFFSET   = 0x08
PEFORMANCE_ERROR_CNT_OFFSET  = 0x0c
PEFORMANCE_DATA_CNT_OFFSET   = 0x10
```

这些 offset 必须和 FPGA 端寄存器设计一致，否则 GUI 操作会正常发 ioctl，但硬件不会按预期响应。
