# Linux 端彩条 PCIe 接收实施路线

本文只保留当前正确路线，历史错误推断和过期调试步骤已经删除。

当前 FPGA 正确参考工程：

```text
PCIE_DMA_safe_test_4
```

当前 Linux 端工程：

```text
camara_host_computer/Colorbar_image
```

参考验证工程仍然是：

```text
pango_pcie_dma_alloc
```

但后续彩条接收不修改 `pango_pcie_dma_alloc`。

## 2026-07-19：当前正确结论

### 1. FPGA 端现在做了什么

已核查 `PCIE_DMA_safe_test_4/source/pcie_dma_ctrl.v`，当前 FPGA 端采用显式安全握手。

当前寄存器协议是：

```text
0x100 ARM   = 0xA55A5AA5
0x104 START = 1
0x108 LEN   = 本次 DMA 字节数
0x110 ADDR  = 连续写 4 个 Linux coherent DMA buffer 低 32 位地址
0x130 STOP  = 停止并清状态
0x170 STATUS = 预留/内部状态信号
```

关键代码位置：

```text
PCIE_DMA_safe_test_4/source/pcie_dma_ctrl.v
DMA_CMD_ARM_ADDR    = 12'h100
DMA_CMD_START_ADDR  = 12'h104
DMA_CMD_LEN_ADDR    = 12'h108
DMA_CMD_L_ADDR      = 12'h110
DMA_CMD_CLEAR_ADDR  = 12'h130
DMA_CMD_STATUS_ADDR = 12'h170
DMA_ARM_MAGIC       = 32'hA55A5AA5
```

DMA 使能门控在顶层已经接上：

```text
PCIE_DMA_safe_test_4/source/ddr_test_top.v
pcie_dma_enable = cfg_bus_master_en && smlh_link_up && rdlh_link_up
```

这表示 FPGA 必须同时满足：

```text
Linux 打开 BusMaster
PCIe smlh_link_up = 1
PCIe rdlh_link_up = 1
```

才允许 DMA 状态机工作。

### 2. 非常重要：命令数据字节序

`PCIE_DMA_safe_test_4/source/pcie_dma_ctrl.v` 对 host 写进来的 32-bit 数据做了字节反转：

```verilog
wire [31:0] cmd_data = {axis_master_tdata_d0[7:0], axis_master_tdata_d0[15:8],
                        axis_master_tdata_d0[23:16], axis_master_tdata_d0[31:24]};
```

所以 Linux 端写寄存器时要注意：

```text
Linux 想让 FPGA 看到 LEN=64，需要实际 iowrite32(0x40000000)
Linux 想让 FPGA 看到 START=1，需要实际 iowrite32(0x01000000)
Linux 写 DMA 地址也需要按这个字节序处理
```

`0xA55A5AA5` 是回文魔数，字节反转后还是 `0xA55A5AA5`，所以 ARM 魔数单独看不出问题。

当前 Linux 驱动已经按这个修正：

```text
LEN/START 使用 swab32() 后写入
ADDR 默认 addr_byteswap=1
```

### 3. FPGA LEN 的含义

FPGA 中的限制：

```text
DMA_DEFAULT_BYTES = 4147264
DMA_MAX_BYTES     = 4194304
TLP_LENGTH        = 16 DW = 64 bytes
```

Linux 当前默认测试全帧按：

```text
1920 * 1080 * 2 = 4147200 bytes
```

这正好是 RGB565 一帧图像大小，不包含额外 64B 标记区。

小步测试推荐：

```text
64 bytes
4096 bytes
4147200 bytes
```

不要跳过 64B 和 4KB 直接测全帧。

### 4. PCIE_DMA_safe_test_4 新增的读回和固定测试能力

`PCIE_DMA_safe_test_4` 已经新增 MRd/CplD 读回雏形，Linux 可以读：

```text
0x114 ADDR_ECHO0
0x118 ADDR_ECHO1
0x11c ADDR_ECHO2
0x120 ADDR_ECHO3
0x170 STATUS
```

当前 Linux 驱动已启用 `verify_readback=1`，在写 START 前会：

```text
1. 写 ARM/LEN/ADDR。
2. 打开 BusMaster，让 FPGA 的读回状态机可工作。
3. 读取 ADDR_ECHO0..3。
4. 和 Linux 分配的 DMA 地址低 32 位逐个比较。
5. 如果不一致，立即 STOP 并拒绝 START。
6. 读取 STATUS，若 addr_error=1，也拒绝 START。
7. 只有读回通过后，才写 START=1。
```

`STATUS` 位定义：

```text
bit0 busy
bit1 frame_done
bit3 addr_error
bit4 host_arm
bit5 host_start
bit6 pcie_dma_enable
bit7 rc_cfg_ep_flag
```

`PCIE_DMA_safe_test_4` 还新增 fixed_test_mode：

```text
64 <= dma_len_bytes < 4147264 时，FPGA 不依赖视频帧/FIFO，直接写固定测试数据。
```

固定测试数据来自 FPGA 内部：

```text
A5A50000, A5A50001, ... A5A5000F
```

因此 64B 测试现在应该看到 `A5A5...` 规律数据，而不是全 00 或全 A5。

## 2026-07-19：Linux 端已经修改的内容

### 1. 驱动文件

```text
camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.c
camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.h
```

已实现：

```text
1. 使用 32-bit DMA mask，匹配 FPGA MWR_32。
2. 使用 pci_alloc_consistent() 分配 coherent DMA buffer。
3. 默认 allow_dma_start=0，只加载驱动不会启动 DMA。
4. START 路径按新 FPGA 协议执行：STOP -> ARM -> LEN -> ADDR x4 -> BusMaster -> START。
5. LEN 和 START 写入时做 swab32()，匹配 FPGA cmd_data 字节反转。
6. ADDR 默认 addr_byteswap=1，匹配 FPGA 地址解析方式。
7. dma_len_bytes 强制 64B 起步并且 64B 对齐，匹配 PCIE_DMA_safe_test_4 的 dma_len_valid。
8. START 前读取 ADDR_ECHO0..3 和 STATUS，地址不一致就拒绝启动 DMA。
9. STOP 会清 START/ARM/LEN/ADDR。
10. read() 只允许用户态读取 dma_len_bytes 长度。
```

