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

## 2026-07-20：test_5 已确认 START 被 FPGA 看见，但 START 条件失败

### 1. 本次日志

本次 64B 测试关键日志：

```text
read ADDR_ECHO0 decoded=0xeec03000 expected=0xeec03000
read ADDR_ECHO1 decoded=0xeec04000 expected=0xeec04000
read ADDR_ECHO2 decoded=0xeec05000 expected=0xeec05000
read ADDR_ECHO3 decoded=0xeec06000 expected=0xeec06000
read STATUS before START: 0x00000050 busy=0 done=0 start_cmd_seen=0 addr_error=0 arm=1 start=0 pcie_dma_enable=1 rc_cfg_ep=0
program DMA command: offset=0x104 value=0x00000001 written=0x01000000
read STATUS after wait before STOP: 0x0000005c busy=0 done=0 start_cmd_seen=1 addr_error=1 arm=1 start=0 pcie_dma_enable=1 rc_cfg_ep=0
```

### 2. 结论

这次已经可以排除“FPGA 完全没看到 START”。

因为：

```text
start_cmd_seen=1
```

说明 FPGA 已经看到过 `0x104 START` 写入。

但是：

```text
addr_error=1
host_start/start=0
frame_done/done=0
```

说明 FPGA 看见 START 后，没有进入 DMA 发送状态，而是判定 START 条件失败。

对应 FPGA 逻辑是：

```verilog
if(cmd_start_request) begin
    start_cmd_seen_flag <= 1'b1;
    if(dma_addr_valid && dma_len_valid) begin
        host_start_flag <= 1'b1;
        frame_done_flag <= 1'b0;
        addr_error_flag <= 1'b0;
    end
    else begin
        host_start_flag <= 1'b0;
        addr_error_flag <= 1'b1;
    end
end
```

所以当前问题集中在：

```text
dma_addr_valid && dma_len_valid 不成立
```

### 3. Linux 侧目前已经确认的部分

Linux 侧已经看到：

```text
ADDR_ECHO0..3 全部 decoded=expected
pcie_dma_enable=1
ARM=1
START 已被 FPGA 看见
```

这说明：

```text
BAR1 控制路径是通的。
4 个 DMA 地址至少在 ADDR_ECHO 读回路径上是正确的。
START 写命令已经进入 FPGA 当前 RTL。
```

因此当前不是“START 写不到 FPGA”的问题，也不是 Linux 默认安全闸门拦住的问题。

### 4. 当前最可疑点

当前最可疑的是 FPGA 内部在 START 判断那一拍看到：

```text
dma_len_valid = 0
```

或者：

```text
dma_addr_valid = 0
```

其中第一怀疑是 LEN 字节序或 LEN 保存值。

Linux 日志显示：

```text
program DMA command: offset=0x108 value=0x00000040 written=0x40000000
```

Linux 想表达的 LEN 是 64 字节：

```text
0x00000040
```

如果 FPGA 内部 `cmd_data` 字节反转后正确，应保存为：

```text
dma_len_bytes = 32'h00000040
```

此时：

```text
dma_len_valid = 1
```

如果 FPGA 内部实际保存为：

```text
dma_len_bytes = 32'h40000000
```

则：

```text
dma_len_valid = 0
```

START 后就会出现当前看到的：

```text
start_cmd_seen=1
addr_error=1
host_start=0
```

### 5. 下一步 FPGA ILA 必抓信号

让 FPGA 端 ILA 触发 `cmd_reg_addr == 12'h104`，也就是 START 写入那一拍。

必须抓：

```text
cmd_reg_addr
cmd_data
cmd_start_request
dma_len_bytes
dma_len_valid
dma_addr0
dma_addr1
dma_addr2
dma_addr3
dma_addr_valid
host_arm_flag
start_cmd_seen_flag
host_start_flag
addr_error_flag
pcie_dma_enable
```

START 写入时理论期望：

```text
cmd_reg_addr = 12'h104
cmd_data != 0
cmd_start_request = 1
dma_len_bytes = 32'h00000040
dma_len_valid = 1
dma_addr0 = 32'heec03000
dma_addr1 = 32'heec04000
dma_addr2 = 32'heec05000
dma_addr3 = 32'heec06000
dma_addr_valid = 1
host_arm_flag = 1
addr_error_flag = 0
host_start_flag = 1
```

### 6. ILA 结果怎么判断

如果 ILA 看到：

```text
dma_len_bytes = 32'h40000000
```

结论：

```text
LEN 字节序不对，FPGA 内部没有把 LEN 保存成 64。
```

如果 ILA 看到：

```text
dma_addr0 = 32'h0030c0ee
```

结论：

```text
地址字节序不对，FPGA 内部保存的地址是反的。
```

如果 ILA 看到：

```text
dma_len_bytes = 32'h00000040
dma_addr0..3 也都是 eec0xxxx
dma_len_valid = 1
dma_addr_valid = 1
但 host_start_flag 仍然不变 1
```

结论：

```text
START 条件判断或状态机时序本身有问题，需要 FPGA 端查 always 块优先级、复位条件、dma_stop_flag、pcie_dma_enable 是否有瞬间变化。
```

