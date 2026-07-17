# PCIe Link Up 后 Linux 上位机操作流程

本文用于记录 FPGA PCIe 链路已经建立后，鲁班猫 Linux 侧应该如何继续操作。当前已经从“链路训练失败”推进到“PCIe Endpoint 已经被 Linux 枚举”，后续重点是确认设备、加载驱动、绑定驱动、运行应用和做 DMA/性能测试。

## 1. 当前状态确认

当前 Linux 已经可以枚举到 FPGA PCIe Endpoint：

```text
lspci -nn
00:00.0 PCI bridge [0604]: Fuzhou Rockchip Electronics Co., Ltd Device [1d87:3566] (rev 01)
01:00.0 Memory controller [0580]: Device [0755:0755]
```

关键 `dmesg` 日志：

```text
rk-pcie 3c0000000.pcie: PCIe Link up, LTSSM is 0x130011
rk-pcie 3c0000000.pcie: PCI host bridge to bus 0000:00
pci 0000:01:00.0: [0755:0755] type 00 class 0x058000
pci 0000:01:00.0: reg 0x10: [mem 0x00000000-0x00001fff]
pci 0000:01:00.0: reg 0x14: [mem 0x00000000-0x00000fff]
pci 0000:01:00.0: reg 0x18: [mem 0x00000000-0x00001fff 64bit]
pci 0000:01:00.0: BAR 0: assigned [mem 0xf4200000-0xf4201fff]
pci 0000:01:00.0: BAR 2: assigned [mem 0xf4202000-0xf4203fff 64bit]
pci 0000:01:00.0: BAR 1: assigned [mem 0xf4204000-0xf4204fff]
```

这说明：

- miniPCIe 对应的 `3c0000000.pcie` 已经 link up。
- FPGA Endpoint BDF 是 `0000:01:00.0`。
- Vendor ID / Device ID 是 `0755:0755`。
- BAR0、BAR1、BAR2 已经被 Linux 分配地址。
- 另一路 `3c0800000.pcie` 仍然 `PCIe Link Fail`，如果没有接设备，这个可以先忽略。

## 2. Linux 侧操作总流程

链路已经起来后，推荐操作顺序是：

```text
1. 确认 lspci 能看到 0755:0755
2. 确认驱动 id_config.h 和 run.sh 中的 Vendor/Device ID 一致
3. 编译 pango_pci_driver.ko
4. 加载驱动
5. 确认 pci_driver_probe() 被调用
6. 确认 /dev/pango_pci_driver 已创建
7. 编译并运行 app_pcie 应用
8. 做配置空间读取、BAR 访问、DMA 测试和性能测试
```

## 3. 枚举确认命令

先不要急着加载驱动，先确认 PCIe 设备存在：

```sh
lspci -nn
lspci -nn -s 01:00.0
lspci -vv -s 01:00.0
```

也可以从 sysfs 直接确认：

```sh
cat /sys/bus/pci/devices/0000:01:00.0/vendor
cat /sys/bus/pci/devices/0000:01:00.0/device
cat /sys/bus/pci/devices/0000:01:00.0/class
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

期望看到：

```text
vendor = 0x0755
device = 0x0755
```

如果这一步失败，说明又回到了枚举/链路问题，不要继续调驱动。

## 4. 确认驱动 ID 匹配

驱动匹配 ID 在：

```text
pango_pcie_dma_alloc/driver/id_config.h
```

当前配置：

```c
#define PCI_PANGO_DEFAULT_VENDOR_ID  0x0755
#define PCI_PANGO_DEFAULT_DEVICE_ID  0x0755
```

`run.sh` 里也有同样的检查：

```sh
pci_vendor="0x0755"
pci_device="0x0755"
```

如果 FPGA bitstream 后续改了 Vendor ID / Device ID，需要同步修改这两个位置。否则可能出现 `lspci` 能看到设备，但驱动不 probe 或 `run.sh` 误判设备未枚举。

## 5. 编译驱动

进入驱动目录：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/driver
```

如果之前加载过驱动，先卸载：

```sh
sudo rmmod pango_pci_driver 2>/dev/null || true
```

重新编译：

```sh
sudo make clean
sudo make
```

成功时应看到：

```text
LD [M]  .../pango_pci_driver.ko
```

确认文件存在：

```sh
ls -l pango_pci_driver.ko
```

## 6. 加载驱动并确认 probe

加载驱动：

```sh
sudo insmod pango_pci_driver.ko
```

确认模块已加载：

```sh
lsmod | grep pango_pci_driver
```