### 2. 用户态工具

```text
camara_host_computer/Colorbar_image/src/pcie_color_rx.c
```

已实现：

```text
1. --once 根据驱动返回的 valid_size 分配内存。
2. 64B 测试只保存 64B。
3. 4KB 测试只保存 4KB。
4. 只有完整 RGB565 一帧才做彩条采样校验。
5. 小包测试打印前 64 字节，方便看 DMA 是否写入。
```

### 3. 加载脚本

```text
camara_host_computer/Colorbar_image/scripts/load_driver.sh
```

默认参数：

```text
bar=1
addr_byteswap=1
frame_wait_ms=100
dma_len_bytes=64
verify_readback=1
allow_dma_start=0
```

默认加载不会启动 DMA，并且默认只按 64B 测试长度准备 DMA buffer，避免在安全验证阶段申请全帧大块 coherent 内存。

## 2026-07-19：当前安全测试完整命令

这一节是当前唯一推荐执行顺序。前面的安全指南内容已经汇总到这里，后续只看本文档。

### 0. 测试前安全前提

必须先确认：

```text
FPGA 已烧录 PCIE_DMA_safe_test_4 对应 bitstream
PCIe 已经 link up
lspci 能看到 01:00.0 Device 0755:0755
Linux 端当前目录是 camara_host_computer/Colorbar_image
```

先进入工程目录：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
```

确认当前目录：

```sh
pwd
```

预期：

```text
/home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
```

查看文件是否存在：

```sh
ls
ls scripts
ls driver/colorbar_pcie_driver.c
```

预期至少能看到：

```text
Makefile
build
scripts/load_driver.sh
driver/colorbar_pcie_driver.c
```

### 1. 编译 Linux 端程序和驱动

清理旧构建：

```sh
make clean
```

重新编译：

```sh
make
```

确认用户态程序存在：

```sh
ls -lh build/pcie_color_rx
```

确认驱动 ko 存在：

```sh
ls -lh driver/colorbar_pcie_driver.ko
```

预期：

```text
build/pcie_color_rx 存在
driver/colorbar_pcie_driver.ko 存在
```

这一步不会加载驱动，不会启动 DMA，风险低。

### 2. 查看 PCIe 枚举状态

查看设备：

```sh
lspci -nn
```

查看 FPGA 设备详细信息：

```sh
lspci -vv -s 01:00.0
```

查看 BAR 资源：

```sh
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

重点看：

```text
01:00.0 Device 0755:0755
LnkSta: Speed 2.5GT/s, Width x1
Region 1 存在，当前脚本默认使用 bar=1
```

这一步只是查询 PCIe 状态，不加载驱动，不启动 DMA，风险低。

### 3. 默认安全加载驱动，不启动 DMA

执行：

```sh
./scripts/load_driver.sh
```

这条命令实际加载参数是：

```text
bar=1
addr_byteswap=1
frame_wait_ms=100
dma_len_bytes=64
verify_readback=1
allow_dma_start=0
```

因此它不会允许 `START` 真正启动 DMA，并且只会按 64B 测试长度准备 DMA buffer。

确认设备节点：

```sh
ls -lh /dev/colorbar_pcie_rx
```

查看最近驱动日志：

```sh
sudo dmesg | tail -n 80
```

预期能看到类似：

```text
colorbar PCIe RX driver loaded
colorbar probe vendor=0x755 device=0x755
using 32-bit DMA mask
sent DMA STOP + cleared ARM/START/LEN/dma_addr0..3
```

这一步会加载驱动并写 STOP/清状态，但不会写 START 启动 DMA，风险低。

### 4. 手动发送 safe-stop

执行：

```sh
sudo ./build/pcie_color_rx --safe-stop
```

预期：

```text
sent safe stop to /dev/colorbar_pcie_rx
```

查看日志：

```sh
sudo dmesg | tail -n 80
```

预期看到：

```text
sent DMA STOP + cleared ARM/START/LEN/dma_addr0..3
```

这一步只停止和清状态，不启动 DMA，风险低。

### 5. 验证默认安全闸门有效

执行：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start.bin
```

预期必须是：

```text
COLORBAR_IOC_START: Operation not permitted
```

这个报错是正确结果，说明：

```text
allow_dma_start=0 生效
默认状态下 --once 不能启动 DMA
```

确认不会生成有效数据文件：

```sh
ls -lh /tmp/should_not_start.bin
```

如果提示文件不存在，也是正常的。

这一步不会真正启动 DMA，风险低。

如果看到下面这种输出：

```text
COLORBAR_IOC_ALLOC_BUFS: Cannot allocate memory
```

含义是驱动在申请 DMA coherent buffer 时失败，还没有走到 START，也没有启动 FPGA DMA。原因通常是当前加载的旧驱动或旧脚本仍按全帧大小一次申请 4 个大 buffer。处理方法是重新编译并重新加载当前新版本：

```sh
make clean
make
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start.bin
```

重新加载后，如果安全闸门正常，应看到 `Operation not permitted`，而不是 `Cannot allocate memory`。

### 6. 64B 真实 DMA 小步测试

只有前面 1 到 5 步都正常，并且确认 FPGA 已烧录 `PCIE_DMA_safe_test_4` 后，才执行 64B。

重新加载驱动，显式允许 DMA，并把长度限制为 64B：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
```

查看加载日志：

```sh
sudo dmesg | tail -n 100
```