### 7. Linux 端下一步

Linux 端暂时不需要改驱动，不要改 `addr_byteswap=0`。

继续使用当前 64B 命令：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_test5.bin
hexdump -C /tmp/frame_64_test5.bin | head
sudo dmesg | tail -n 120
```

在 FPGA ILA 没确认 `dma_len_valid=1` 和 `dma_addr_valid=1` 前，不进入 4KB 或全帧测试。

## 2026-07-20：PCIE_DMA_safe_test_6 对 Linux 侧的影响

### 1. FPGA test_6 改动摘要

当前 FPGA 参考工程更新为：

```text
PCIE_DMA_safe_test_6
```

FPGA 侧关键改动：

```text
64B fixed_test_mode 下，START 只要求 dma_addr0 合法。
整帧模式下，START 才要求 dma_addr0..3 全部合法。
STATUS 新增 bit8、bit9、bit10 诊断位。
```

对应 FPGA 逻辑：

```text
dma_addr0_valid = dma_addr0 != 0 且 4 字节对齐
dma_addr_valid = dma_addr0..3 全部非 0 且 4 字节对齐
dma_start_addr_valid = fixed_test_mode ? dma_addr0_valid : dma_addr_valid
```

这版改动是合理的：64B 固定小包只写 buffer0，不应该被四缓冲整帧规则卡住。

### 2. Linux 侧需要改什么

Linux 侧不需要改 START/LEN/ADDR 写法。

仍然保持：

```text
START 写 value=0x00000001，实际 written=0x01000000。
LEN=64 写 value=0x00000040，实际 written=0x40000000。
ADDR 默认 addr_byteswap=1。
不要加 addr_byteswap=0。
```

Linux 侧只需要识别新的 STATUS 高位，当前驱动已增加打印：

```text
bit8  start_addr_valid
bit9  len_valid
bit10 fixed_test
```

完整 STATUS 位定义变为：

```text
bit0  busy
bit1  frame_done
bit2  start_cmd_seen
bit3  addr_error
bit4  host_arm
bit5  host_start
bit6  pcie_dma_enable
bit7  rc_cfg_ep
bit8  start_addr_valid
bit9  len_valid
bit10 fixed_test
```

### 3. test_6 下 STATUS 怎么判断

如果烧录 test_6 后仍然看到：

```text
0x50
0x5c
```

并且日志里：

```text
start_addr_valid=0 len_valid=0 fixed_test=0
```

说明当前运行的 FPGA 很可能还不是 test_6，或者 STATUS 高位没有接到当前 BAR 读回路径。

如果看到类似：

```text
0x774
```

拆开是：

```text
fixed_test=1
len_valid=1
start_addr_valid=1
pcie_dma_enable=1
host_start=1
arm=1
start_cmd_seen=1
```

说明 64B fixed_test_mode 的 START 合法性门槛已经通过，接下来重点看 MWr 状态机和数据是否写进 Linux buffer。

如果完成后看到类似：

```text
0x742
```

拆开是：

```text
fixed_test=1
len_valid=1
start_addr_valid=1
pcie_dma_enable=1
frame_done=1
```

这是 64B DMA 完成的理想状态之一。随后必须看 hexdump 是否不再全是 `cd`。

### 4. test_6 推荐测试命令

确认 FPGA 已重新综合并烧录 `PCIE_DMA_safe_test_6` 后，Linux 端执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo make
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64_test6.bin
hexdump -C /tmp/frame_64_test6.bin | head
sudo dmesg | tail -n 140
```

预期日志里应该能看到新增字段：

```text
start_addr_valid=1 len_valid=1 fixed_test=1
```

64B 数据预期不是：

```text
cd cd cd cd ...
```

而应该出现 FPGA fixed_test_mode 的固定数据：

```text
A5A50000, A5A50001, ..., A5A5000F
```

### 5. 仍然不能做的事

在 64B 没通过前，不要测：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4096
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4147200
```

只有同时满足下面条件，才进入 4KB：

```text
start_cmd_seen=1
start_addr_valid=1
len_valid=1
fixed_test=1
addr_error=0
frame_done=1 或者 host_start 曾经为 1
hexdump 不再全是 cd
```

## 2026-07-20：PCIE_DMA_safe_test_8 强制整帧后的 Linux 侧修改

### 1. FPGA test_8 改动摘要

当前 FPGA 参考工程更新为：

```text
PCIE_DMA_safe_test_8
```

FPGA 侧现在不再使用 Linux 写入的 `0x108 LEN` 控制传输长度，而是强制：

```text
DMA_FORCED_BYTES = DMA_DEFAULT_BYTES = 4147264
requested_tlps = (4147264 + 63) >> 6
full_frame_mode = 1
fixed_test_mode = 0
```

含义：

```text
即使 Linux 驱动写 dma_len_bytes=64，FPGA 仍会按 4147264 字节发送。
```

所以 test_8 之后，64B 小包测试不能再用于真实 DMA。

### 2. Linux 侧为什么必须改

旧 Linux 驱动默认 `dma_len_bytes=64`，会只分配：

```text
buffer size = 4096
```

但 FPGA test_8 会写：

```text
4147264 bytes
```

如果用 4096 字节 buffer 承接 4147264 字节 DMA，会越界写主机内存，有系统崩溃和文件系统损坏风险。

### 3. Linux 侧已经修改的内容

Linux 侧已把最大 DMA 长度定义为：

```text
COLORBAR_DMA_MAX_BYTES = COLORBAR_FRAME_SIZE + COLORBAR_MARK_SIZE
                       = 4147200 + 64
                       = 4147264
