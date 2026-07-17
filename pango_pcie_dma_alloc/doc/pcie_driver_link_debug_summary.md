# Pango PCIe DMA 调试总结

本文汇总本次对 `pango_pcie_dma_alloc` 工程的两轮排查结果，覆盖驱动编译失败、内核 headers 工具架构错误、驱动加载状态、PCIe Endpoint 未枚举，以及 FPGA 端 `PERST#` 低有效复位信号相关分析。

## 1. 当前结论

当前问题分为两层：

1. 软件层：驱动源码和构建环境原本存在问题，导致 `pango_pci_driver.ko` 无法生成。
2. 硬件/链路层：驱动修复并成功加载后，Linux 仍没有枚举到 FPGA PCIe Endpoint，`lspci` 为空，`dmesg` 显示 Rockchip PCIe host `PCIe Link Fail`。

软件层已经修到可以生成并加载 `pango_pci_driver.ko`。当前真正阻塞点是 PCIe 链路未建立，也就是鲁班猫 2 没有在 `/sys/bus/pci/devices` 中发现 FPGA 设备。

## 2. 原始编译错误

运行 `./run.sh` 时，驱动编译失败，日志核心如下：

```text
warning: ignoring return value of 'copy_to_user', declared with attribute warn_unused_result
error, forbidden warning:pango_pci_driver.c:294

warning: ignoring return value of 'copy_from_user', declared with attribute warn_unused_result
error, forbidden warning:pango_pci_driver.c:360
```

失败后继续执行：

```text
insmod: ERROR: could not load module pango_pci_driver.ko: No such file or directory
```

根因是驱动中多处 `copy_to_user()` / `copy_from_user()` 没有检查返回值。当前内核构建环境把 warning 当成 error 处理，所以 `.ko` 没有生成。后面的 `insmod` 失败只是连锁结果，不是第一根因。

## 3. 驱动源码修改

修改文件：

- `pango_pcie_dma_alloc/driver/pango_pci_driver.c`
- `pango_pcie_dma_alloc/driver/pango_pci_driver.h`

主要修复：

1. `pango_cdev_read()` 检查用户缓冲区长度和 `copy_to_user()` 返回值。
2. `pango_cdev_ioctl()` 中所有 `copy_from_user()` / `copy_to_user()` 都检查返回值。
3. 用户态拷贝失败时返回 `-EFAULT`。
4. PCIe 设备尚未绑定时返回 `-ENODEV`，避免 `op_dev` 为空还继续访问 PCI 配置空间或 DMA。
5. 初始化 `dma_info.addr_r.lock`、`dma_info.addr_w.lock`、`performance_config.addr.lock`。
6. 修复 `class_create()` 后 class 指针没有保存到全局 `pci_info._cdev_class` 的问题。
7. 增加 `linux/err.h`，用于 `IS_ERR()` / `PTR_ERR()`。

注意：原始代码里有一些用户态拷贝、DMA 分配、spinlock 混用的问题。当前修改已经避免了本次编译失败和明显的空指针风险，但后续如果要做稳定性增强，建议继续审查 DMA 缓冲区生命周期、长度边界、锁粒度和错误路径。

## 4. Makefile 和 run.sh 修改

修改文件：

- `pango_pcie_dma_alloc/driver/Makefile`
- `pango_pcie_dma_alloc/run.sh`

### Makefile

`clean` 规则增加清理 `.*.d`：

```make
rm -rf *.o *~ core .depend .*.cmd .*.d *.ko *.mod *.mod.c .tmp_versions Module* modules*
```

原因：内核模块编译会生成隐藏依赖文件，例如 `.pango_pci_driver.o.d`。之前该文件由 root 编译残留，普通用户编译时可能出现权限问题。

### run.sh

脚本增加：

1. 驱动 `make clean` / `make` 失败立即退出。
2. 应用 `make clean` / `make` 失败立即退出。
3. 增加 PCIe Endpoint 枚举检查，默认查找：

```sh
pci_vendor="0x0755"
pci_device="0x0755"
```

如果 `/sys/bus/pci/devices` 中找不到 `0x0755:0x0755`，脚本会停止并提示：