查看内核日志：

```sh
dmesg | tail -n 120
```

期望看到驱动初始化日志和 PCI probe 日志：

```text
init_pci_pango.
init_pango_pci_driver.
pci_register_driver, result : 0
pci_driver_probe.
dev vendor : 0x755, device : 0x755
```

如果只看到：

```text
pci_register_driver, result : 0
```

但没有：

```text
pci_driver_probe.
```

说明驱动没有绑定到 PCIe 设备。优先检查：

```sh
lspci -nn
cat /sys/bus/pci/devices/0000:01:00.0/vendor
cat /sys/bus/pci/devices/0000:01:00.0/device
ls /sys/bus/pci/drivers/
```

## 7. 确认设备节点

驱动加载成功后，应有字符设备节点：

```sh
ls -l /dev/pango_pci_driver
ls -l /sys/class/pango_pci_driver/
```

期望类似：

```text
crw------- 1 root root ... /dev/pango_pci_driver
```

应用默认打开的路径在：

```text
pango_pcie_dma_alloc/app_pcie/includes/config_gui.h
```

对应：

```c
#define PCIE_DRIVER_FILE_PATH "/dev/pango_pci_driver"
```

## 8. 运行完整脚本

链路已枚举、驱动可编译后，可以直接运行工程脚本：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc
sudo ./run.sh
```

`run.sh` 会做这些事：

```text
1. 检查是否 root
2. 如果驱动未加载，编译并 insmod 驱动
3. 检查 /sys/bus/pci/devices 中是否存在 0x0755:0x0755
4. 编译 app_pcie
5. 启动 app_pcie/build/app
```

如果你已经手动加载过驱动，脚本会显示：

```text
PCIe驱动已装载
```

然后继续检查 PCIe Endpoint 和启动应用。

## 9. 推荐的手动调试流程

如果不想一上来跑完整 GUI，可以按下面流程手动分段验证：

```sh
# 1. 确认 PCIe Endpoint
lspci -nn
lspci -vv -s 01:00.0

# 2. 编译/加载驱动
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/driver
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo make clean
sudo make
sudo insmod pango_pci_driver.ko

# 3. 确认 probe 和设备节点
dmesg | tail -n 120
lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver

# 4. 编译应用
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/app_pcie
make clean
make

# 5. 运行应用
cd build
sudo ./app
```

## 10. 应用层测试建议

应用启动后，建议按风险从低到高测试：

```text
1. 先看 PCIe 信息/配置空间读取是否正常
2. 再做 BAR / PIO 读写测试
3. 再做小长度 DMA 测试
4. 最后做大包 DMA 和 performance 测试
```

不要一上来就跑最大长度 DMA。先用小长度、固定 pattern 验证数据一致性，更容易定位问题。

## 11. 测试结束后的收尾和驱动卸载

DMA/PIO/Performance 测试结束后，建议按下面顺序收尾：

```text
1. 先停止 GUI 里的循环测试
2. 如果正在 DMA Manual，先点击 Close DMA
3. 关闭 Pango PCIe Test v1.0 应用
4. 确认没有 app 进程占用 /dev/pango_pci_driver
5. 卸载 pango_pci_driver 驱动
6. 查看 dmesg，确认没有异常日志
```

### 11.1 GUI 内部先停止测试

如果正在跑 `DMA Auto` 或 `Performance`，不要直接关窗口，先再次点击对应的 `Start Test` 按钮让它停止。

如果正在做 `DMA Manual`，最后必须点击：

```text
Close DMA
```

这个按钮会触发：

```text
PCI_UMAP_ADDR_CMD
```

作用是释放驱动里 `pci_alloc_consistent()` 申请的 DMA 缓冲区。如果不点 `Close DMA` 就直接退出 GUI，进程会结束，但从调试习惯上不推荐这样做，因为不利于确认 DMA 资源释放路径是否正常。

### 11.2 关闭应用并确认没有进程占用

关闭 GUI 后，可以确认应用进程是否还在：

```sh
ps -ef | grep app_pcie
ps -ef | grep "./app"
```

也可以检查是否还有进程打开驱动节点：

```sh
sudo fuser -v /dev/pango_pci_driver
```

如果 `fuser` 没有输出，说明当前没有进程占用该字符设备。

如果还有残留进程，先正常关闭对应终端或窗口；确认是卡死进程时再结束：

```sh
sudo kill <pid>
```

### 11.3 卸载驱动

确认 GUI 已退出、没有进程占用后，卸载驱动：

```sh
sudo rmmod pango_pci_driver
```

确认模块已经卸载：

```sh
lsmod | grep pango_pci_driver
```

如果没有输出，说明驱动已经卸载。

同时 `/dev/pango_pci_driver` 正常也会消失：

```sh
ls -l /dev/pango_pci_driver
```

如果提示 `No such file or directory`，这是正常的。

### 11.4 rmmod 提示 busy 怎么办

如果卸载时报：

```text
rmmod: ERROR: Module pango_pci_driver is in use
```

说明还有进程打开着驱动节点。按下面查：

```sh
sudo fuser -v /dev/pango_pci_driver
ps -ef | grep app
```

先关闭 GUI 或结束占用进程，然后再执行：

```sh
sudo rmmod pango_pci_driver
```

### 11.5 查看收尾日志

卸载后看一下最后的内核日志：

```sh
dmesg | tail -n 80
```

重点确认没有这些异常：

```text
kernel oops
BUG:
Unable to handle kernel
DMA allocation/free error
use-after-free
```

如果只是看到驱动 remove、字符设备删除、模块退出之类日志，属于正常收尾。

### 11.6 下次重新测试怎么做

如果 FPGA 仍然保持上电、PCIe 仍然枚举正常，下次测试可以直接重新加载驱动并启动应用：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc
sudo ./run.sh
```