```

驱动现在允许：

```text
dma_len_bytes <= 4147264
```

用户态工具现在也允许保存：

```text
valid_size <= 4147264
```

加载脚本默认长度已改为：

```text
dma_len_bytes=4147264
```

注意：默认仍然是：

```text
allow_dma_start=0
```

所以默认加载驱动不会启动 DMA。

### 4. test_8 下绝对不要执行的命令

不要再执行：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
sudo ./build/pcie_color_rx --once --output /tmp/frame_64.bin
```

也不要执行：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4096
```

原因：FPGA 不再按 64B 或 4KB 发送，而是强制 4147264 字节。

### 5. test_8 安全测试命令

先重新编译：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo make
```

先做一次“只分配 buffer、不启动 DMA”的安全检查。注意：buffer 是用户态 `--once` 调用 `COLORBAR_IOC_ALLOC_BUFS` 时才分配的，不是 `load_driver.sh` 后立刻分配。

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start_test8.bin
sudo dmesg | tail -n 120
```

这一步预期 `--once` 被安全闸门拒绝：

```text
COLORBAR_IOC_START: Operation not permitted
```

同时 dmesg 必须看到类似：

```text
buffer0 ... size=4149248 requested_len=4147264
buffer1 ... size=4149248 requested_len=4147264
buffer2 ... size=4149248 requested_len=4147264
buffer3 ... size=4149248 requested_len=4147264
```

只要看到：

```text
size=4096
```

立刻停止，不要执行真实 DMA。

确认 buffer 足够后，再重新加载并允许 DMA：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/frame_full_test8.rgb565
hexdump -C /tmp/frame_full_test8.rgb565 | head
sudo dmesg | tail -n 160
```

### 6. test_8 结果怎么判断

输出文件大小应该是：

```text
4147264 bytes
```

其中前面：

```text
4147200 bytes
```

是 RGB565 1920x1080 图像数据。

最后：

```text
64 bytes
```

是 FPGA 额外标记或尾部测试区。

如果 hexdump 仍然大面积是：

```text
cd cd cd cd ...
```

说明 FPGA DMA 数据没有真正写入 host buffer。

如果系统稳定、文件大小正确、数据不是全 cd，再继续做 RGB565 彩条采样校验。

## 2026-07-20：test_8 申请整帧 DMA buffer 失败后的修正

### 1. 本次现象

执行安全检查：

```sh
./scripts/load_driver.sh dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start_test8.bin
```

返回：

```text
COLORBAR_IOC_ALLOC_BUFS: Cannot allocate memory
```

这一步没有启动 DMA，因为失败发生在 `COLORBAR_IOC_ALLOC_BUFS`，还没有走到 `COLORBAR_IOC_START`。

### 2. 原因

旧 Linux 驱动仍按 4 个 DMA buffer 分配：

```text
4 * PAGE_ALIGN(4147264) = 4 * 4149248 = 16596992 bytes
```

RK3568 当前内核环境下，一次申请约 16.6MB coherent DMA 内存失败。

FPGA test_8 当前强制整帧发送，主要使用 buffer0 地址承接整帧。为了降低内存压力，Linux 侧改为：

```text
COLORBAR_BUFFER_COUNT = 1
```

也就是只申请 1 个约 4.15MB 的 coherent buffer。

### 3. 兼容 FPGA 四地址寄存器的做法

虽然 Linux 只分配 1 个 buffer，但写 FPGA 地址寄存器时仍连续写 4 次：

```text
ADDR0 = buffer0
ADDR1 = buffer0
ADDR2 = buffer0
ADDR3 = buffer0
```

这样做的目的：

```text
如果 FPGA test_8 只使用 buffer0，则正好匹配。
如果 FPGA 仍然检查 4 个地址非 0，也不会因为 ADDR1..3 为 0 而失败。
```

ADDR_ECHO 读回也按 4 个寄存器检查，但期望值都等于 buffer0 地址。

### 4. 新的安全检查命令

重新编译：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo make
```

先加载但不允许 DMA：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start_test8.bin
sudo dmesg | tail -n 120
```

预期：

```text
COLORBAR_IOC_START: Operation not permitted
```

并且 dmesg 应看到：

```text
buffer0 ... size=4149248 requested_len=4147264
```

现在只应看到 `buffer0`，不再要求看到 buffer1..3。

如果还是：

```text
COLORBAR_IOC_ALLOC_BUFS: Cannot allocate memory
```

说明连单个约 4.15MB coherent buffer 都申请失败，需要考虑预留 CMA、降低系统内存压力或改用不同 DMA 分配策略。

### 5. 真实 test_8 DMA 命令