```text
PCIe设备未枚举
未在 /sys/bus/pci/devices 中找到 0x0755:0x0755
请确认FPGA已加载PCIe Endpoint位流、供电/REFCLK/PERST#正常，然后重启或重新训练PCIe链路
```

这样可以避免驱动已加载但硬件没枚举时继续启动应用，减少误判。

## 5. 内核 headers 工具架构错误

修复驱动源码后，继续编译遇到：

```text
/bin/sh: 1: scripts/basic/fixdep: Exec format error
```

检查结果：

```text
uname -m
aarch64

file /usr/src/linux-headers-4.19.232/scripts/basic/fixdep
ELF 64-bit LSB shared object, x86-64
```

鲁班猫 2 是 `aarch64`，但 `/usr/src/linux-headers-4.19.232/scripts/` 里的部分 host tools 是 `x86-64`，所以无法在板子上执行。

已重建的工具包括：

- `scripts/basic/fixdep`
- `scripts/recordmcount`
- `scripts/mod/modpost`
- `scripts/dtc/dtc`
- `scripts/sortextable`
- `scripts/kallsyms`
- 以及其他内核构建 host tools

重建后驱动成功生成：

```text
LD [M]  /home/cat/cat_pcie_project/pango_pcie_dma_alloc/driver/pango_pci_driver.ko
```

## 6. 驱动加载结果

驱动可以成功加载：

```text
pango_pci_driver       49152  0
```

`dmesg` 中可以看到：

```text
pango_pci_driver: loading out-of-tree module taints kernel.
init_pci_pango.
lock init.
init_pango_cdev.
alloc_chrdev_region, result : 0
init_pango_pci_driver.
pci_register_driver, result : 0
init_pango_cdev_class.
class create success.
create device.
```

字符设备节点也已创建：

```text
/dev/pango_pci_driver
/sys/class/pango_pci_driver/pango_pci_driver
```

但是没有出现：

```text
pci_driver_probe.
dev vendor : ...
```

这说明 PCI 驱动注册成功了，但没有 PCIe 设备匹配到该驱动。

## 7. 当前 PCIe 链路状态

当前系统没有枚举到 PCIe Endpoint：

```text
lspci -nn
# 无输出

ls /sys/bus/pci/devices
# 为空
```

`dmesg` 里有：

```text
rk-pcie 3c0000000.pcie: PCIe Linking... LTSSM is 0x3
rk-pcie 3c0800000.pcie: PCIe Linking... LTSSM is 0x0
rk-pcie 3c0000000.pcie: PCIe Link Fail
rk-pcie 3c0000000.pcie: failed to initialize host
rk-pcie 3c0800000.pcie: PCIe Link Fail
rk-pcie 3c0800000.pcie: failed to initialize host
```

这表示 Rockchip PCIe Root Complex 已经尝试训练链路，但链路没有起来。

## 8. PERST# 分析

FPGA 端 PCIe IP 要求：

```text
复位信号低有效
异步复位
来自插槽 PERST#
```

从当前设备树看，鲁班猫 Linux 已经给 PCIe host 配置了 reset GPIO。

### miniPCIe 相关节点

大概率 miniPCIe 对应：

```text
/proc/device-tree/pcie@fe260000
platform device: 3c0000000.pcie
```

关键属性：

```text
status: okay
num-lanes: 1
max-link-speed: 2
reset-gpios: <gpio@fe760000 pin 17 flags 0>
vpcie3v3-supply: mini-pcie-3v3-regulator
```

对应 GPIO：

```text
gpio@fe760000
gpiochip3
line 17
GPIO3_C1
```

GPIO debug 状态：

```text
gpiochip3:
 gpio-113 (reset)       out hi
 gpio-115 (minipcie_3v3) out hi
```

解释：

- `gpio-113` 是 PCIe reset GPIO，大概率就是 miniPCIe 插槽 `PERST#`。
- 当前输出高电平。
- 对低有效 `PERST#` 来说，低电平表示复位有效，高电平表示复位释放。
- `gpio-115` 是 miniPCIe 3.3V 供电使能，当前也是高电平。

因此，Linux 并不是完全没有给复位信号。更可能的问题是启动时序：Linux 启动早期已经释放 `PERST#` 并尝试链路训练，但当时 FPGA Endpoint 还没有 ready。

需要特别注意：`PERST#` 不应该一直保持低电平。正常 PCIe 复位时序应是：