执行一次 64B 采集：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_64.bin
```

确认文件大小：

```sh
ls -lh /tmp/frame_64.bin
```

查看前 64 字节：

```sh
hexdump -C /tmp/frame_64.bin | head
```

查看驱动日志：

```sh
sudo dmesg | tail -n 120
```

64B 测试预期：

```text
/tmp/frame_64.bin 大小为 64 字节
系统不崩溃
用户态打印 first 64 byte(s)
dmesg 里能看到 len=64
dmesg 里能看到 LEN written=0x40000000
dmesg 里能看到 read ADDR_ECHO0..3 且 got=expected
dmesg 里能看到 read STATUS before START
dmesg 里能看到 START written=0x01000000
hexdump 应该看到 A5A50000..A5A5000F 相关规律数据
```

64B 是第一条真实 DMA 测试，有风险，但风险被限制在最小长度。若这一步异常，不能继续测试 4KB 或全帧。

64B 如果读出来全是 `00`，不要马上判断成功。旧版本驱动会在 START 前把 DMA buffer 清 0，因此“全 00”可能有两种情况：

```text
FPGA 真的写入了 0 数据
FPGA 没有写入，Linux 读到的是驱动预清零内容
```

当前驱动已经改为 START 前用 `0xCD` 预填充 DMA buffer。`0xCD` 和 FPGA fixed_test_mode 的 `A5A50000..A5A5000F` 明显不同，便于判断 DMA 是否真的改写了 host buffer。重新编译并重复 64B 后，判断方式如下：

```text
如果 /tmp/frame_64.bin 仍然全是 cd，说明 FPGA 大概率没有写入 host buffer。
如果 /tmp/frame_64.bin 出现 A5A50000..A5A5000F 规律数据，说明 PCIE_DMA_safe_test_4 的 fixed_test_mode 已经写入成功。
如果系统不崩溃但数据全 cd，下一步查 FPGA 的 host_start_flag、pcie_dma_enable、AXIS_S_TVALID/TREADY/TLAST。
```

重复 64B 的命令：

```sh
make clean
make
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_prefill.bin
ls -lh /tmp/frame_64_prefill.bin
hexdump -C /tmp/frame_64_prefill.bin | head
sudo dmesg | tail -n 120
```

### 7. 4KB 真实 DMA 测试

只有 64B 稳定后才执行。

重新加载驱动，设置 4KB：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4096
```

执行一次 4KB 采集：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_4k.bin
```

确认文件大小：

```sh
ls -lh /tmp/frame_4k.bin
```

查看数据：

```sh
hexdump -C /tmp/frame_4k.bin | head
```

查看驱动日志：

```sh
sudo dmesg | tail -n 120
```

4KB 测试预期：

```text
/tmp/frame_4k.bin 大小为 4096 字节
系统不崩溃
数据不是全 00 或全 ff
dmesg 里能看到 len=4096
```

### 8. RGB565 全帧 DMA 测试

只有 64B 和 4KB 都稳定后才执行。

重新加载驱动，设置完整 RGB565 一帧：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4147200
```

执行一次完整帧采集：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
```

确认文件大小：

```sh
ls -lh /tmp/frame_test.rgb565
```

校验彩条采样点：

```sh
sudo ./build/pcie_color_rx --validate /tmp/frame_test.rgb565
```

查看驱动日志：

```sh
sudo dmesg | tail -n 120
```

全帧测试预期：

```text
/tmp/frame_test.rgb565 大小为 4147200 字节
--validate 打印 white/yellow/cyan/green/magenta/red/blue/black 采样点
所有采样点 OK
系统不崩溃
```

### 9. 每次测试结束后的收尾

测试结束后发送 safe-stop：

```sh
sudo ./build/pcie_color_rx --safe-stop
```

卸载驱动：

```sh
sudo rmmod colorbar_pcie_driver
```

确认驱动已卸载：

```sh
lsmod | grep colorbar_pcie_driver
```

如果没有输出，说明已卸载。

查看最后日志：

```sh
sudo dmesg | tail -n 80
```

### 10. 风险等级总结

```text
make clean / make                                      风险低，不碰 DMA
lspci / cat resource                                   风险低，只查询 PCIe
./scripts/load_driver.sh                               风险低，默认 allow_dma_start=0，dma_len_bytes=64，verify_readback=1
--safe-stop                                            风险低，只 STOP/清状态
--once 且 allow_dma_start=0                             风险低，应该被拒绝
allow_dma_start=1 dma_len_bytes=64 + --once             有真实 DMA 风险，但长度最小
allow_dma_start=1 dma_len_bytes=4096 + --once           有真实 DMA 风险，必须 64B 通过后再做
allow_dma_start=1 dma_len_bytes=4147200 + --once        全帧 DMA，最后才做
```

### 11. 立刻停止的条件

出现下面任意情况，停止测试，不要扩大 DMA 长度：

```text
64B 测试卡死、重启、文件系统异常
/tmp/frame_64.bin 不是 64 字节
START 日志不是 written=0x01000000
LEN=64 日志不是 written=0x40000000
ADDR_ECHO0..3 任意一个 got != expected
STATUS 里 addr_error=1
FPGA ILA 看到 cmd_data 不等于预期
FPGA ILA 看到 pcie_dma_enable=0
dmesg 出现 kernel oops、SError、PCIe AER 异常
```

### 12. 当前风险边界

`0x170 STATUS` 在 `PCIE_DMA_safe_test_4` 中已经可以通过 PCIe MRd 读到。当前 Linux 驱动先采用保守方式：START 后等待 `frame_wait_ms`，然后在 STOP 前读取一次 STATUS，用它辅助判断 FPGA 状态机有没有启动、完成或报地址错误。

后续如果 STATUS 行为稳定，Linux 驱动可以从固定延时改成轮询 `frame_done_flag`，但在 64B 数据真正落到 host buffer 前，暂时不扩大 DMA 长度。



## 2026-07-20：PCIE_DMA_safe_test_4 ADDR_ECHO0 为 0 的修正

### 1. 本次现象

执行：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_test4.bin
```