只有安全检查确认 buffer0 分配成功后，才允许真实 DMA：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/frame_full_test8.rgb565
hexdump -C /tmp/frame_full_test8.rgb565 | head
sudo dmesg | tail -n 160
```

仍然禁止使用：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=64
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4096
```

## 2026-07-20：单 buffer 仍 Cannot allocate memory 的进一步修正

### 1. 本次现象

即使 Linux 已经改成：

```text
COLORBAR_BUFFER_COUNT = 1
```

安全检查仍返回：

```text
COLORBAR_IOC_ALLOC_BUFS: Cannot allocate memory
```

说明不是 4 个 buffer 总量太大的问题，而是连单个约 4.15MB coherent DMA buffer 都没有申请成功。

### 2. 新发现的 Linux 内核原因

当前 RK3568 内核头文件中：

```text
pci_alloc_consistent()
```

实际使用的是：

```text
dma_alloc_coherent(..., GFP_ATOMIC)
```

`GFP_ATOMIC` 更适合不能睡眠的原子上下文，不适合在 ioctl 中申请 4MB 级别的大块 DMA buffer。当前分配发生在用户态 ioctl 调用路径，可以睡眠，所以更合适的是：

```text
dma_alloc_coherent(..., GFP_KERNEL)
```

### 3. Linux 驱动已修改

已把 DMA buffer 分配从：

```text
pci_alloc_consistent()
```

改为：

```text
dma_alloc_coherent(&pdev->dev, size, &dma_addr, GFP_KERNEL)
```

释放对应改为：

```text
dma_free_coherent()
```

同时分配失败时会打印更明确的日志：

```text
failed to allocate coherent DMA buffer0 size=4149248 requested_len=4147264; check CMA/free contiguous DMA memory
```

### 4. 重新测试命令

重新编译：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo make
```

重新做安全检查，不允许 START：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start_test8.bin
sudo dmesg | tail -n 140
```

预期：

```text
buffer0 ... size=4149248 requested_len=4147264
COLORBAR_IOC_START: Operation not permitted
```

如果仍然 `Cannot allocate memory`，下一步不应该继续改 DMA 逻辑，而应检查系统 CMA/连续 DMA 内存：

```sh
dmesg | grep -i cma
cat /proc/meminfo | grep -i cma
free -h
```

如果 `CmaFree` 很小或没有 CMA，需要考虑增加内核启动参数或设备树中的 CMA 预留，例如预留 32MB/64MB 以上。具体参数要按鲁班猫当前启动方式调整。

## 2026-07-20：PCIE_DMA_safe_test_9 单大 buffer 整帧方案

### 1. FPGA test_9 改动摘要

当前 FPGA 参考工程更新为：

```text
PCIE_DMA_safe_test_9
```

FPGA 侧已按“鲁班猫只拿一个大 buffer”修改：

```text
dma_start_addr_valid = dma_addr0_valid
alloc_addrl <= dma_addr0
```

含义：

```text
整帧 DMA 启动合法性只要求 dma_addr0 非 0 且 4 字节对齐。
整帧发送首地址固定使用 dma_addr0。
不再要求 dma_addr1..3 都是独立大 buffer。
不会再按 addr_page 在 dma_addr0..3 间轮换。
```

### 2. Linux 侧是否需要再改代码

当前 Linux 侧已经匹配 test_9，不需要再改代码。

当前 Linux 行为：

```text
COLORBAR_BUFFER_COUNT = 1
只申请 1 个 coherent DMA 大 buffer。
buffer0 size 应为 4149248，requested_len 为 4147264。
写 FPGA 地址寄存器时仍写 4 次，但 4 次都是 buffer0 地址。
```

这和 test_9 的要求一致：

```text
FPGA 只看 dma_addr0。
Linux 保证 buffer0 足够大。
```

### 3. test_9 预期 STATUS

烧录 test_9 后，START 前理想状态类似：

```text
0x00000350
```

含义：

```text
len_valid=1
start_addr_valid=1
arm=1
pcie_dma_enable=1
fixed_test=0
```

START 后可能看到类似：

```text
0x00000374
0x000003f5
0x00000342
```

重点不是死盯某一个 raw 值，而是看字段：

```text
start_addr_valid=1
len_valid=1
addr_error=0
start_cmd_seen=1
host_start=1 或 done=1
```

如果仍然看到：

```text
start_addr_valid=0
```

说明：

```text
FPGA 当前运行的可能还不是 test_9。
或者 dma_addr0 没真正写进当前 RTL。
或者 ADDR_ECHO0 读回和 START 判断使用的不是同一份 dma_addr0。
```

### 4. test_9 安全测试命令

先确认安全闸门和 buffer0 分配：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start_test9.bin
sudo dmesg | tail -n 140
```

预期：

```text
COLORBAR_IOC_START: Operation not permitted
buffer0 ... size=4149248 requested_len=4147264
```

确认 buffer0 成功后，才允许真实 DMA：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4147264
sudo ./build/pcie_color_rx --once --output /tmp/frame_full_test9.rgb565
hexdump -C /tmp/frame_full_test9.rgb565 | head
ls -lh /tmp/frame_full_test9.rgb565
sudo dmesg | tail -n 180
```