或者手动：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/driver
sudo insmod pango_pci_driver.ko
lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver

cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/app_pcie/build
sudo ./app
```

如果重新加载驱动后 probe 不出现，先确认设备仍在：

```sh
lspci -nn
cat /sys/bus/pci/devices/0000:01:00.0/vendor
cat /sys/bus/pci/devices/0000:01:00.0/device
```

如果 `lspci` 已经看不到 FPGA，就不是单纯驱动卸载问题，需要回到 PCIe 链路/枚举流程排查，通常重启鲁班猫或重新让 Root Complex 扫描链路。

## 12. 常见问题处理

### 12.1 lspci 有设备，但驱动没有 probe

检查 ID 是否一致：

```sh
lspci -nn -s 01:00.0
cat /sys/bus/pci/devices/0000:01:00.0/vendor
cat /sys/bus/pci/devices/0000:01:00.0/device
```

确认 `id_config.h`：

```c
#define PCI_PANGO_DEFAULT_VENDOR_ID  0x0755
#define PCI_PANGO_DEFAULT_DEVICE_ID  0x0755
```

重新编译并加载：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/driver
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo make clean
sudo make
sudo insmod pango_pci_driver.ko
```

### 12.2 /dev/pango_pci_driver 不存在

先确认模块是否加载：

```sh
lsmod | grep pango_pci_driver
```

再看日志：

```sh
dmesg | tail -n 120
```

如果 `class_create` 或 `device_create` 失败，字符设备节点不会出现。当前源码已经修复过 class 指针保存问题，但仍要看实际日志确认。

### 12.3 insmod 报 File exists

说明模块已经加载，先卸载：

```sh
sudo rmmod pango_pci_driver
```

再加载：

```sh
sudo insmod pango_pci_driver.ko
```

### 12.4 run.sh 仍提示 PCIe 设备未枚举

先手动确认：

```sh
lspci -nn
cat /sys/bus/pci/devices/0000:01:00.0/vendor
cat /sys/bus/pci/devices/0000:01:00.0/device
```

如果实际 ID 不是 `0x0755:0x0755`，修改 `run.sh` 的：

```sh
pci_vendor="0x0755"
pci_device="0x0755"
```

### 12.5 另一路 PCIe Link Fail 是否要处理

当前日志中：

```text
3c0000000.pcie: PCIe Link up
3c0800000.pcie: PCIe Link Fail
```

如果 FPGA 接在 miniPCIe，且 `lspci` 已经出现 `01:00.0 [0755:0755]`，那么 `3c0800000.pcie` 的失败可以先忽略。那通常是另一组 PCIe 控制器没有接设备。

## 13. 成功判据

Linux 上位机侧正确状态应满足：

```text
1. lspci -nn 能看到 01:00.0 [0755:0755]
2. dmesg 有 PCIe Link up
3. dmesg 有 pango 驱动 pci_driver_probe 日志
4. /dev/pango_pci_driver 存在
5. app_pcie 能打开 /dev/pango_pci_driver
6. 配置空间读取、BAR 访问、小长度 DMA 测试正常
```

达到这些以后，才进入大数据 DMA、性能测试和稳定性验证阶段。
