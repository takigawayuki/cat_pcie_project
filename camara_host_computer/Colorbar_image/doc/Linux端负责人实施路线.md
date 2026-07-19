# Linux 端彩条接收实施时间线

本文是 `camara_host_computer/Colorbar_image` 的唯一主文档。后续按这个时间线推进，不再把“实施路线”和“开发进度”分成两份文档。

当前目标：在鲁班猫2 RK3568 Linux 端，通过 PCIe 接收 FPGA 工程 `PCIE_DMA_test_color_MES50HP_X1` 发来的 1920x1080 RGB565 彩条图像。

重要边界：`pango_pcie_dma_alloc` 已经验证 PCIe 通信和基础 DMA 可用，当前不修改它；新的彩条接收工作只放在 `camara_host_computer/Colorbar_image`。

## 时间线 0：先明确现在到底在测什么

现在这套测试测的是：

```text
FPGA 能不能通过 PCIe 把一帧 RGB565 彩条数据写进 RK3568 Linux 内存，Linux 能不能把这帧数据保存并校验出来。
```

数据路径是：

```text
FPGA 彩条生成
  -> FPGA PCIe Endpoint 发起 MWR 写请求
  -> RK3568 PCIe Root Complex 接收
  -> colorbar_pcie_driver 分配的 DMA coherent buffer
  -> 用户态 pcie_color_rx 通过 mmap 读取 buffer
  -> 保存 frame_0000.rgb565
  -> 抽样校验 RGB565 彩条颜色
```

当前不是在测试：

```text
不是测试 HDMI 显示
不是测试 LCD 显示
不是测试 GUI
不是测试 V4L2 摄像头框架
不是测试 DRM/fb0 实时显示
不是测试长时间连续视频流
不是测试 FPGA frame_done 中断，当前驱动先用 frame_wait_ms 固定延时临时代替
```

也就是说，现阶段先确认“Linux 收到正确的一帧 raw 图像”，再进入“实时显示”。

## 时间线 1：已经确认的 FPGA 侧协议

从 FPGA 工程：

```text
PCIE_DMA_test_color_MES50HP_X1/source/pcie_dma_ctrl.v
```

已经确认以下信息。

### 1.1 Linux 要给 FPGA 写 4 个 DMA 地址

FPGA 监听 BAR 偏移：

```text
0x110
```

Linux 需要连续 4 次向 `BAR + 0x110` 写入 32-bit DMA 地址：

```text
第 1 次写 0x110 -> dma_addr0
第 2 次写 0x110 -> dma_addr1
第 3 次写 0x110 -> dma_addr2
第 4 次写 0x110 -> dma_addr3
```

FPGA 收到地址后，会按帧循环写入这 4 个 buffer。

### 1.2 DMA 地址必须优先按 32-bit 处理

FPGA 侧 DMA 地址寄存器是 32 位：

```text
dma_addr0..dma_addr3 都是 32-bit
```

所以 Linux 新驱动里使用：

```c
dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
```

目的：避免内核分配出 FPGA 无法完整表示的 64-bit DMA 地址。

### 1.3 图像格式和大小

```text
分辨率       1920 x 1080
格式         RGB565
单像素       2 bytes
有效帧大小   4147200 bytes
FPGA 标记包   64 bytes
Linux buffer  4149248 bytes，按页对齐后可容纳有效帧和标记包
buffer 数量   4
```

## 时间线 2：为什么要新建 Colorbar_image 工程

`pango_pcie_dma_alloc` 的意义是验证基础链路：

```text
PCIe link up
Linux 能枚举 FPGA
驱动能 probe
BAR 能访问
基础 DMA 读写能跑通
```

但它不是专门的视频接收工程，旧测试程序偏向通用 DMA/GUI 测试，不适合直接作为 1080p 彩条收帧程序。

所以新建：

```text
camara_host_computer/Colorbar_image
```

它只做 Linux 彩条接收这件事。

## 时间线 3：当前已经新增的内容

当前工程文件：

```text
camara_host_computer/Colorbar_image/Makefile
camara_host_computer/Colorbar_image/.gitignore
camara_host_computer/Colorbar_image/include/colorbar_pcie_rx.h
camara_host_computer/Colorbar_image/src/pcie_color_rx.c
camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.c
camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.h
camara_host_computer/Colorbar_image/driver/Makefile
camara_host_computer/Colorbar_image/doc/Linux端负责人实施路线.md
```

编译后生成：

```text
build/pcie_color_rx              用户态彩条接收/校验工具
driver/colorbar_pcie_driver.ko   独立 PCIe 彩条接收驱动
```

新驱动设备节点：