输出文件大小应为：

```text
4147264 bytes
```

如果文件仍然全是：

```text
cd cd cd cd ...
```

说明 FPGA 仍未把 MWr 数据写进 Linux buffer。

如果文件不是全 cd，但彩条校验失败，则继续分析图像格式、字节序、行宽、帧尾 64B 标记区和 FPGA 发送内容。


## 2026-7-21 PCIE_DMA_single_1：Linux 端适配记录

### 1. 本次 FPGA 协议变化

`PCIE_DMA_single_1` 已经不是 test_8/test_9 的协议。Linux 端从这一节开始不要再使用 `4147264` 和 4 地址轮转思路。

新的 FPGA 协议要点：

```text
DMA buffer 数量：1
DMA buffer 分配大小：4 MiB = 4194304 bytes
有效图像长度：4147200 bytes = 1920 * 1080 * 2
保护区长度：47104 bytes = 4194304 - 4147200
每个 MWr TLP：64 bytes
每帧 TLP 数：64800
帧尾额外 64B marker：已取消
```

寄存器协议：

```text
0x100 ARM   = 0xA55A5AA5
0x104 START = 非零启动一帧
0x108 LEN   = 必须写 4147200
0x110 ADDR  = 一个 32-bit DMA 地址
0x114 ADDR_ECHO0
0x130 STOP
0x170 STATUS
0x174 ACTIVE_ADDR
0x178 ACTIVE_LEN
0x17c BYTES_SENT
```

### 2. Linux 端已修改内容

本次已修改：

```text
camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.h
camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.c
camara_host_computer/Colorbar_image/include/colorbar_pcie_rx.h
camara_host_computer/Colorbar_image/src/pcie_color_rx.c
camara_host_computer/Colorbar_image/scripts/load_driver.sh
```

核心修改：

```text
1. dma_len_bytes 默认改为 4147200，并且只允许等于 4147200。
2. DMA buffer 固定申请 4MiB，不再按 4147264 或 PAGE_ALIGN(4147264) 申请。
3. 只使用 buffer0，一个 coherent DMA buffer。
4. 只写 0x110 一个 DMA 地址。
5. 只读 0x114 ADDR_ECHO0 做地址回读确认。
6. STATUS 改为解析 PCIE_DMA_single_1 的新格式，版本号必须是 0x0A。
7. START 前必须检查：IDLE=1、ARM=1、ADDR_VALID=1、LEN_VALID=1、BUSY=0、无错误。
8. START 前才打开 Bus Master，并等待 STATUS bit8 bus_master_seen=1。
9. START 后不再固定睡眠当作成功，而是轮询 DONE+IDLE。
10. DONE 后读取 0x174/0x178/0x17c，确认实际地址、实际长度、实际发送字节数。
11. 检查 buffer 尾部 47104 bytes guard 区是否仍为 0xA5。
12. 用户态输出文件只保存 4147200 bytes 有效 RGB565 图像。
13. STOP 只写 0x130，不再向 0x110 写 0，避免新 FPGA 把清零地址判成 addr_error。
```

### 3. 新 STATUS 判断方式

新 STATUS 不是旧 test_9 的字段。现在驱动日志会打印：

```text
version=0x0a
rc_cfg_ep
host_start
stop_pending
fifo_underflow
fifo_overflow
len_error
addr_error
bus_master_seen
len_valid
addr_valid
start_seen
arm
stop_ack
idle
done
busy
```

成功 START 前应该看到：

```text
version=0x0a
arm=1
addr_valid=1
len_valid=1
idle=1
busy=0
addr_error=0
len_error=0
fifo_overflow=0
fifo_underflow=0
```

打开 Bus Master 后应该看到：

```text
bus_master_seen=1
```

一帧成功完成后应该看到：

```text
done=1
idle=1
busy=0
```

并且驱动日志应该有：

```text
DMA result: active_addr=... expected_addr=... active_len=4147200 bytes_sent=4147200
DMA guard intact: [4147200, 4194304) size=47104 pattern=0xa5
```

### 4. 安全测试命令

先只验证默认安全闸门，不允许 START：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start_single1.bin
sudo dmesg | tail -n 160
```

预期现象：

```text
COLORBAR_IOC_START: Operation not permitted
buffer0 ... size=4194304 frame_len=4147200 guard=47104
```

这一步如果不是 `Operation not permitted`，不要继续真实 DMA。

确认安全闸门有效后，再允许真实 DMA：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1
sudo ./build/pcie_color_rx --once --output /tmp/frame_single1.rgb565
ls -lh /tmp/frame_single1.rgb565
hexdump -C /tmp/frame_single1.rgb565 | head
sudo dmesg | tail -n 220
```

预期输出文件大小：

```text
4147200 bytes
```

预期驱动日志重点：