返回：

```text
COLORBAR_IOC_START: Input/output error
```

dmesg 关键日志：

```text
program DMA command: offset=0x100 value=0xa55a5aa5 written=0xa55a5aa5
program DMA command: offset=0x108 value=0x00000040 written=0x40000000
program DMA address: dma=0x00000000eec03000 low32=0xeec03000 written=0x0030c0ee addr_byteswap=1
read ADDR_ECHO0: got=0x00000000 expected=0xeec03000
refuse to start FPGA DMA: ADDR_ECHO0 mismatch
```

这次不是系统崩溃，也不是 DMA 已经写坏内存。恰恰相反，是 Linux 驱动的保护生效：读回地址不一致，所以驱动拒绝写 `START=1`。

### 2. 原因分析

`PCIE_DMA_safe_test_4/source/pcie_dma_ctrl.v` 里命令解析逻辑受这个条件影响：

```text
pcie_dma_enable = cfg_bus_master_en && smlh_link_up && rdlh_link_up
```

并且在命令解析 always 块里：

```verilog
if(!rstn || !pcie_dma_enable || dma_stop_flag) begin
    dma_addr0 <= 0;
    dma_addr1 <= 0;
    dma_addr2 <= 0;
    dma_addr3 <= 0;
    host_arm_flag <= 0;
    host_start_flag <= 0;
    dma_len_bytes <= 0;
end
```

也就是说，如果 Linux 在 BusMaster 关闭时写 `ARM/LEN/ADDR`，FPGA 端会因为 `pcie_dma_enable=0` 处于清零/复位状态，这些命令不会被保存。所以读 `ADDR_ECHO0` 时得到 0。

### 3. Linux 驱动修正

已修正 `colorbar_pcie_driver.c`：

```text
旧顺序：
关闭 BusMaster -> 写 STOP/ARM/LEN/ADDR -> 打开 BusMaster -> 读 ADDR_ECHO -> START

新顺序：
关闭 BusMaster -> 打开 BusMaster -> 等待 10us -> STOP 清状态 -> ARM -> LEN -> ADDR x4 -> 读 ADDR_ECHO/STATUS -> START
```

STOP 路径也已修正：

```text
旧顺序：
关闭 BusMaster -> STOP

新顺序：
打开 BusMaster -> 等待 10us -> STOP -> 关闭 BusMaster
```

这样做的原因：当前 FPGA 版本要求 `pcie_dma_enable=1` 时才接收 host 写入命令。安全性来自 `ARM/START` 显式握手：打开 BusMaster 后，如果没有写合法 START，FPGA 不应该主动 DMA。

### 4. 现在重新测试命令

先重新加载新驱动：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
```

再执行 64B：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_test4_retry.bin
```

查看结果：

```sh
ls -lh /tmp/frame_64_test4_retry.bin
hexdump -C /tmp/frame_64_test4_retry.bin | head
sudo dmesg | tail -n 180
```

预期：

```text
ADDR_ECHO0..3 的 got 应该等于 expected
STATUS before START 不应该显示 addr_error=1
START written=0x01000000
/tmp/frame_64_test4_retry.bin 大小为 64 字节
hexdump 应该看到 A5A50000..A5A5000F 相关固定测试数据
```

如果仍然 `ADDR_ECHO0 got=0`，说明 FPGA 端 MRd/CplD 读回路径或命令保存路径还没有真正工作，继续禁止 4KB/全帧。



## 2026-07-20：ADDR_ECHO 读回字节序修正

### 1. 本次现象

重新调整 BusMaster 顺序后，`ADDR_ECHO0` 已经不再是 0，而是：

```text
read ADDR_ECHO0: got=0x0030c0ee expected=0xeec03000
```

这说明 FPGA 已经收到并保存了 Linux 写入的地址，只是读回到 Linux 时仍然带着 FPGA CplD 数据通道的字节反转。

Linux 写入地址时：

```text
expected low32 = 0xeec03000
written        = 0x0030c0ee
```

FPGA ADDR_ECHO 原始读回：

```text
raw = 0x0030c0ee
```

Linux 需要再做一次 `swab32(raw)`，才能得到：

```text
value = 0xeec03000
```

### 2. Linux 驱动修正

已新增读回解码：

```text
colorbar_read_cmd32()
raw = ioread32(BAR + offset)
value = swab32(raw)
```

现在 `ADDR_ECHO0..3` 按 `swab32(raw)` 后的 `value` 判断；`STATUS` 按 raw 低位判断，不再做 `swab32()`。