```text
/dev/colorbar_pcie_rx
```

当前新驱动做的事情：

```text
匹配 PCI ID 0755:0755
默认映射 BAR1，可用 bar=0/1 参数切换
强制 32-bit DMA mask
分配 4 个 DMA coherent buffer
每个 buffer 大小 4149248 bytes
mmap 给用户态直接读取
START 时连续 4 次写 BAR+0x110，把 DMA 地址告诉 FPGA
STOP 时写 BAR+0x130
WAIT_FRAME 当前先用 frame_wait_ms 固定延时模拟等待一帧
```

当前用户态工具做的事情：

```text
--info                         显示当前图像和 buffer 参数
--once --output frame.rgb565   接收并保存一帧 raw 图像
--validate frame.rgb565        抽样校验 RGB565 彩条颜色
```

## 时间线 4：第一次编译

进入工程目录：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
```

完整编译：

```sh
make
```

预期生成：

```text
build/pcie_color_rx
driver/colorbar_pcie_driver.ko
```

只编译用户态：

```sh
make app
```

只编译驱动：

```sh
make driver
```

清理：

```sh
make clean
```

## 时间线 5：不上板也能先检查用户态参数

运行：

```sh
./build/pcie_color_rx --info
```

预期输出类似：

```text
Colorbar RX constants:
  resolution      : 1920x1080
  format          : RGB565
  frame_size      : 4147200 bytes
  mark_size       : 64 bytes
  buffer_count    : 4
  buffer_size     : 4149248 bytes
  default device  : /dev/colorbar_pcie_rx
```

这一步只说明用户态程序能运行、图像参数是当前预期值，不说明 PCIe 或 DMA 已经成功。

## 时间线 6：上板前确认 PCIe 设备存在

前提：鲁班猫2 已启动，FPGA 已上电，PCIe link up，并且系统能枚举到 FPGA。

运行：

```sh
lspci -nn
lspci -nn -s 01:00.0
```

预期看到类似：

```text
01:00.0 ... Device [0755:0755]
```

如果看不到 `0755:0755`，不要继续加载彩条驱动，先回到 PCIe 上电、PERST#、链路、枚举问题排查。

## 时间线 7：卸载旧 pango 测试驱动

一个 PCIe 设备同一时间只能绑定一个 PCI 驱动。测试新驱动前，先卸载旧驱动：

```sh
sudo rmmod pango_pci_driver 2>/dev/null || true
```

确认旧驱动不在：

```sh
lsmod | grep pango_pci_driver
```

预期：没有输出。

如果卸载失败，通常是旧 GUI 或旧测试程序还占着 `/dev/pango_pci_driver`，先关掉旧程序。

## 时间线 8：加载新的彩条接收驱动

默认先试 BAR1，地址字节反转打开：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
```

参数含义：

```text
bar=1              先按 BAR1 映射 FPGA 控制寄存器，不对再试 bar=0
addr_byteswap=1    按当前 FPGA 0x110 地址解析逻辑做 32-bit 字节反转
frame_wait_ms=100  临时等待一帧 100ms，后续应换成 FPGA 状态寄存器或中断
```

确认模块：

```sh
lsmod | grep colorbar_pcie_driver
```

确认设备节点：

```sh
ls -l /dev/colorbar_pcie_rx
```

查看内核日志：

```sh
dmesg | tail -n 120
```

预期看到类似：

```text
colorbar PCIe RX probe ok, BAR1 ...
colorbar PCIe RX driver loaded
```

如果没有 probe 日志，先检查：

```sh
lspci -nn
lsmod | grep pango_pci_driver
lsmod | grep colorbar_pcie_driver
```

## 时间线 9：接收并保存一帧 raw 图像

驱动加载成功，并且 `/dev/colorbar_pcie_rx` 存在后运行：

```sh
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
```

这一步内部会做：

```text
打开 /dev/colorbar_pcie_rx
ioctl 分配 4 个 DMA buffer
mmap 这 4 个 buffer
ioctl START，驱动把 4 个 DMA 地址写到 FPGA BAR+0x110
等待 frame_wait_ms
从当前 buffer 保存 4147200 bytes 到 frame_0000.rgb565
ioctl STOP
释放 buffer
```

预期输出类似：

```text
captured frame_counter=... buffer=... to frame_0000.rgb565
```

检查文件大小：

```sh
ls -l frame_0000.rgb565
```

预期大小：

```text
4147200 bytes
```

如果文件全 0 或内容完全不变，通常表示 FPGA 没有写到 Linux buffer，优先排查 BAR 和 `addr_byteswap`。

## 时间线 10：校验彩条数据

运行：