```text
read STATUS ... version=0x0a
read ADDR_ECHO0 decoded=... expected=...
read STATUS before BusMaster/START ... addr_valid=1 len_valid=1 arm=1 idle=1
read STATUS for BusMaster visible ... bus_master_seen=1
read STATUS for frame DONE+IDLE ... done=1 idle=1 busy=0
DMA result: active_addr=... expected_addr=... active_len=4147200 bytes_sent=4147200
DMA guard intact: [4147200, 4194304) size=47104 pattern=0xa5
captured frame_counter=1 buffer=0 bytes=4147200
```

### 5. 异常结果怎么判断

如果看到：

```text
STATUS version mismatch
```

优先怀疑：FPGA 还没有烧 `PCIE_DMA_single_1`，或者 STATUS 回读字节序/BAR 不是当前这版。

如果看到：

```text
ADDR_ECHO0 mismatch
```

说明 Linux 写到 `0x110` 的地址和 FPGA `0x114` 保存的地址不一致。先不要 START。重点检查 `addr_byteswap` 和 FPGA `cmd_data` 字节序。

如果看到：

```text
len_error=1
len_valid=0
```

说明 `0x108 LEN` 没有被 FPGA 识别为 `4147200`。检查加载参数是否仍写成旧的 `4147264`，或者是否仍加载了旧驱动。

如果看到：

```text
bus_master_seen=0
```

说明 Linux `pci_set_master()` 后 FPGA 没看到 `cfg_bus_master_en`。这时 START 不应该继续。

如果看到：

```text
timeout waiting for frame DONE+IDLE
```

说明 FPGA 已经进入启动流程，但没有完成一帧。重点看 FPGA 侧视频时序、VS 边沿、FIFO 水位、AXIS_S_TREADY、MWr TLP 是否真正发出。

如果看到：

```text
DMA guard overwritten
```

说明 FPGA 写超过了 4147200 有效图像区域，这是严重越界风险，必须停止测试并回到 FPGA 侧查 TLP 计数和最后一包逻辑。

### 6. 当前建议

当前 Linux 端已经按 `PCIE_DMA_single_1` 改成单 buffer 安全握手。下一次测试必须先跑默认安全闸门，再跑 `allow_dma_start=1`。不要再跑旧命令：

```sh
./scripts/load_driver.sh allow_dma_start=1 dma_len_bytes=4147264
```

也不要再跑 64B/4KB 小包测试，因为 `PCIE_DMA_single_1` 的 FPGA 已经固定要求整帧 `4147200`。


## 2026-7-22 PCIE_DMA_single_1：寄存器字节序修正

### 1. 本次失败日志结论

这次真实 DMA 命令返回：

```text
COLORBAR_IOC_START: Input/output error
```

驱动日志关键行：

```text
program DMA address: dma=0x00000000eef00000 low32=0xeef00000 written=0x0000f0ee addr_byteswap=1
program DMA command: offset=0x108 value=0x003f4800 written=0x00483f00
read STATUS before BusMaster/START: raw=0x0a000614 ... len_error=1 addr_error=1 len_valid=0 addr_valid=0 arm=1 idle=1 busy=0
refuse to start FPGA DMA
```

判断：

```text
FPGA STATUS version=0x0A，说明读到的是 PCIE_DMA_single_1 新状态寄存器。
Linux 已经申请到 4MiB buffer。
Linux 写了 ARM、ADDR、LEN。
驱动在 START 前看到 addr_error=1、len_error=1，因此拒绝 START。
没有真正启动 DMA，没有写 host 内存。
```

失败原因集中在 BAR 控制寄存器字节序：Linux 软件提前 `swab32()`，FPGA 数据通路内部又做了一次字节翻转，导致 FPGA 内部用于合法性判断的值变成：

```text
ADDR = 0x0000f0ee  不是 64B 对齐，所以 addr_error=1
LEN  = 0x00483f00  不是 4147200，所以 len_error=1
```

`ADDR_ECHO0` 曾经看起来匹配，是因为回读路径和驱动解码再次发生字节排列变化；它不能单独证明 FPGA 内部合法性判断用的是正确地址。当前最可信的是 STATUS 里的：

```text
addr_valid
len_valid
addr_error
len_error
```

### 2. Linux 端已修改内容

本次驱动新增 3 个字节序参数：

```text
cmd_byteswap       控制 ARM/LEN/START 这类命令值写 BAR 前是否 swab32
addr_byteswap      控制 DMA 地址写 0x110 前是否 swab32
readback_byteswap  控制 0x114/0x174/0x178/0x17c 回读后是否 swab32
```

`PCIE_DMA_single_1` 当前默认值改为：

```text
cmd_byteswap=0
addr_byteswap=0
readback_byteswap=0
```

也就是 Linux 直接写原值：

```text
ARM   0xa55a5aa5
ADDR  0xeef00000  示例，实际以驱动分配地址为准
LEN   0x003f4800
START 0x00000001
```

加载脚本 `scripts/load_driver.sh` 已同步默认参数。

### 3. 当前测试命令

先验证默认安全闸门，不允许 START：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh
sudo ./build/pcie_color_rx --once --output /tmp/should_not_start_single1.bin
sudo dmesg | tail -n 180
```

预期：

```text
COLORBAR_IOC_START: Operation not permitted
loaded params: ... cmd_byteswap=0 addr_byteswap=0 readback_byteswap=0 ... dma_len_bytes=4147200
```

再允许真实 DMA：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1
sudo ./build/pcie_color_rx --once --output /tmp/frame_single1.rgb565
ls -lh /tmp/frame_single1.rgb565
hexdump -C /tmp/frame_single1.rgb565 | head
sudo dmesg | tail -n 240
```