```text
上电早期：PERST# = 0，复位有效，FPGA PCIe Endpoint 被按住复位
供电/REFCLK 稳定后：PERST# = 1，复位释放，Endpoint 开始工作并参与链路训练
```

所以 Linux 启动后看到：

```text
gpio-113 (reset) out hi
```

本身是合理的，表示当前复位已经释放。真正要确认的是上电或重启早期 `PERST#` 是否出现过“先低后高”的复位脉冲。如果它从上电开始一直是高，FPGA 端可能没有经历 PCIe 规范要求的复位；如果它一直是低，FPGA PCIe Endpoint 会一直处于复位状态，链路也不可能起来。

## 9. 为什么后加载 FPGA 位流容易失败

PCIe Endpoint 必须在 Root Complex 扫描窗口内 ready。

如果流程是：

1. 鲁班猫先启动 Linux；
2. Linux PCIe host 开始训练链路；
3. FPGA 此时还没加载 PCIe Endpoint 位流；
4. Rockchip PCIe host 报 `PCIe Link Fail`；
5. 后面再通过 JTAG 加载 FPGA full bit；

那么 Linux 通常不会自动重新枚举这个 Endpoint。此时即使驱动 `.ko` 加载成功，也没有设备可绑定。

## 10. 推荐上电顺序

当前实际使用的上电/连接顺序是：

```text
1. 先启动鲁班猫
2. 进入 Linux 系统桌面
3. 再插转接板
4. 再插 FPGA
5. FPGA 上电
6. FPGA 初始化/加载位流
```

这个顺序不适合 PCIe。PCIe 不是 USB，通常不能在 Linux 已经启动完成后再热插转接板和 Endpoint。鲁班猫在启动早期就会初始化 PCIe Root Complex、控制 `PERST#`、训练链路并扫描设备。如果那个时间点 FPGA Endpoint 还没有上电、没有加载 PCIe 位流或 PCIe IP 尚未 ready，内核就会报 `PCIe Link Fail`。后续即使 FPGA 再初始化完成，Linux 通常也不会自动重新枚举。

推荐顺序是：

```text
1. 断电状态下，先把转接板接到鲁班猫 miniPCIe
2. 把 FPGA 板接到转接板 PCIe 口
3. FPGA 先上电
4. FPGA 加载/固化 PCIe Endpoint 位流，并确保 PCIe IP ready
5. 再给鲁班猫上电，或者重启鲁班猫
6. Linux 启动早期释放 PERST#，开始 PCIe 链路训练和枚举
7. 进入系统后执行 lspci -nn
8. 确认能看到 FPGA PCIe 设备后，再加载驱动或运行 run.sh
```

更理想的工程化流程是：

```text
FPGA 上电 -> 自动从 Flash/Tandem 加载 PCIe Endpoint -> Endpoint ready
鲁班猫上电/重启 -> Linux 扫描 PCIe -> 枚举到 FPGA
```

如果必须通过 JTAG 手动加载 FPGA 位流，至少应按这个流程：

```text
1. 鲁班猫关机或断电
2. 接好鲁班猫、转接板、FPGA
3. FPGA 上电
4. 通过 JTAG 加载 PCIe Endpoint 位流
5. 确认 FPGA PCIe IP ready
6. 再给鲁班猫上电，或者重启鲁班猫
7. 进入 Linux 后执行 lspci -nn
```

一句话总结：FPGA PCIe Endpoint 必须在鲁班猫启动扫描 PCIe 之前就存在并 ready。

## 11. 驱动加载时机

`pango_pci_driver.ko` 应该在 Linux 已经枚举到 FPGA PCIe Endpoint 之后加载。驱动本身不会让 PCIe 链路从无到有建立起来，它只是注册一个 PCI driver，等待内核 PCI 总线上出现匹配的 Vendor ID / Device ID。

正确顺序是：

```text
1. FPGA 插好、上电、PCIe Endpoint 位流已加载、PCIe IP ready
2. 鲁班猫上电或重启
3. Linux 启动早期扫描 PCIe
4. 用 lspci -nn 确认能看到 FPGA 设备
5. 再加载 pango_pci_driver.ko，或者运行 ./run.sh
```

因此，如果当前状态是：