### 3. 重新测试命令

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_test4_readfix.bin
ls -lh /tmp/frame_64_test4_readfix.bin
hexdump -C /tmp/frame_64_test4_readfix.bin | head
sudo dmesg | tail -n 200
```

这次预期日志应该看到：

```text
read DMA command: offset=0x114 raw=0x0030c0ee value=0xeec03000
read ADDR_ECHO0 decoded=0xeec03000 expected=0xeec03000
read ADDR_ECHO1 decoded=... expected=...
read ADDR_ECHO2 decoded=... expected=...
read ADDR_ECHO3 decoded=... expected=...
program DMA command: offset=0x104 value=0x00000001 written=0x01000000
```

如果这些都通过，才说明 START 真正写出去了。随后看 `/tmp/frame_64_test4_readfix.bin` 是否是 64 字节，并且是否出现 `A5A50000..A5A5000F` 规律数据。



## 2026-07-20：ADDR_ECHO 和 START 已通过，但 64B buffer 未被改写

### 1. 本次结果

执行 `PCIE_DMA_safe_test_4` 64B 测试后：

```text
ADDR_ECHO0..3 全部 decoded=expected
STATUS before START raw 为 0x00000050
START written=0x01000000
/tmp/frame_64_test4_readfix.bin 大小为 64 字节
文件内容全是 a5
系统没有崩溃
```

关键结论：

```text
Linux -> FPGA 的 ARM/LEN/ADDR/ADDR_ECHO/START 控制链路已经基本打通。
但是 FPGA 的 MWR 数据还没有确认写进 Linux host buffer。
```

旧日志里全 `a5` 的含义是：当时 Linux 驱动在 START 前用 `0xA5` 预填充了 DMA buffer，测试后还是 `0xA5`，说明 host buffer 内容没有变化。当前新驱动已改成预填 `0xCD`，新测试应按“是否全 cd”判断。

### 2. 为什么还不能测 4KB

`PCIE_DMA_safe_test_4` 的 64B fixed_test_mode 理论上应该写固定数据：

```text
A5A50000, A5A50001, ..., A5A5000F
```

如果 Linux 读出来仍然全是：

```text
cd cd cd cd ...
```

就不能证明 FPGA MWR 已经落到主机内存。因此暂时不能扩大到 4KB 或全帧。

### 3. Linux 驱动新增日志

已修正 STATUS 解析：

```text
STATUS 按 raw 低 8 位解释，不再 swab32 后解释。
```

已新增等待结束前状态读取：

```text
read STATUS after wait before STOP: ...
```

下一次 64B 测试要看 START 后 100ms 内，FPGA 是否出现：

```text
busy=1 或 done=1
start=1 曾经有效
rc_cfg_ep=1 曾经有效
addr_error=0
pcie_dma_enable=1
```

### 4. 重新测试命令

注意：如果 dmesg 里还出现下面这种日志，说明当前正在运行的还是旧 ko：

```text
read DMA command: offset=0x170 raw=0x00000050 value=0x50000000
read STATUS before START: 0x50000000 ... pcie_dma_enable=0
```

新驱动应该直接按 raw STATUS 打印，例如：

```text
read STATUS before START: 0x00000050 ... arm=1 ... pcie_dma_enable=1
```

所以重新测试前必须重新编译并重新加载驱动：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make clean
make
sudo rmmod colorbar_pcie_driver
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_status.bin
ls -lh /tmp/frame_64_status.bin
hexdump -C /tmp/frame_64_status.bin | head
sudo dmesg | tail -n 220
```

### 5. 判断方式

如果看到：

```text
ADDR_ECHO0..3 全部 decoded=expected
START written=0x01000000
read STATUS after wait before STOP: ... done=1 ...
hexdump 仍然全 cd
```

说明 FPGA 状态机可能认为完成，但 MWR 数据没有真正落到 Linux 地址，需要 FPGA 端查 MWR TLP 格式、AXIS_S_TVALID/TREADY/TLAST、目标地址字段。

如果看到：

```text
ADDR_ECHO0..3 全部 decoded=expected
START written=0x01000000
read STATUS after wait before STOP: ... busy=0 done=0 start=0 rc_cfg_ep=0 ...
hexdump 全 cd
```

说明 FPGA START 后没有进入 DMA 发送状态，重点查 `host_start_flag`、`fixed_test_mode`、`dma_len_valid`、`mwr_state`。

如果看到文件出现 `A5A50000..A5A5000F` 规律数据，才允许进入 4KB。

## 2026-07-20：本次日志结论和下一条命令

### 1. 本次日志说明什么

用户本次日志里已经看到：

```text
read ADDR_ECHO0 decoded=0xeec03000 expected=0xeec03000
read ADDR_ECHO1 decoded=0xeec04000 expected=0xeec04000
read ADDR_ECHO2 decoded=0xeec05000 expected=0xeec05000
read ADDR_ECHO3 decoded=0xeec06000 expected=0xeec06000
program DMA command: offset=0x104 value=0x00000001 written=0x01000000
captured frame_counter=1 buffer=0 bytes=64
```

这说明：

```text
Linux 写 BAR1 命令没有问题。
ARM/LEN/ADDR 四个地址已经被 FPGA 保存并可读回。
START 已经写出。
64B 测试没有导致系统崩溃。
```

但是输出文件仍然是：

```text
cd cd cd cd ...
```

这不是成功图像数据。当前新驱动在 START 前会把 DMA buffer 预填成 `0xCD`，如果测试后仍然全 `0xCD`，表示 FPGA 的 MWr 数据还没有确认写进 Linux buffer。

### 2. 为什么这次还不能测 4KB

`PCIE_DMA_safe_test_4` 的 64B fixed_test_mode 理论预期是固定 16 个 32-bit word：

```text
A5A50000
A5A50001
...
A5A5000F
```

考虑字节序，hexdump 可能表现为下列两类之一：

```text
00 00 a5 a5 01 00 a5 a5 ...
a5 a5 00 00 a5 a5 00 01 ...
```

但不应该是整段全 `cd`。

所以当前不能测：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4096
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4147200
```

### 3. 下一步只做这一组命令

执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make clean
make
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_status.bin
ls -lh /tmp/frame_64_status.bin
hexdump -C /tmp/frame_64_status.bin | head
sudo dmesg | tail -n 220
```

这组命令的目的不是测图像，而是只验证 64B 的 fixed_test_mode 是否真的写入 host buffer。

### 4. 下一步重点看什么

先看 dmesg 是否已经是新驱动格式：

```text
read STATUS before START: 0x00000050 ...
read STATUS after wait before STOP: ...
```

再看 `after wait before STOP`：

```text
done=1, addr_error=0
```

如果 `done=1` 但 hexdump 仍然全 `cd`，结论偏向 FPGA MWr TLP 没有真正落到 RK3568 内存，FPGA 端需要查 MWr 数据通道。

如果 `done=0` 且 hexdump 全 `cd`，结论偏向 FPGA START 后没有进入发送状态，FPGA 端需要查 `host_start_flag`、`fixed_test_mode`、`dma_len_valid`、`mwr_state`。

如果 hexdump 出现 `A5A50000..A5A5000F` 的规律数据，64B 通过，才进入 4KB。

## 2026-07-19：如果还是异常，下一步查什么