START 前理想 STATUS 字段：

```text
version=0x0a
arm=1
addr_valid=1
len_valid=1
idle=1
busy=0
addr_error=0
len_error=0
```

如果用 raw 值粗略看，常见可能是：

```text
0x0a0000d4  未打开 Bus Master 前
0x0a0001d4  打开 Bus Master 后
```

但驱动不要按完整 raw 值相等判断，只按位判断。

### 4. 如果仍失败

如果仍看到：

```text
ADDR_ECHO0 mismatch
```

先不要启动 DMA。可以只在下一轮临时测试：

```sh
./scripts/load_driver.sh allow_dma_start=1 readback_byteswap=1
```

如果 `ADDR_ECHO0` 变匹配，但 STATUS 仍然 `addr_valid=1 len_valid=1`，说明只是回读显示路径需要翻转，写路径不需要翻转。

如果仍看到：

```text
addr_error=1
len_error=1
addr_valid=0
len_valid=0
```

则说明 FPGA 内部实际保存的 ADDR/LEN 仍不对，需要 FPGA 侧抓 `cmd_data`、`dma_addr0`、`dma_len_bytes` 三个信号，而不是只看回读值。

如果看到：

```text
addr_valid=1
len_valid=1
bus_master_seen=1
start_seen=1
```

但之后没有 `done=1`，问题才进入视频/FIFO/MWr 发送阶段。


## 2026-7-22 PCIE_DMA_single_1：DONE 超时后的发送进度诊断

### 1. 本次超时日志结论

字节序修正后，START 前状态已经正确：

```text
ADDR written=0xeef00000 addr_byteswap=0
LEN  written=0x003f4800 cmd_byteswap=0
STATUS before BusMaster/START: raw=0x0a0000d4 ... addr_valid=1 len_valid=1 addr_error=0 len_error=0 arm=1 idle=1 busy=0
STATUS for BusMaster visible: raw=0x0a0001d4 ... bus_master_seen=1
START written=0x00000001
```

START 后状态长期保持：

```text
raw=0x0a00c1f1
rc_cfg_ep=1
host_start=1
start_seen=1
arm=1
bus_master_seen=1
addr_valid=1
len_valid=1
busy=1
idle=0
done=0
fifo_underflow=0
fifo_overflow=0
addr_error=0
len_error=0
```

这说明 Linux 侧寄存器配置、DMA 地址、LEN、Bus Master、START 已经通过。当前问题进入 FPGA 数据发送阶段：FPGA 进入 busy，但没有在超时时间内完成整帧并置 DONE。

### 2. Linux 端已增加的诊断

驱动已修改：当等待 `DONE+IDLE` 超时时，也会读取并打印：

```text
0x174 ACTIVE_ADDR
0x178 ACTIVE_LEN
0x17c BYTES_SENT
```

日志格式类似：

```text
DMA progress after timeout: active_addr=0x... active_len=4147200 bytes_sent=... bytes_sent_hex=0x... remaining=...
```

用户态提示也改成：

```text
FPGA DMA did not report DONE before the driver timeout; check dmesg for ACTIVE_ADDR/ACTIVE_LEN/BYTES_SENT
```

### 3. 下一次测试命令

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1
sudo ./build/pcie_color_rx --once --output /tmp/frame_single1.rgb565
sudo dmesg | tail -n 260
```

### 4. BYTES_SENT 判断方法

重点看：

```text
bytes_sent
bytes_sent_hex
remaining
```

如果：

```text
bytes_sent=0
```

说明一个 MWr TLP 都没有发出。FPGA 侧优先检查：

```text
VS 边沿是否出现
DE 是否有效
FIFO 是否写入数据
rd_water_level 是否达到发送门槛
dma_start 是否被拉起
mwr_state 是否离开 IDLE
AXIS_S_TVALID 是否拉起
```

如果：

```text
bytes_sent=4147136
bytes_sent_hex=0x003f47c0
remaining=64
```

说明整帧 4147200 字节只差最后 64 字节。此时重点怀疑最后一个 TLP 的启动条件过严。

FPGA 侧建议检查并修改最后一包条件：

```text
普通包要求 rd_water_level >= TLP_LENGTH/4
最后一包允许 final_tlp && rd_water_level >= TLP_LENGTH/4 - 1
```

也就是：

```text
普通包水位 >= 4
最后一包水位 >= 3
```

如果：

```text
0 < bytes_sent < 4147136
```

说明已经发出一部分 TLP，但中途卡住。FPGA 侧重点检查：

```text
rd_water_level 是否下降后无法恢复
AXIS_S_TREADY 是否曾经拉低后没有恢复
mwr_tlast 是否正常出现
dma_cnt 是否继续递增
FIFO 读写时钟/复位是否异常
```

如果：

```text
bytes_sent=4147200
```

但仍然没有 DONE，则说明数据包数量已经够了，问题集中在 FPGA DONE/IDLE/host_start_flag 清零逻辑，而不是数据发送量本身。


## 2026-7-22 图形界面采集程序

### 1. 新增程序

新增 GTK2 图形界面程序：

```text
src/pcie_color_gui.c
build/pcie_color_gui
```

Makefile 已增加 GUI 构建目标，执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo make
```