```text
FPGA 已经插上转接板
FPGA 已经上电
FPGA 已经初始化完成
鲁班猫当前系统是在这些动作之前启动的
```

那么下一步可以直接重启鲁班猫进行验证，但要保持 FPGA 不断电、转接板不拔出，并确保 FPGA PCIe Endpoint 已经 ready：

```sh
sudo reboot
```

重启进入系统后，先不要急着运行驱动，先确认 PCIe 枚举：

```sh
dmesg | grep -i pcie
lspci -nn
ls -la /sys/bus/pci/devices
```

如果 `lspci -nn` 能看到 FPGA 设备，例如：

```text
0755:0755
```

再运行：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc
sudo ./run.sh
```

如果重启后 `lspci -nn` 仍然为空，说明问题不是驱动加载时机，而是 FPGA Endpoint 仍没有在鲁班猫 PCIe 扫描时 ready，或者 `PERST# / REFCLK / TX/RX / lane / speed / 供电` 等链路条件仍有问题。

## 12. 建议排查顺序

### 12.1 软件侧确认

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc/driver
sudo make clean
sudo make
ls -l pango_pci_driver.ko
```

```sh
sudo insmod pango_pci_driver.ko
lsmod | grep pango_pci_driver
dmesg | tail -n 80
```

### 12.2 PCIe 枚举确认

```sh
lspci -nn
ls -la /sys/bus/pci/devices
dmesg | grep -i pcie
```

期望看到类似：

```text
0755:0755
```

如果没有任何 PCIe 设备，说明问题仍在链路或硬件时序，不在 Linux 字符设备驱动。

### 12.3 PERST# 和供电确认

```sh
sudo cat /sys/kernel/debug/gpio | grep -A12 gpiochip3
```

关注：

```text
gpio-113 (reset) out hi
gpio-115 (minipcie_3v3) out hi
```

建议用示波器或逻辑分析仪测 miniPCIe 插槽 `PERST#`：

1. 上电或启动早期应有低电平复位。
2. 随后释放到高电平。
3. 如果一直低，说明复位没有释放。
4. 如果一直高，可能没有复位脉冲，或者测量点/转接线不对。
5. 只看 Linux 启动后的 GPIO debug 状态不够，因为那通常只能看到复位释放后的最终电平。

### 12.4 FPGA 端确认

重点确认：

1. FPGA PCIe Endpoint IP 是否在鲁班猫 PCIe 扫描前 ready。
2. 是否需要使用 flash 启动或 Tandem PCIe 流程，而不是 Linux 启动后再 JTAG 下载 full bit。
3. `PERST#` 是否真正接到了 FPGA PCIe IP 的低有效异步复位输入。
4. REFCLK 是否来自 miniPCIe 插槽并稳定接入 FPGA。
5. TX/RX 是否交叉正确。
6. lane 宽度和速率是否匹配。当前 miniPCIe 节点是 `num-lanes = 1`，`max-link-speed = 2`。
7. FPGA bitstream 中 Vendor ID / Device ID 是否确实是 `0x0755:0x0755`。

如果 FPGA 实际 ID 不是 `0x0755:0x0755`，需要同步修改：

- `pango_pcie_dma_alloc/driver/id_config.h`
- `pango_pcie_dma_alloc/run.sh`

## 13. 枚举不到的可能原因与排错路径

当前现象是：

```text
lspci -nn 无输出
/sys/bus/pci/devices 为空
rk-pcie 3c0000000.pcie: PCIe Linking... LTSSM is 0x3
rk-pcie 3c0000000.pcie: PCIe Link Fail
```

这说明问题还没有进入 `pango_pci_driver.ko` 驱动绑定阶段。Linux PCIe Root Complex 没有枚举出任何 Endpoint，所以优先排查链路层、复位时序、参考时钟、供电和 FPGA PCIe IP ready 状态。

### 13.1 先确认排查边界

在 `lspci -nn` 为空时，不要优先调应用或字符设备驱动。此时 `pango_pci_driver.ko` 加载与否并不能让 PCIe 设备出现。正确边界是：

```text
先让 lspci 能看到 FPGA PCIe 设备
再看 pango_pci_driver 是否 probe
最后再调 BAR / DMA / 应用程序
```

最小验证命令：