如果 64B 都异常，不要测 4KB 或全帧，直接查下面几项。

### 1. Linux 侧看 dmesg

```sh
sudo dmesg -w
```

重点看：

```text
program DMA command: offset=0x100 value=0xa55a5aa5 written=0xa55a5aa5
program DMA command: offset=0x108 value=0x00000040 written=0x40000000
program DMA address: dma=... low32=... written=... addr_byteswap=1
program DMA command: offset=0x104 value=0x00000001 written=0x01000000
started colorbar RX with ARM/LEN/ADDR/START handshake, len=64
```

如果 `START written` 不是 `0x01000000`，说明 Linux 驱动不是当前新版本。

### 2. Linux 侧确认 PCIe 状态

```sh
lspci -nn
lspci -vv -s 01:00.0
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

重点看：

```text
LnkSta: Speed 2.5GT/s, Width x1
Control: BusMaster+ 在 DMA 测试窗口内出现
Region 1: Memory at ... size=4K 或控制 BAR 正常存在
```

### 3. FPGA 侧 ILA 需要抓的信号

```text
cmd_reg_addr
cmd_data
host_arm_flag
host_start_flag
dma_len_bytes
dma_addr0..dma_addr3
dma_addr_valid
dma_len_valid
addr_error_flag
pcie_dma_enable
cfg_bus_master_en
smlh_link_up
rdlh_link_up
rc_cfg_ep_flag
frame_done_flag
dma_cnt
AXIS_S_TVALID
AXIS_S_TREADY
AXIS_S_TLAST
```

64B 测试时 FPGA 端应该看到：

```text
cmd_reg_addr = 0x100, cmd_data = 0xA55A5AA5
cmd_reg_addr = 0x108, cmd_data = 0x00000040
cmd_reg_addr = 0x110, cmd_data = Linux DMA 地址低 32 位
cmd_reg_addr = 0x104, cmd_data = 0x00000001
host_arm_flag = 1
host_start_flag = 1
pcie_dma_enable = 1
```

## 2026-07-19：当前禁止做的事

不要执行没有 `dma_len_bytes` 限制的随机测试。

不要直接跳到全帧。

不要把 `addr_byteswap=0` 当默认值测试当前 `PCIE_DMA_safe_test`，除非 FPGA 端明确改掉了 `cmd_data` 字节反转逻辑。

## 2026-07-19：一句话总结

当前正确方向是：

```text
以 PCIE_DMA_safe_test_4 为 FPGA 唯一依据，Linux 按 ARM/LEN/ADDR/START 新握手写寄存器，并且 LEN/START/ADDR 都按 FPGA cmd_data 字节反转规则处理；测试必须 64B -> 4KB -> 4147200B 逐步放大。
```

## 2026-07-20：关于是否可能是位宽不匹配

### 1. 当前日志先说明什么

最新 64B 测试中，关键日志是：

```text
read ADDR_ECHO0 decoded=0xeec03000 expected=0xeec03000
read ADDR_ECHO1 decoded=0xeec04000 expected=0xeec04000
read ADDR_ECHO2 decoded=0xeec05000 expected=0xeec05000
read ADDR_ECHO3 decoded=0xeec06000 expected=0xeec06000
read STATUS before START: 0x00000050 busy=0 done=0 addr_error=0 arm=1 start=0 pcie_dma_enable=1 rc_cfg_ep=0
program DMA command: offset=0x104 value=0x00000001 written=0x01000000
read STATUS after wait before STOP: 0x00000050 busy=0 done=0 addr_error=0 arm=1 start=0 pcie_dma_enable=1 rc_cfg_ep=0
```

这说明：

```text
Linux -> FPGA 的 BAR 写命令路径已经能工作。
FPGA 已经正确保存并读回 4 个 DMA 地址。
BusMaster/pcie_dma_enable 已经打开。
但是 START 后 host_start_flag 仍然是 0，frame_done_flag 也是 0。
```

所以当前最直接的问题不是“图像数据格式不对”，而是：

```text
FPGA 没有确认接收到 START，或者 START 条件没有命中。
```

### 2. 会不会是位宽不匹配

有可能，但要分成两类看。

第一类是 Linux DMA buffer 位宽不匹配：

```text
概率低。
```

原因是 Linux 端只是给 FPGA 一个 32-bit DMA bus address，buffer 是按 byte 地址访问的 coherent memory。PCIe MWr 写 32-bit、64-bit、128-bit payload，本质上都落到同一段物理地址空间。只要 FPGA TLP 地址和 length 正确，Linux 不需要知道 FPGA 内部 AXIS 是 32 位、64 位还是 128 位。

第二类是 FPGA PCIe AXIS/TLP 组包位宽或 lane 放置不匹配：

```text
需要重点怀疑。
```

当前 `PCIE_DMA_safe_test_4/source/pcie_dma_ctrl.v` 里：

```text
axis_master_tdata  是 128 位
axis_master_tkeep  是 4 位
AXIS_S_TDATA       是 128 位
AXIS_S_TKEEP       没有出现在 pcie_dma_ctrl 输出接口
```

如果 Pango PCIe IP 的 AXIS slave TX 口需要 `tkeep` 或者要求 header/data 放在特定 32-bit lane，而 FPGA 组包没有严格匹配 IP 手册，那么 FPGA 发出的 MWr TLP 可能不会被 RK3568 Root Complex 正确接收。

不过按照当前 STATUS 来看，MWr 数据发送还不是第一优先级。因为如果只是 TX MWr 位宽不匹配，通常 START 后至少应该看到：

```text
start=1
rc_cfg_ep=1
busy=1
或者 done 有变化
```

但现在 START 后 STATUS 仍然是：

```text
0x00000050 = arm=1 + pcie_dma_enable=1
```

`start=0`、`done=0`，说明 FPGA 的 START 命令解析本身还没确认成功。

### 3. 当前第一优先级排查点

FPGA 端 ILA 先抓 START 这一次 host MWr，不要先抓全帧视频。

触发条件建议：

```text
cmd_reg_addr == 12'h104
```

必须确认这些信号：

```text
tlp_fmt
tlp_type
tlp_lenght
cmd_reg_addr
cmd_data
host_arm_flag
dma_addr_valid
dma_len_valid
host_start_flag
addr_error_flag
pcie_dma_enable
axis_master_tdata_d0
axis_master_tkeep
axis_master_tvalid_d0
axis_master_tvalid_d1
```

START 写入时理论预期：

```text
{tlp_fmt, tlp_type} = 8'h40
 tlp_lenght = 1
 cmd_reg_addr = 12'h104
 cmd_data = 32'h00000001
 host_arm_flag = 1
 dma_addr_valid = 1
 dma_len_valid = 1
 host_start_flag 变成 1
 addr_error_flag = 0