会同时生成：

```text
build/pcie_color_rx
build/pcie_color_gui
driver/colorbar_pcie_driver.ko
```

### 2. 驱动新增状态 ioctl

为了让界面显示 DMA 状态，驱动和公共头文件新增：

```text
COLORBAR_IOC_GET_STATUS
struct colorbar_status_info
```

界面可以读取：

```text
status_raw
status
dma_addr_low
active_addr
active_len
bytes_sent
frame_counter
```

界面显示字段包括：

```text
帧号
帧率
DMA_ADDR
ACTIVE_ADDR
ACTIVE_LEN
BYTES_SENT
DONE
IDLE
BUSY
FIFO_OVERFLOW
FIFO_UNDERFLOW
运行日志和错误信息
```

### 3. 界面按钮

界面包含：

```text
连接设备
采集一帧
连续采集
停止
保存图片
RGB565 Little Endian / RGB565 Big Endian 切换
```

`保存图片` 当前保存为 PNG。

### 4. 单 buffer 采集顺序

GUI 严格按单 buffer 顺序运行：

```text
START
→ 等待 DONE+IDLE
→ 从 /dev/colorbar_pcie_rx 复制 4147200 字节
→ RGB565 转 RGB888
→ 显示图像
→ 连续采集模式下再启动下一帧
```

不会在当前图像没有复制完成时启动下一帧。

### 5. DMA 成功条件

当前驱动和 GUI 按 FPGA 建议保持严格条件：

```text
BYTES_SENT = 4147200
DONE = 1
IDLE = 1
BUSY = 0
FIFO_OVERFLOW = 0
FIFO_UNDERFLOW = 0
```

如果 START 失败、WAIT_FRAME 失败或出现错误位，连续采集会立即停止，不会无限自动重试。

### 6. 运行命令

先加载驱动，允许 START：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1
```

启动图形界面：

```sh
sudo ./build/pcie_color_gui
```

如果当前桌面用户无法用 `sudo` 打开图形窗口，可以先尝试：

```sh
xhost +local:root
sudo ./build/pcie_color_gui
```

测试流程：

```text
1. 点击“连接设备”。
2. 点击“采集一帧”。
3. 如果图像颜色不对，切换 RGB565 大小端。
4. 如果单帧稳定，再点击“连续采集”。
5. 出错后查看右侧日志和状态字段。
6. 需要保存当前帧时点击“保存图片”。
```

### 7. 当前需要重点观察

如果界面日志显示 `WAIT_FRAME failed`，同时状态里：

```text
BYTES_SENT = 4147200
DONE = 1
IDLE = 1
BUSY = 0
FIFO_UNDERFLOW = 1
```

说明图像数据量已经发满，但 FPGA 同时报告 FIFO_UNDERFLOW。按当前严格成功条件，Linux 不会把这一帧显示出来。此时 FPGA 侧需要继续确认最后一包或收尾阶段是否多读 FIFO 一拍。


## 2026-7-22 GUI 1080p 固定布局与 IDLE 含义

### 1. GUI 布局修改

图形界面已取消图像滚动条，不再需要拖动滑块查看画面。

当前界面按 1920x1080 桌面设计：

```text
窗口默认尺寸：1920x1080
图像预览区域：1440x810
右侧状态/日志区域：约 450px 宽
```

注意：FPGA/驱动采集的原始图像仍然是完整的：

```text
1920x1080 RGB565
4147200 bytes
```

界面只是把 1920x1080 图像按 16:9 比例缩放到 1440x810 显示，避免界面出现横向/纵向滚动条。保存图片时仍保存当前帧转换后的 PNG。

### 2. IDLE 一直是 1 表示什么

`IDLE=1` 表示 FPGA DMA 状态机当前处于空闲状态，也就是没有正在发送 DMA TLP。

常见情况：

```text
刚加载驱动/STOP 后：IDLE=1, BUSY=0, DONE=0
START 后正在传输：IDLE=0, BUSY=1
一帧完成后：IDLE=1, BUSY=0, DONE=1
```

所以如果没有采集，或者采集已经结束，`IDLE=1` 是正常的。

判断一次 DMA 是否成功，不能只看 `IDLE=1`，要同时看：

```text
DONE=1
IDLE=1
BUSY=0
BYTES_SENT=4147200
FIFO_OVERFLOW=0
FIFO_UNDERFLOW=0
```

如果 `IDLE=1` 但 `DONE=0`，通常只是空闲或 STOP 后状态，不代表已经采到一帧。

如果 `IDLE=1` 且 `DONE=1`，但 `FIFO_UNDERFLOW=1`，说明 FPGA 报告了 FIFO 读空错误。当前 Linux/GUI 仍按错误处理，不默认显示这一帧。
