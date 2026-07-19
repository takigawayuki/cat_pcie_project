# Linux 端彩条 PCIe 接收实施路线

本文只保留当前正确路线，历史错误推断和过期调试步骤已经删除。

当前 FPGA 正确参考工程：

```text
PCIE_DMA_safe_test
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

已核查 `PCIE_DMA_safe_test/source/pcie_dma_ctrl.v`，当前 FPGA 端采用显式安全握手。

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
PCIE_DMA_safe_test/source/pcie_dma_ctrl.v
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
PCIE_DMA_safe_test/source/ddr_test_top.v
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

`PCIE_DMA_safe_test/source/pcie_dma_ctrl.v` 对 host 写进来的 32-bit 数据做了字节反转：

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

### 4. STATUS 当前不能作为 Linux 轮询依据

虽然 FPGA 里定义了：

```text
0x170 STATUS
```

也生成了内部信号：

```verilog
dma_status <= {24'd0,
               rc_cfg_ep_flag,
               pcie_dma_enable,
               host_start_flag,
               host_arm_flag,
               addr_error_flag,
               1'b0,
               frame_done_flag,
               dma_busy_status};
```

但当前源码里没有看到完整的 PCIe MRd 读回路径，所以 Linux 暂时不能依赖 `0x170` 做轮询完成判断。

当前 Linux 驱动仍然用：

```text
frame_wait_ms 等待一段时间
然后 STOP
然后用户态 read() 拷贝 DMA buffer
```

后续如果 FPGA 端把 `0x170` 做成可读寄存器，Linux 驱动再改成轮询 `frame_done_flag`。

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
7. STOP 会清 START/ARM/LEN/ADDR。
8. read() 只允许用户态读取 dma_len_bytes 长度。
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
allow_dma_start=0
```

默认加载不会启动 DMA，并且默认只按 64B 测试长度准备 DMA buffer，避免在安全验证阶段申请全帧大块 coherent 内存。

## 2026-07-19：当前安全测试完整命令

这一节是当前唯一推荐执行顺序。前面的安全指南内容已经汇总到这里，后续只看本文档。

### 0. 测试前安全前提

必须先确认：

```text
FPGA 已烧录 PCIE_DMA_safe_test 对应 bitstream
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

只有前面 1 到 5 步都正常，并且确认 FPGA 已烧录 `PCIE_DMA_safe_test` 后，才执行 64B。

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
dmesg 里能看到 START written=0x01000000
```

64B 是第一条真实 DMA 测试，有风险，但风险被限制在最小长度。若这一步异常，不能继续测试 4KB 或全帧。

64B 如果读出来全是 `00`，不要马上判断成功。旧版本驱动会在 START 前把 DMA buffer 清 0，因此“全 00”可能有两种情况：

```text
FPGA 真的写入了 0 数据
FPGA 没有写入，Linux 读到的是驱动预清零内容
```

当前驱动已经改为 START 前用 `0xA5` 预填充 DMA buffer。重新编译并重复 64B 后，判断方式如下：

```text
如果 /tmp/frame_64.bin 仍然全是 a5，说明 FPGA 大概率没有写入 host buffer。
如果 /tmp/frame_64.bin 变成 00 或其他规律数据，说明 FPGA 至少发生了写入。
如果系统不崩溃但数据全 a5，下一步查 FPGA 的 host_start_flag、pcie_dma_enable、AXIS_S_TVALID/TREADY/TLAST。
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
./scripts/load_driver.sh                               风险低，默认 allow_dma_start=0，dma_len_bytes=64
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
FPGA ILA 看到 cmd_data 不等于预期
FPGA ILA 看到 pcie_dma_enable=0
dmesg 出现 kernel oops、SError、PCIe AER 异常
```

### 12. 当前风险边界

`0x170 STATUS` 目前在源码里是内部状态信号，没有确认可通过 PCIe MRd 读回。所以 Linux 端暂时不能用它轮询完成，只能用 `frame_wait_ms` 等待后 STOP。

后续 FPGA 如果把 STATUS 做成可读寄存器，Linux 驱动应改为轮询 `frame_done_flag`，不要长期依赖固定延时。

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
以 PCIE_DMA_safe_test 为 FPGA 唯一依据，Linux 按 ARM/LEN/ADDR/START 新握手写寄存器，并且 LEN/START/ADDR 都按 FPGA cmd_data 字节反转规则处理；测试必须 64B -> 4KB -> 4147200B 逐步放大。
```