```

### 4. 如果 START 没有命中，优先查这些

如果 ILA 看不到 `cmd_reg_addr == 12'h104`：

```text
说明 Linux 的 0x104 MWr 没被 FPGA 当前解析逻辑识别，重点查 TLP header 解析位段和 address lane。
```

如果看到 `cmd_reg_addr == 12'h104`，但 `cmd_data != 32'h00000001`：

```text
说明 START 数据所在 lane 或字节序不对。重点查 128-bit AXIS 下 payload 是不是固定在 axis_master_tdata_d0[31:0]。
```

如果看到 `cmd_data == 32'h00000001`，但 `host_start_flag` 不变 1：

```text
重点查 dma_addr_valid 和 dma_len_valid。
```

如果 `host_start_flag` 曾经变 1，但很快又掉回 0，同时 `done=0`：

```text
重点查 dma_stop_flag、rstn、pcie_dma_enable 是否有瞬间抖动，或者状态机是否被复位条件清掉。
```

### 5. 如果 START 已经命中，再查位宽/TLP 发送

只有确认 START 命中后，才继续查 MWr 发送位宽和 TLP 组包：

```text
mwr_state 是否进入 MWR_TLP_HEADER 和 MWR_TLP_DATA
AXIS_S_TVALID 是否拉高
AXIS_S_TREADY 是否为 1
AXIS_S_TLAST 是否在 64B 最后一个 beat 拉高
AXIS_S_TDATA header 是否是 MWr_32
AXIS_S_TDATA address 是否等于 dma_addr0
TLP_LENGTH 是否为 16DW/64B
first/last byte enable 是否符合 64B 对齐写
```

64B fixed_test_mode 期望 FPGA 写入 16 个 32-bit word：

```text
A5A50000, A5A50001, ..., A5A5000F
```

Linux hexdump 只要仍然全是 `cd`，就不能进入 4KB 或全帧测试。

## 2026-07-20：Linux 预填值应该怎么填

### 1. 结论

Linux 预填值不要填 FPGA 将要发送的固定测试数据。

当前驱动预填：

```text
0xCD
```

FPGA 64B fixed_test_mode 期望写入：

```text
A5A50000, A5A50001, ..., A5A5000F
```

这样判断最清楚：

```text
hexdump 仍然全 cd：FPGA 没有写进 host buffer，或者 START/MWr 没真正发生。
hexdump 出现 A5A50000..A5A5000F：FPGA fixed_test_mode 写入成功。
hexdump 出现部分 cd、部分 A5A5 数据：可能是 TLP/byte enable/长度/last 有问题，需要继续查 FPGA MWr 组包。
```

### 2. 为什么不继续填 0xA5

`0xA5` 和 FPGA 预期数据 `A5A50000..A5A5000F` 的高字节重复，容易让肉眼判断混乱。比如 `A5A50000` 在 hexdump 里本来就会出现 `a5 a5` 相关字节。

预填 `0xCD` 的好处是：

```text
0xCD 不属于 FPGA 固定测试序列。
DMA 没写入时一眼就是 cd cd cd cd。
DMA 写入成功时会明显变成 A5A50000..A5A5000F 对应字节。
```

### 3. 修改位置

```text
camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.c
COLORBAR_DMA_PREFILL_PATTERN = 0xCD
```