```sh
dmesg | grep -i pcie
lspci -nn
ls -la /sys/bus/pci/devices
```

只要 `lspci -nn` 仍然没有 FPGA 设备，问题就仍在枚举前。

### 13.2 最可能原因 1：FPGA Endpoint 没有在扫描前 ready

这是当前最可疑的问题。PCIe Endpoint 必须在鲁班猫启动扫描 PCIe 之前已经 ready。如果 Linux 已经启动完成后才插 FPGA、给 FPGA 上电、JTAG 下载位流，通常会错过枚举窗口。

正确验证流程：

```text
1. 鲁班猫关机或断电
2. 转接板和 FPGA 保持连接
3. FPGA 上电
4. FPGA 加载 PCIe Endpoint 位流
5. 确认 FPGA PCIe IP ready
6. 鲁班猫上电或重启
7. Linux 启动后执行 lspci -nn
```

如果 FPGA 不能在鲁班猫扫描前自动 ready，建议使用 FPGA Flash/Tandem 启动，而不是 Linux 启动后再手动 JTAG 加载 full bit。

### 13.3 最可能原因 2：PERST# 时序或连接不正确

FPGA 端要求低有效异步复位，来自插槽 `PERST#`。对低有效 `PERST#`：

```text
PERST# = 0：复位有效
PERST# = 1：复位释放
```

当前 Linux 启动后能看到：

```text
gpio-113 (reset) out hi
```

这只能说明当前最终状态是复位释放，不能证明上电早期出现过“先低后高”的复位脉冲。启动日志也没有打印 `PERST#` 拉低和释放的过程。

需要用示波器或逻辑分析仪测 miniPCIe 插槽 `PERST#`：

```text
上电/重启早期：应为低电平
供电和 REFCLK 稳定后：应释放为高电平
```

异常判断：

```text
一直低：FPGA PCIe IP 一直处于复位，链路不会起来
一直高：可能没有给 Endpoint 做规范复位，或者测错点/转接板没接 PERST#
先高后低或抖动：复位时序异常，需要检查转接板、GPIO 极性、供电时序
```

还要确认 `PERST#` 是否真正接到了 FPGA PCIe IP 的低有效异步复位输入，而不是接到了普通 GPIO 或被板上逻辑反相。

### 13.4 最可能原因 3：REFCLK 没有到 FPGA

PCIe Endpoint 通常需要来自插槽的 100 MHz 差分参考时钟。如果 REFCLK 没有接入、幅度不对、端接不对或 FPGA IP 没有使用该时钟，Root Complex 会一直训练失败。

需要检查：

```text
miniPCIe REFCLK+ / REFCLK- 是否经过转接板接到 FPGA PCIe 参考时钟引脚
FPGA 约束是否把 REFCLK 约束到了正确管脚
参考时钟是否为 PCIe IP 所使用的 refclk
示波器是否能看到稳定差分时钟
```

如果 FPGA PCIe IP 使用了板上本地时钟，而不是插槽 REFCLK，需要确认该 IP 和转接方案是否支持这种模式。常规 PCIe Endpoint 推荐使用 Root Complex 提供的 REFCLK。

### 13.5 最可能原因 4：转接板 TX/RX 或 lane 接线问题

PCIe 差分对必须正确交叉：

```text
Root Complex TX -> Endpoint RX
Endpoint TX -> Root Complex RX
```

需要确认：

```text
TX/RX 是否交叉正确
P/N 极性是否正确，或 FPGA IP 是否开启极性翻转支持
lane 数是否匹配，当前 miniPCIe 节点是 x1
FPGA PCIe IP 是否配置为 x1，而不是 x2/x4
转接板是否把 miniPCIe 的 PCIe lane 接到了 FPGA 的正确 lane
```

当前 miniPCIe 相关设备树显示：

```text
num-lanes: 1
max-link-speed: 2
```

因此建议 FPGA 端先用最保守配置验证：

```text
Endpoint 模式
x1
Gen1 优先，跑通后再尝试 Gen2
Vendor ID / Device ID 明确可见
```

### 13.6 最可能原因 5：FPGA PCIe IP 配置不匹配

需要确认 FPGA bitstream 中 PCIe IP 是 Endpoint 模式，而不是 Root Complex 或其他模式。还要确认：