```sh
./build/pcie_color_rx --validate frame_0000.rgb565
```

预期每个采样点都是 `OK`：

```text
sample white    at ( 120,100): actual=0xffff expected=0xffff OK
sample yellow   at ( 360,100): actual=0xffe0 expected=0xffe0 OK
sample cyan     at ( 600,100): actual=0x07ff expected=0x07ff OK
sample green    at ( 840,100): actual=0x07e0 expected=0x07e0 OK
sample magenta  at (1080,100): actual=0xf81f expected=0xf81f OK
sample red      at (1320,100): actual=0xf800 expected=0xf800 OK
sample blue     at (1560,100): actual=0x001f expected=0x001f OK
sample black    at (1800,100): actual=0x0000 expected=0x0000 OK
validation passed
```

如果全部 OK，说明当前阶段成功：

```text
Linux 已经接收到 FPGA 通过 PCIe 写来的 RGB565 彩条帧。
```

## 时间线 11：如果失败，按这个顺序排查

### 11.1 lspci 看不到 0755:0755

说明问题还在 PCIe 链路、PERST#、上电时序、FPGA bitstream 或枚举阶段。此时不要继续测新驱动。

### 11.2 insmod 后没有 probe

检查旧驱动是否占用：

```sh
lsmod | grep pango_pci_driver
```

检查设备是否还在：

```sh
lspci -nn
```

检查日志：

```sh
dmesg | tail -n 120
```

### 11.3 BAR1 不行就试 BAR0

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=0 addr_byteswap=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output frame_bar0.rgb565
./build/pcie_color_rx --validate frame_bar0.rgb565
```

### 11.4 数据全 0 或完全不变就试地址字节序

先试关闭字节反转：

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=0 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output frame_noswap.rgb565
./build/pcie_color_rx --validate frame_noswap.rgb565
```

也可以组合试：

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=0 addr_byteswap=0 frame_wait_ms=100
```

### 11.5 数据像半帧或偶发错就加等待时间

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=200
sudo ./build/pcie_color_rx --once --output frame_wait200.rgb565
./build/pcie_color_rx --validate frame_wait200.rgb565
```

可尝试：

```text
frame_wait_ms=50
frame_wait_ms=100
frame_wait_ms=200
frame_wait_ms=500
```

注意：固定等待只是临时方案，最终应该让 FPGA 提供 frame_done 状态寄存器或中断。

## 时间线 12：测试结束后卸载驱动

测试完后卸载新驱动：

```sh
sudo rmmod colorbar_pcie_driver
```

确认卸载：

```sh
lsmod | grep colorbar_pcie_driver
ls -l /dev/colorbar_pcie_rx
```

预期：模块没有输出，设备节点不存在。

如果卸载失败，查看是否有程序占用：

```sh
sudo fuser -v /dev/colorbar_pcie_rx
```

## 时间线 13：当前最推荐的一整套命令

如果 FPGA 彩条 bitstream 已经运行、PCIe 已经 link up，直接按这个顺序跑：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make
lspci -nn
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
ls -l /dev/colorbar_pcie_rx
dmesg | tail -n 120
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
ls -l frame_0000.rgb565
./build/pcie_color_rx --validate frame_0000.rgb565
sudo rmmod colorbar_pcie_driver
```

## 时间线 14：当前完成状态和下一步

已经完成：

```text
[x] 建立独立 Colorbar_image Linux 接收工程
[x] 建立独立 PCIe 彩条接收驱动 colorbar_pcie_driver
[x] 建立 /dev/colorbar_pcie_rx 字符设备
[x] 32-bit DMA mask
[x] 4 个 DMA coherent buffer
[x] mmap 给用户态
[x] START 连续 4 次写 BAR+0x110
[x] STOP 写 BAR+0x130
[x] 用户态 --info
[x] 用户态 --once 保存一帧 raw
[x] 用户态 --validate 抽样校验 RGB565 彩条
[x] make 一键编译用户态和驱动
```

待上板验证：

```text
[ ] colorbar_pcie_driver.ko 能否 probe 到 0755:0755
[ ] 实际控制 BAR 是 bar=1 还是 bar=0
[ ] addr_byteswap 应该是 1 还是 0
[ ] --once 能否保存有效 frame_0000.rgb565
[ ] --validate 是否全部 OK
```

后续优化：

```text
[ ] 和 FPGA 端约定 frame_done 状态寄存器或中断
[ ] 去掉固定 frame_wait_ms 等待
[ ] 做连续采集
[ ] 做 RGB565 到显示输出，接入 fb0/DRM 或保存转换成 PNG 预览
[ ] 必要时再考虑 V4L2 设备化
```