修改后必须重新编译并重新加载驱动：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make clean
make
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_cd_prefill.bin
hexdump -C /tmp/frame_64_cd_prefill.bin | head
sudo dmesg | tail -n 220
```


## 2026-07-20：关于 Linux 预填 buffer 是否会影响 FPGA 写入

### 1. 结论

Linux 预填 DMA buffer 不会阻止 FPGA 写入。

预填值只是调试用的“背景值”或“哨兵值”，用来判断 FPGA DMA 是否真的改写了这段 host buffer。它不参与 PCIe DMA 协议，也不会锁住这段内存。

当前判断逻辑是：

```text
Linux 先把 DMA buffer 填成 0xCD。
FPGA 如果真的对这个 DMA 地址发 MWr，buffer 内容会被 FPGA 数据覆盖。
如果测试后仍然全是 cd cd cd cd，说明 FPGA 没有写进来，或者 START/MWr 没真正发生。
```

### 2. 能不能不预填

技术上可以不预填，但调试时不推荐。

不预填时，buffer 里可能是：

```text
全 00
旧数据
上一次测试残留
内核分配出来时的已有内容
```

这样会导致结果很难判断。例如看到全 00 时，无法确认是 FPGA 写了 0，还是 FPGA 根本没写、buffer 原本就是 0。

预填 `0xCD` 的目的就是让“没有写入”的状态非常明显：

```text
全 cd：没被 FPGA 改写。
不是全 cd，并且出现 A5A50000..A5A5000F：FPGA 64B fixed_test_mode 写入成功。
部分 cd、部分固定数据：可能是 TLP 长度、byte enable、TLAST 或 MWr 组包问题。
```

### 3. 为什么不把预填值设成 FPGA 固定测试值

不要把 Linux 预填值设成 FPGA 将要发送的固定测试值。

FPGA 64B fixed_test_mode 期望写入：

```text
A5A50000, A5A50001, ..., A5A5000F
```

如果 Linux 也提前填类似 `0xA5`，就会让“FPGA 真写了”和“FPGA 没写，buffer 只是原本就是 A5”混在一起，判断不清。

所以当前 Linux 预填 `0xCD` 更合适。

### 4. 当前主要问题集中在哪里

根据最新日志：

```text
ADDR_ECHO0..3 decoded=expected
read STATUS before START: 0x00000050 busy=0 done=0 addr_error=0 arm=1 start=0 pcie_dma_enable=1 rc_cfg_ep=0
program DMA command: offset=0x104 value=0x00000001 written=0x01000000
read STATUS after wait before STOP: 0x00000050 busy=0 done=0 addr_error=0 arm=1 start=0 pcie_dma_enable=1 rc_cfg_ep=0
```

这说明：

```text
ARM 已经生效。
pcie_dma_enable 已经生效。
4 个 DMA 地址已经正确写入并读回。
但 START 后 host_start_flag 仍然是 0。
frame_done_flag 也仍然是 0。
```

因此当前主要问题不是 Linux buffer 预填值，而是：

```text
FPGA 没有确认接收到 START，或者 START 条件没有命中。
```

### 5. 当前最该查的 FPGA 信号

FPGA 端 ILA 应优先抓 `0x104 START` 这次 host MWr。

触发条件建议：

```text
cmd_reg_addr == 12'h104
```

重点看：

```text
cmd_reg_addr
cmd_data
tlp_fmt
tlp_type
tlp_lenght
host_arm_flag
dma_addr_valid
dma_len_valid
host_start_flag
addr_error_flag
pcie_dma_enable
```

START 写入时预期：

```text
cmd_reg_addr = 12'h104
cmd_data = 32'h00000001
tlp_lenght = 1
host_arm_flag = 1
dma_addr_valid = 1
dma_len_valid = 1
host_start_flag 变成 1
addr_error_flag = 0
```

如果 `cmd_reg_addr == 12'h104` 没出现，说明 START 这次 BAR 写没有被 FPGA 解码到。

如果 `cmd_reg_addr == 12'h104` 出现但 `cmd_data != 32'h00000001`，说明 START 数据 lane 或字节序可能不对。

如果 `cmd_data == 32'h00000001` 但 `host_start_flag` 不变 1，重点查 `dma_addr_valid` 和 `dma_len_valid`。

## 2026-07-20：PCIE_DMA_safe_test_5 对 Linux 侧的影响

### 1. FPGA test_5 改动摘要

当前 FPGA 参考工程更新为：

```text
PCIE_DMA_safe_test_5
```

FPGA 侧关键改动：

```text
START 不再只判断 cmd_data[0]
START 改为判断 cmd_data != 32'd0
STATUS bit2 新增 start_cmd_seen_flag
```

含义是：

```text
Linux 写 0x00000001 可以启动。
Linux 因字节序写成 0x01000000，FPGA 也会认为是 START 请求。
但前提仍然是 ARM 正确、ADDR 正确、LEN 正确。
```

所以这不是无条件放开 DMA，只是把 START 字节序差异兼容掉。

### 2. Linux 侧需要改什么

Linux 驱动不需要改 START 写法。

仍然保持：

```text
LEN/START 通过 colorbar_write_cmd32() 写入。
ADDR 默认 addr_byteswap=1。
不要加 addr_byteswap=0。
```

Linux 侧只需要识别新的 STATUS bit2：

```text
bit0 busy
bit1 frame_done
bit2 start_cmd_seen
bit3 addr_error
bit4 host_arm
bit5 host_start
bit6 pcie_dma_enable
bit7 rc_cfg_ep
```

当前 Linux 驱动已增加 `start_cmd_seen` 打印。

### 3. test_5 下 STATUS 怎么判断

烧录 test_5 后，重新做 64B 测试。

如果还是：

```text
0x50
```

含义是：

```text
arm=1
pcie_dma_enable=1
start_cmd_seen=0
host_start=0
```

说明 START 写命令没有被这份 RTL 看见。重点怀疑：烧错 bitstream、顶层没有用这版 `pcie_dma_ctrl.v`、BAR 写没有到达当前逻辑。

如果变成类似：

```text
0x54
```

含义是：

```text
start_cmd_seen=1
arm=1
pcie_dma_enable=1
host_start=0
```

说明 FPGA 已经看见 START 写入，但 START 条件没有完全通过。重点查：

```text
dma_addr_valid
dma_len_valid
addr_error_flag
```

如果变成类似：

```text
0x74
```

含义是：

```text
start_cmd_seen=1
host_start=1
arm=1
pcie_dma_enable=1
```

说明 START 已经进到 FPGA，接下来要查 MWr 发送状态机：

```text
mwr_state
AXIS_S_TVALID
AXIS_S_TREADY
AXIS_S_TLAST
AXIS_S_TDATA
```

如果完成后看到：

```text
0x42
```

含义是：

```text
pcie_dma_enable=1
frame_done=1
```

这是比较理想的完成状态。随后再看 `/tmp/frame_64.bin` 前 64 字节是否从 `cd cd cd...` 变成 FPGA fixed_test_mode 的固定数据。

### 4. test_5 推荐测试命令

确认 FPGA 已重新综合并烧录 `PCIE_DMA_safe_test_5` 后，Linux 端执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo make
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_test5.bin
hexdump -C /tmp/frame_64_test5.bin | head
sudo dmesg | tail -n 120
```

注意：

```text
不要加 addr_byteswap=0。
不要直接测 4KB。
不要直接测全帧。
```

只有 64B 下同时满足：

```text
start_cmd_seen=1
addr_error=0
frame_done=1 或者 host_start 曾经为 1
hexdump 不再全是 cd
```

才进入 4KB 测试。