```text
Vendor ID / Device ID 是否确实是 0x0755:0x0755
BAR 是否至少配置了一个合法空间
Class code 是否合理
MSI/MSI-X 配置不会影响最小枚举
链路宽度和速率是否与鲁班猫端口兼容
复位输入是否使用 PERST#，且极性为低有效
```

如果 FPGA 实际 ID 不是 `0x0755:0x0755`，即使枚举成功，当前驱动和 `run.sh` 也不会按预期匹配，需要修改：

```text
pango_pcie_dma_alloc/driver/id_config.h
pango_pcie_dma_alloc/run.sh
```

不过注意：Vendor ID 不匹配不会导致 `lspci` 为空。只要链路和配置空间正常，`lspci` 仍应能看到设备。

### 13.7 最可能原因 6：鲁班猫端口或转接目标不对

当前设备树里两个启用的 PCIe host 都尝试过链路训练：

```text
pcie@fe260000 -> 3c0000000.pcie，x1，关联 mini-pcie-3v3-regulator
pcie@fe280000 -> 3c0800000.pcie，x2，关联 pcie30_3v3
```

miniPCIe 更可能对应 `3c0000000.pcie`。如果转接板实际接到另一组 PCIe 通道，或者板卡座子与设备树启用端口不一致，也会枚举不到。

建议做一个交叉验证：

```text
用已知可工作的 miniPCIe 设备测试鲁班猫插槽，例如 miniPCIe 网卡
如果已知设备也枚举不到，优先查鲁班猫端口、转接板、供电和设备树
如果已知设备能枚举，优先查 FPGA Endpoint、PERST#、REFCLK、TX/RX 和 IP 配置
```

### 13.8 推荐排错顺序

按下面顺序排，避免一上来就陷入驱动细节：

```text
1. 不加载 pango 驱动，只看 lspci 是否能看到任何 FPGA PCIe 设备
2. 保持 FPGA ready，重启鲁班猫，再查 lspci -nn
3. 用示波器确认 3.3V、PERST#、REFCLK
4. 用 FPGA ILA/调试信号观察 PCIe IP LTSSM 状态
5. 确认 FPGA PCIe IP 为 Endpoint / x1 / Gen1 或 Gen2 兼容
6. 确认 TX/RX 差分对和 P/N 极性
7. 用已知可工作的 miniPCIe 设备验证鲁班猫插槽和转接板
8. lspci 能看到 FPGA 后，再加载 pango_pci_driver.ko
9. 驱动 probe 后，再调 BAR 和 DMA
```

### 13.9 期望的正确结果

正确结果应该按阶段出现：

第一阶段，PCIe 枚举成功：

```sh
lspci -nn
```

能看到 FPGA 设备，例如：

```text
0755:0755
```

第二阶段，驱动绑定成功：

```text
pci_driver_probe.
dev vendor : 0x755, device : 0x755
```

第三阶段，应用运行：

```sh
cd /home/cat/cat_pcie_project/pango_pcie_dma_alloc
sudo ./run.sh
```

只有第一阶段达成后，后面的驱动和应用调试才有意义。

## 14. 最可能的根因

当前最可能的根因是：

```text
FPGA PCIe Endpoint 没有在鲁班猫 Linux PCIe host 扫描时 ready，导致 Rockchip Root Complex 链路训练失败，系统没有枚举出 PCIe 设备。
```

软件驱动已经能编译并加载，但由于 `/sys/bus/pci/devices` 为空，驱动没有机会进入 `pci_driver_probe()`。

## 15. 推荐下一步

优先做这个启动流程：

1. 让 FPGA 先通过 flash 或 Tandem 方式自动加载 PCIe Endpoint。
2. 确认 FPGA PCIe IP ready 后，再给鲁班猫上电或重启鲁班猫。
3. 登录 Linux 后执行：

```sh
dmesg | grep -i pcie
lspci -nn
sudo ./run.sh
```

目标现象：

```text
lspci -nn
# 能看到 0755:0755 或 FPGA 实际 vendor/device id
```

随后 `dmesg` 中应出现驱动 probe 日志：

```text
pci_driver_probe.
dev vendor : 0x755, device : 0x755
```

只有达到这一步后，才进入 BAR 映射、DMA 测试、应用层 GUI/测试程序调试阶段。
