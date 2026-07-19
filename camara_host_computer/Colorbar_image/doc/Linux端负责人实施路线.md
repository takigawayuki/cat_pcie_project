# Linux 端彩条接收实施时间线

本文是 `camara_host_computer/Colorbar_image` 的唯一主文档。后续按这个时间线推进，不再把“实施路线”和“开发进度”分成两份文档。

当前目标：在鲁班猫2 RK3568 Linux 端，通过 PCIe 接收 FPGA 工程 `PCIE_DMA_test_color_MES50HP_X1` 发来的 1920x1080 RGB565 彩条图像。

重要边界：`pango_pcie_dma_alloc` 已经验证 PCIe 通信和基础 DMA 可用，当前不修改它；新的彩条接收工作只放在 `camara_host_computer/Colorbar_image`。


## 重要事故记录：2026-07-19 系统镜像崩溃

现象：执行到 `./build/pcie_color_rx --validate frame_wait200.rgb565` 附近后，系统镜像崩溃，桌面无法进入。

判断：`--validate` 本身只是读取本地 raw 文件并做 RGB565 采样校验，它不打开 `/dev/colorbar_pcie_rx`，也不会主动触发 PCIe DMA。真正危险的动作发生在前面的 `--once` 阶段：驱动把 4 个 DMA 地址写给 FPGA 后，FPGA 会作为 PCIe Endpoint 主动向 RK3568 物理内存发起 MWR 写事务。

高度怀疑原因：

```text
BAR 选错
addr_byteswap 猜错
FPGA 端 0x110 地址解析和 Linux 端写入方式不一致
FPGA 仍在使用旧的或错误的 dma_addr0..dma_addr3
FPGA DMA 没有停止，持续向错误物理地址写数据
```

如果 FPGA 拿到错误 DMA 地址，它不是只会写坏 `frame_0000.rgb565`，而是可能直接覆盖：

```text
Linux 内核内存
页缓存
systemd/桌面进程内存
SD 卡文件系统缓存
rootfs 关键文件
```

所以这类崩溃可能表现为：

```text
当前系统卡死
重启后进不了桌面
文件系统损坏
系统镜像需要 fsck 或重刷
```

从现在开始，未确认 BAR 和 DMA 地址字节序前，禁止直接跑真正启动 DMA 的命令。

当前恢复建议：

```text
1. 先给鲁班猫和 FPGA 都断电。
2. 恢复系统前，先拔掉 FPGA/转接板，或者确保 FPGA 不会继续发起 PCIe DMA。
3. 优先用串口控制台看启动日志；如果能进命令行，先不要进桌面测试 PCIe。
4. 如果 rootfs 是 SD 卡，建议在另一台 Linux 机器上对分区做 fsck，或者直接重刷镜像。
5. 重刷或修复后，先不插 FPGA 启动一次，确认系统桌面能正常进入。
6. 回到 PCIe 测试时，只能先跑安全模式，不加 allow_dma_start=1。
```

如果能从另一台 Linux 机器看到 SD 卡分区，常见检查命令类似：

```sh
sudo fsck.ext4 -f /dev/sdX3
```

这里的 `/dev/sdX3` 必须换成实际 rootfs 分区，不能照抄。看不准设备名就不要执行，避免修错盘。



## 这个问题到底是 RK 的问题还是 FPGA 的问题

先给结论：

```text
不是 RK3568 硬件坏了。
不是 FPGA 芯片坏了。
也不是 --validate 这条用户态校验命令导致的。

这是 FPGA PCIe DMA 设计和 Linux 驱动控制协议之间没有安全握手，导致 FPGA 可能向 RK3568 的错误物理内存地址写数据。
```

更准确地说，这是一个 **PCIe DMA 主设备安全控制问题**。

### 1. 谁在主动写内存

在这套系统里：

```text
RK3568 / 鲁班猫2 = PCIe Root Complex，提供主机内存
FPGA             = PCIe Endpoint，同时也是 DMA Master
```

真正发起内存写的人是 FPGA。FPGA 通过 PCIe MWR TLP 主动写 RK3568 的物理内存。

所以如果写错地址，直接破坏的是 RK Linux 内存和文件系统缓存：

```text
FPGA 发起错误 PCIe MWR
  -> RK3568 PCIe 控制器接收写事务
  -> 写入某个物理地址
  -> 如果这个地址不是 Linux 分配给 DMA buffer 的地址
  -> 可能覆盖内核、进程、页缓存、rootfs 缓存
  -> 系统卡死或重启后桌面进不去
```

这里 RK 只是按照 PCIe 协议接收来自 Endpoint 的 Memory Write。它不会天然知道“这个地址是不是你本来想写的 buffer”。

### 2. 为什么更偏向 FPGA/协议侧问题

从 FPGA 代码看，当前逻辑比较危险：

```text
Linux 连续写 4 个非零 DMA 地址到 0x110
FPGA 保存到 dma_addr0..dma_addr3
只要 4 个地址非 0
FPGA 就把 rc_cfg_ep_flag 置 1
后面遇到 VS 边沿就自动开始 DMA
```

也就是说，FPGA 端当前没有这些保护：

```text
没有必须先写 MAGIC/ARM 才允许接收地址
没有单独 START 命令
没有 ADDR_ECHO 让 Linux 读回确认地址
没有地址范围检查
没有 LEN 限制作为安全边界
没有 STATUS/frame_done 让 Linux 精确判断状态
```

因此，FPGA 只要保存了错误地址，或者保存了上一次测试残留的旧地址，就可能自动开始写 RK 内存。

这就是为什么问题更偏向：

```text
FPGA DMA 控制协议不够安全
FPGA 端缺少防误启动机制
Linux 驱动一开始也没有足够早地 STOP/清旧地址
```

### 3. Linux/RK 端有没有责任

有，但不是“RK 硬件问题”。Linux 端的问题是驱动一开始还不够保守。

之前 Linux 驱动存在的风险：

```text
probe 后没有第一时间 STOP FPGA
没有第一时间清 dma_addr0..3
allow_dma_start 保护只能阻止写新地址，不能阻止 FPGA 使用旧地址
早期文档建议尝试 bar=0/1、addr_byteswap=0/1，这对 DMA master 来说太冒险
```

现在 Linux 端已经做了补救：

```text
probe 阶段先 safe_stop，再 pci_set_master，避免旧地址窗口期自动 DMA
probe 阶段写 0x130 STOP，并连续 4 次写 0 到 0x110，尝试清 dma_addr0..3
STOP/FREE/remove 都走 safe stop
新增 --safe-stop，用户可手动 STOP + 清地址
allow_dma_start 默认 0，不显式允许就不 START
START 前打印 Linux DMA 地址和实际写给 FPGA 的值
```

所以现在 Linux 端的原则是：

```text
默认不启动 DMA
先清旧状态
必须人工确认 FPGA 保存的地址正确
确认后才 allow_dma_start=1
```

### 4. RK3568 本身能不能避免这个问题

一般情况下，普通嵌入式 Linux PCIe RC 不一定默认给 Endpoint 开严格 IOMMU 隔离。没有 IOMMU/DMA remapping 保护时，PCIe Endpoint 作为 DMA Master 拿到什么地址就能写什么地址。

所以 RK3568 不是“坏了”，而是：

```text
它没有自动替你阻止一个 PCIe Endpoint 写错物理地址。
```

如果平台有可用 IOMMU/SMMU 并且 PCIe 设备接入了 IOMMU，理论上可以把 FPGA DMA 限制在指定 buffer 范围内。但当前工程没有确认 RK3568 PCIe 对这个 Endpoint 已启用 IOMMU 隔离，所以不能假设它能兜底。

### 5. 最终责任怎么划分

可以这样划分：

| 层级 | 是否有问题 | 说明 |
| --- | --- | --- |
| RK3568 硬件 | 不是主要问题 | 它只是执行 PCIe Memory Write，不能自动判断地址是否逻辑正确 |
| Linux 系统 | 受害者 | 错误 DMA 会破坏 Linux 内存、页缓存、文件系统 |
| Linux 驱动早期版本 | 有不足 | probe 阶段没有立即 STOP/清旧地址，安全闸门不够早 |
| 当前 Linux 驱动 | 已补强 | 增加 probe STOP、清地址、allow_dma_start、--safe-stop |
| FPGA 当前 DMA 协议 | 主要风险来源 | 4 地址非 0 就自动 DMA，缺少 MAGIC/START/ADDR_ECHO/STATUS/LEN |
| FPGA 芯片硬件 | 不是坏 | 当前看不到会伤 FPGA 寿命的逻辑，风险主要是写坏 RK Linux 系统 |

一句话总结：

```text
问题主要不是 RK 的问题，而是 FPGA DMA 协议太信任软件写入的地址；Linux 早期驱动也没有足够早地清掉 FPGA 残留 DMA 状态。当前 Linux 侧已经补强，但要真正安全，还需要 FPGA 端增加 MAGIC/START/ADDR_ECHO/STATUS/LEN 这些保护。
```

### 6. 对 Linux 负责人的实际要求

你作为 Linux 端负责人，后续不要把目标理解成“修 RK”。你的目标是：

```text
让 Linux 驱动默认不启动危险 DMA
上电后先 STOP/清地址
提供 --safe-stop 手动清状态
打印 DMA 地址给 FPGA 端核对
没有 FPGA 地址回读/ILA 确认前，不允许 allow_dma_start=1
```

而需要 FPGA 端配合的事情是：

```text
确认 0x130 STOP 能清地址并停止 MWR
确认 addr_byteswap=1 后 dma_addr0..3 和 Linux written 值一致
增加 ADDR_ECHO，让 Linux 能读回地址
增加 MAGIC/ARM 和 START，禁止 4 个地址非 0 就自动 DMA
增加 STATUS/frame_done，替代固定延时
增加 LEN，限制每帧最大写入长度
```

## FPGA 代码核查结论：问题是什么，是否伤硬件

已核查 FPGA 文件：

```text
PCIE_DMA_test_color_MES50HP_X1/source/pcie_dma_ctrl.v
PCIE_DMA_test_color_MES50HP_X1/source/ddr_test_top_pcie_fixed.v
PCIE_DMA_test_color_MES50HP_X1/source/ddr_test_top.v
PCIE_DMA_test_color_MES50HP_X1/ipcore/pcie_test/example_design/rtl/ipsl_pcie_dma_ctrl/ipsl_pcie_dma_rx_top.v
```

结论分两层看。

第一层：这次系统崩溃的高概率原因是 PCIe DMA 写错主机物理地址。

依据：

```text
pcie_dma_ctrl.v 里 0x110 是 DMA 地址配置寄存器
Linux 连续写 4 次 0x110 后，FPGA 保存为 dma_addr0..dma_addr3
FPGA 代码会把 Linux 写入的数据做 32-bit 字节重排后保存
4 个 dma_addr 都非 0 后，rc_cfg_ep_flag 置 1
后续遇到视频 VS 边沿和 FIFO 水位条件后，FPGA 自动发起 MWR 写主机内存
MWR TLP Header 里的目标地址来自 alloc_addrl，也就是 dma_addr0..dma_addr3
每个 TLP 写 64 bytes，单帧约 4 MB
```

关键代码位置：

```text
pcie_dma_ctrl.v:40   DMA_CMD_L_ADDR = 12'h110
pcie_dma_ctrl.v:41   DMA_CMD_CLEAR_ADDR = 12'h130
pcie_dma_ctrl.v:157  收到 MWR_32 且偏移 0x110 时保存 DMA 地址
pcie_dma_ctrl.v:161  dma_addr0 做了字节重排
pcie_dma_ctrl.v:166  dma_addr1 做了字节重排
pcie_dma_ctrl.v:171  dma_addr2 做了字节重排
pcie_dma_ctrl.v:176  dma_addr3 做了字节重排
pcie_dma_ctrl.v:221  4 个地址非 0 后 rc_cfg_ep_flag 置 1
pcie_dma_ctrl.v:224  VS 边沿后开始准备一帧 DMA
pcie_dma_ctrl.v:322  MWR 目标地址使用 alloc_addrl
pcie_dma_ctrl.v:355  每个 TLP 后 alloc_addrl += 64
pcie_dma_ctrl.v:181  收到 0x130 后 dma_stop_flag 置 1
```

第二层：从目前代码看，这类问题主要伤 Linux 系统，不是伤板子硬件寿命。

原因：FPGA 发的是 PCIe Memory Write TLP，本质是“往主机物理地址写数据”。如果地址错，危险对象是：

```text
Linux 内核内存
用户进程内存
页缓存
SD 卡 rootfs 文件系统缓存
系统文件
```

所以可能导致系统卡死、文件系统损坏、桌面进不去。但这不等价于烧坏鲁班猫或 FPGA。当前代码没有看到会直接改变电源、电压、IO 标准、时钟过压这类会影响硬件寿命的行为。

仍然要注意：硬件寿命风险主要来自这些外部因素，不是这段 DMA 逻辑本身：

```text
热插拔 PCIe 转接板
供电电压不匹配或电源不稳
PERST#、REFCLK、GND 接触不可靠
接口方向或排线接错
FPGA IO 电平标准与转接板不匹配
长时间短路、过流、过热
```

所以为了安全，后续操作原则是：

```text
不热插拔
先断电再插拔转接板和 FPGA
系统恢复前先拔掉 FPGA，避免旧 bitstream 继续 DMA
不确认 BAR 和地址字节序前，不允许 allow_dma_start=1
不靠 bar=0/1、addr_byteswap=0/1 盲试
必须用 FPGA ILA/SignalTap 对比 Linux dmesg 里的 written 地址和 FPGA 内部 dma_addr0..dma_addr3
```

Linux 驱动已经加了保护：

```text
allow_dma_start 默认是 0，不会真正启动 FPGA DMA
只有显式 insmod allow_dma_start=1 才会向 FPGA 写 4 个 DMA 地址
驱动会打印 dma_addr、low32、written、addr_byteswap，供 FPGA 端抓波形对比
```

真正想把风险再降一档，建议 FPGA 端后续加协议保护：

```text
增加 ARM/MAGIC 寄存器，例如必须先写 0xA55A5AA5 才允许 DMA
增加 START 寄存器，不要 4 个地址非 0 就自动开始
增加 LEN 寄存器，限制最多写 4147200 或 4147264 bytes
增加 STATUS 寄存器，可读 current_page/frame_done/dma_busy/error
增加 ADDR_ECHO 寄存器，可让 Linux 读回 FPGA 实际保存的 dma_addr0..dma_addr3
STOP 后立即清空地址并禁止继续 MWR
如果地址为 0、未对齐、未 armed，禁止 MWR
```

在 FPGA 端没有这些保护前，Linux 只能靠 `allow_dma_start=0` 默认保护和人工核对降低风险，不能从软件上 100% 硬隔离一个配置错误的 PCIe DMA Master。

## 对 PCIe 调试安全指南的采纳情况

针对 `PCIe调试安全指南与优化建议.md`，当前 Linux 工程已经采纳这些建议：

```text
[x] probe 阶段立即写 STOP，降低 FPGA 使用旧 DMA 地址自动写主机内存的风险
[x] probe 阶段连续 4 次写 0 到 0x110，尝试清除 dma_addr0..3
[x] allow_dma_start 默认关闭，不显式允许就拒绝 START
[x] addr_byteswap 默认按 FPGA 当前字节反转逻辑设为 1；真正启动仍由 allow_dma_start 控制
[x] START 前打印 dma_addr、low32、written、addr_byteswap，便于和 FPGA ILA 对表
[x] STOP/FREE/remove 都走安全停止路径
[x] 新增用户态 --safe-stop，只 STOP + 清地址，不分配 buffer，不启动 DMA
```

暂不建议现在做的 Linux 侧功能：

```text
bar_auto_detect：没有 FPGA 回读寄存器时只能靠写 STOP/0 探测，不能真正证明 BAR 正确
--probe-bar：如果用 devmem/resource 做裸写，反而容易绕过驱动保护，不适合现在阶段
自动尝试 bar=0/1、addr_byteswap=0/1：这属于盲试 DMA 地址，风险太高
```

我额外建议增加两条 Linux 侧规则：

```text
1. 正式采集前必须先执行 --safe-stop 并查看 dmesg，确认 STOP 日志出现。
2. 采集生成的 raw 文件先保存到 /tmp 或外接 U 盘，减少 SD 卡 rootfs 被异常写缓存影响的概率。
```

仍然必须让 FPGA 端配合的建议：

```text
ADDR_ECHO：Linux 读回 FPGA 实际保存的 dma_addr0..3，这是最关键的安全改进
ARM/MAGIC：没有 magic 不接受 0x110 地址写入
START：不要 4 个地址非 0 就自动启动
STATUS/frame_done：替代 Linux 固定 frame_wait_ms
LEN：限制每帧最多写入字节数
STOP 后清地址并禁止继续 MWR：当前代码已有类似逻辑，但建议 FPGA 端再确认综合后行为
```


## 对安全指南第 11 节更新的分析

`PCIe调试安全指南与优化建议.md` 最后一节的核心判断是有价值的：当前最大的残余风险确实不是 `--once` 里的新地址写入，而是 **FPGA 内部可能残留旧 dma_addr0..3**。如果旧地址非零、PCIe Bus Master 又被重新打开、同时 VS 信号存在，FPGA 可能不等 Linux START 就自动 MWR 写主机内存。

我认可这些结论：

```text
1. FPGA 当前启动条件是 4 个地址非零 + EP 活跃 + VS 边沿。
2. allow_dma_start=0 只能阻止 Linux 写入新地址，不能清除 FPGA 内部旧地址。
3. probe 阶段必须尽早 STOP + 清 dma_addr0..3。
4. remove 阶段也必须 STOP + 清地址，避免下一次 insmod 继承旧状态。
5. addr_byteswap=1 符合当前 FPGA 32-bit 地址字节反转逻辑。
6. 32-bit DMA mask 和 buffer 尺寸余量是必要保护。
```

但我认为第 11 节里有一个地方还可以更保守：

```text
原说法：pci_set_master() 后几个微秒内 safe_stop，通常能赶在第一个 VS 边沿前清旧地址。
我的判断：不应该依赖“通常能赶上”。更安全做法是先不要开启 Bus Master。
```

当前代码已经按更保守方案调整：

```text
probe 顺序：
1. pci_enable_device_mem()       只允许主机访问 BAR/MMIO
2. pci_request_region()
3. pci_iomap()
4. colorbar_hw_safe_stop()       写 0x130 STOP + 4 次 0x110=0
5. pci_set_master()              最后才允许 FPGA 主动 DMA
```

这样做的意义：

```text
主机写 BAR 不需要 FPGA 具备 Bus Master 能力。
FPGA 主动发起 PCIe MWR 才需要 Bus Master。
所以先 STOP/清地址，再 pci_set_master，可以进一步缩短甚至消除旧地址自动 DMA 的窗口。
```

remove 阶段也已经调整为：

```text
1. colorbar_stop_locked()        STOP + 清地址
2. pci_clear_master()            关闭 Endpoint Bus Master 能力
3. colorbar_free_buffers_locked()
4. pci_iounmap/pci_release/pci_disable
```

对第 11 节“唯一残余风险是 BAR 选错”的分析，我的补充是：

```text
这个判断基本对，但前提是 FPGA 内部确实可能残留旧地址。
如果 FPGA 重新上电/重新加载 bitstream，旧地址归零，那么 BAR 选错通常不会立刻导致旧地址 DMA。
如果没有重新上电，且上一次异常退出留下旧地址，那么 BAR 选错会让 STOP 无法到达 FPGA，这是高风险。
```

因此更严格的安全流程是：

```text
1. 发生过系统崩溃后，先给 FPGA 重新上电或重新加载 bitstream。
2. Linux 驱动 probe 阶段先 safe_stop，再 pci_set_master。
3. 手动执行 --safe-stop，再看 dmesg 确认日志。
4. 未确认 BAR 正确前，不加 allow_dma_start=1。
5. 最终仍建议 FPGA 端增加 ADDR_ECHO/STATUS/MAGIC/START，彻底消除 BAR 和地址猜测。
```

一句话：

```text
第 11 节指出的旧地址残留风险是对的；当前 Linux 侧已经进一步把 pci_set_master 延后到 safe_stop 之后，比指南里的 probe 防护更保守。
```

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
probe 时立即 STOP 并连续 4 次写 0 清除 FPGA 旧 DMA 地址
默认禁止真正 START，必须显式 allow_dma_start=1 才会向 FPGA 写 DMA 地址
START 时连续 4 次写 BAR+0x110，把 DMA 地址告诉 FPGA
STOP/SAFE_STOP 时写 BAR+0x130 并连续 4 次写 0 清地址
WAIT_FRAME 当前先用 frame_wait_ms 固定延时模拟等待一帧
```

当前用户态工具做的事情：

```text
--info                         显示当前图像和 buffer 参数
--safe-stop                    只发送 STOP + 清 4 个地址，不启动 DMA
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

先加载安全模式。安全模式下不会真正启动 FPGA DMA：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
```

参数含义：

```text
bar=1                先按 BAR1 映射 FPGA 控制寄存器，不对再试 bar=0
addr_byteswap=1      按 FPGA 当前 32-bit 地址字节反转逻辑写地址
allow_dma_start=0    默认值，禁止 START 真正向 FPGA 写 DMA 地址
frame_wait_ms=100    临时等待一帧 100ms，后续应换成 FPGA 状态寄存器或中断
```

只有同时满足下面条件，才允许显式打开 DMA：

```text
已经备份或准备好重刷系统镜像
FPGA 端确认控制寄存器确实在当前 BAR
FPGA 端确认 0x110 连续 4 次写地址协议正确
FPGA 端用 ILA/SignalTap 确认收到的 dma_addr 和 dmesg 打印的 dma_addr 完全一致
FPGA 端确认 0x130 stop 有效，且不会继续使用旧 DMA 地址写主机内存
```

确认后才使用：

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
```

当前 FPGA 代码已经显示内部会做 32-bit 字节反转，所以默认使用 `addr_byteswap=1`。如果 FPGA 端后续修改了地址解析逻辑，再同步改 Linux 参数。

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

驱动加载成功，并且 `/dev/colorbar_pcie_rx` 存在后，只有在 `allow_dma_start=1` 已经明确打开时才运行：

```sh
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
```

如果没有加 `allow_dma_start=1`，这条命令应该在 `COLORBAR_IOC_START` 处失败，这是预期的安全保护，表示没有真正启动 FPGA DMA。

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

### 11.3 BAR 或地址字节序不能靠反复盲试

之前系统镜像崩溃后，不能再按“BAR1 不行就试 BAR0、addr_byteswap 不行就反过来”的方式盲试。原因是 FPGA DMA 一旦拿到错误地址，可能写坏任意主机内存。

正确顺序是：

```text
先让 FPGA 端用 ILA/SignalTap 抓 BAR 写事务
确认 Linux 写的是不是 FPGA 真正监听的 BAR
确认 Linux 写 BAR+0x110 时，FPGA 内部 dma_addr0..dma_addr3 变成了什么值
把 FPGA 抓到的 dma_addr 和 Linux dmesg 打印的 dma_addr 对比
完全一致后，再 allow_dma_start=1
```

安全加载但不启动 DMA：

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=0 addr_byteswap=0 frame_wait_ms=100
```

真正启动 DMA 只能在协议确认后执行，例如：

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
./build/pcie_color_rx --validate frame_0000.rgb565
```

### 11.4 数据像半帧或偶发错就加等待时间

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=200
sudo ./build/pcie_color_rx --once --output frame_wait200.rgb565
./build/pcie_color_rx --validate frame_wait200.rgb565
```

前提仍然是 BAR 和地址字节序已经确认，不能用这组命令盲试。

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

系统镜像崩溃后，当前最推荐的命令改成“安全检查版”，不真正启动 DMA：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make
lspci -nn
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
ls -l /dev/colorbar_pcie_rx
dmesg | tail -n 120
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 80
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
sudo rmmod colorbar_pcie_driver
```

这时 `--once` 应该因为没有 `allow_dma_start=1` 而失败，目的是确认保护生效，不让 FPGA 真正写主机内存。

等 FPGA 端确认 BAR 和地址字节序后，才使用真正采集版：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make
lspci -nn
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
ls -l /dev/colorbar_pcie_rx
dmesg | tail -n 120
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 80
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
ls -l frame_0000.rgb565
./build/pcie_color_rx --validate frame_0000.rgb565
sudo rmmod colorbar_pcie_driver
```

当前真正采集版默认使用 `addr_byteswap=1`；如果 FPGA 端后续移除地址字节反转，再改为 `addr_byteswap=0`。

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
[x] 新增 allow_dma_start 保护，默认禁止误启动 FPGA DMA
[x] 新增 --safe-stop / COLORBAR_IOC_SAFE_STOP，手动 STOP + 清旧地址
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
[x] 当前 FPGA 代码下 addr_byteswap 应为 1
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

## 当前下一步到底要调试什么

现在不要急着调试“彩条图像内容对不对”，也不要急着加 `allow_dma_start=1` 真正采集。

当前第一目标是调试：

```text
Linux 和 FPGA 之间的 PCIe DMA 安全握手是否成立。
```

也就是说，先确认不会再出现 FPGA 写错 RK3568 内存、导致系统镜像损坏的问题。

### 1. 当前最优先调试的问题

按优先级排列：

```text
1. BAR 到底对不对
2. STOP 是否真的到达 FPGA
3. STOP 是否真的能清 dma_addr0..3 和 DMA 状态
4. addr_byteswap=1 后，FPGA 内部 dma_addr0..3 是否等于 Linux DMA 地址
5. 不加 allow_dma_start=1 时，FPGA 是否绝对不会启动 DMA
```

### 2. 问题 1：BAR 到底对不对

Linux 现在默认使用：

```sh
bar=1
```

但必须确认：

```text
Linux 写 BAR1 + 0x130 时，FPGA 端 dma_stop_flag 有没有变化。
Linux 写 BAR1 + 0x110 时，FPGA 端 dma_addr0..3 有没有变化。
```

如果 BAR 错了，Linux 以为自己在 STOP，实际上 FPGA 没收到，旧 DMA 地址可能还在。

所以 BAR 是当前最关键的问题。

### 3. 问题 2：STOP 是否真的清掉 FPGA 状态

Linux 的 `--safe-stop` 会做：

```text
写 0x130 STOP
连续 4 次写 0 到 0x110
```

FPGA 端应该看到：

```text
dma_stop_flag 置 1
dma_addr0 = 0
dma_addr1 = 0
dma_addr2 = 0
dma_addr3 = 0
rc_cfg_ep_flag = 0
fram_start = 0
dma_start = 0
dma_cnt = 0
```

这一步如果没有确认，就不能认为系统安全。

### 4. 问题 3：addr_byteswap=1 是否正确

当前 FPGA 代码里对 32-bit 地址做了字节反转，所以 Linux 默认使用：

```sh
addr_byteswap=1
```

驱动 START 前会在 `dmesg` 打印类似：

```text
program DMA address: dma=... low32=0x???????? written=0x???????? addr_byteswap=1
```

需要 FPGA 端用 ILA/SignalTap 对比：

```text
FPGA 内部 dma_addr0..3 是否等于 Linux dmesg 里的 low32 原始 DMA 地址。
```

如果 FPGA 内部保存的是 `written`，或者是其他值，说明字节序理解还不对。

### 5. 问题 4：不加 allow_dma_start=1 时是否绝对不会 DMA

这是 Linux 侧安全底线。

安全模式加载：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
```

手动安全停止：

```sh
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 100
```

然后故意执行一次 `--once`：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
```

预期结果：

```text
COLORBAR_IOC_START: Operation not permitted
```

这个失败是正确的，说明：

```text
allow_dma_start=0 时，驱动拒绝 START。
Linux 没有向 FPGA 写入新的非零 DMA 地址。
FPGA 不应该启动 DMA。
```

### 6. 安全调试命令顺序

当前推荐只跑这一组，不要加 `allow_dma_start=1`：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make
lspci -nn
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
ls -l /dev/colorbar_pcie_rx
dmesg | tail -n 120
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 100
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
sudo rmmod colorbar_pcie_driver
```

预期：

```text
--safe-stop 成功
--once 在 COLORBAR_IOC_START 处被拒绝
系统不崩溃
重启后系统仍然正常
```

### 7. FPGA 端需要配合抓的信号

请 FPGA 端优先抓这些信号：

```text
tlp_fmt
tlp_type
mwr_addr
cmd_reg_addr
dma_addr0
dma_addr1
dma_addr2
dma_addr3
dma_stop_flag
rc_cfg_ep_flag
fram_start
dma_start
dma_cnt
bar_hit 或 o_bar1_wr_en
```

重点观察：

```text
Linux 执行 --safe-stop 时，cmd_reg_addr 是否出现 0x130。
Linux 执行 --safe-stop 后，dma_addr0..3 是否为 0。
Linux 不加 allow_dma_start=1 执行 --once 时，dma_addr0..3 是否仍为 0。
Linux 不加 allow_dma_start=1 执行 --once 时，dma_start 是否保持 0。
```

### 8. 什么时候才能真正采集一帧

只有下面条件都满足，才允许进入真正采集阶段：

```text
[ ] 确认 BAR 正确
[ ] 确认 0x130 STOP 能到达 FPGA
[ ] 确认 --safe-stop 后 dma_addr0..3 全部为 0
[ ] 确认 allow_dma_start=0 时 --once 被拒绝，FPGA 不启动 DMA
[ ] 确认 addr_byteswap=1 后，FPGA 内部 dma_addr0..3 等于 Linux DMA 地址
[ ] 已经备份系统镜像或准备好重刷
```

满足后，才使用：

```sh
sudo rmmod colorbar_pcie_driver
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --safe-stop
sudo ./build/pcie_color_rx --once --output /tmp/frame_0000.rgb565
./build/pcie_color_rx --validate /tmp/frame_0000.rgb565
sudo rmmod colorbar_pcie_driver
```

### 9. 当前一句话结论

```text
现在要调试的是 BAR、STOP、清地址、字节序、allow_dma_start 保护是否成立。
这些都确认之前，不调试彩条图像内容，也不真正启动 DMA。
```


## BAR 和安全链路具体怎么调试

这一节只讲“具体怎么看、看什么、按什么顺序判断”。当前不要直接调图像内容，先把 PCIe 控制链路和 DMA 安全链路调通。

### 1. 总体原则

BAR 要两边一起看：

```text
Linux 侧：看这个 PCIe 设备枚举出了哪些 BAR，每个 BAR 的物理地址和大小是多少。
FPGA 侧：看 Linux 写某个 BAR 的偏移时，FPGA 控制逻辑是否真的收到。
```

最终以 FPGA 侧为准：

```text
Linux 写 BARx + 0x130
FPGA 端 cmd_reg_addr 看到 0x130
dma_stop_flag 跳变
```

只有这样才能证明 `bar=x` 是正确的控制 BAR。


### 1.1 FPGA 代码里能不能直接看出 BAR

可以在 FPGA 代码里查到一部分，但不能只靠静态代码 100% 确认运行时实际使用哪个 BAR。

当前 FPGA 工程里，PCIe IP 配置文件：

```text
PCIE_DMA_test_color_MES50HP_X1/ipcore/pcie_test/pcie_test.v
```

能看到 BAR 配置：

```text
BAR0_ENABLED = 1
BAR1_ENABLED = 1
BAR2_ENABLED = 1
BAR0_MASK    = 31'hfff
BAR1_MASK    = 31'h7ff
BAR2_MASK    = 31'hfff
```

这说明 FPGA PCIe IP 开了 BAR0、BAR1、BAR2。

Pango 示例 DMA 接收代码：

```text
PCIE_DMA_test_color_MES50HP_X1/ipcore/pcie_test/example_design/rtl/ipsl_pcie_dma_ctrl/ipsl_pcie_dma_rx_top.v
```

里面有 BAR1 写接口逻辑：

```verilog
if(bar_hit == 2'b1 && (DEVICE_TYPE == 3'b000 || DEVICE_TYPE == 3'b001)) begin
    o_bar1_wr_en   = 1'b1;
    o_bar1_wr_addr = mwr_addr[ADDR_WIDTH-1:0];
    o_bar1_wr_data = mwr_data;
end
```

所以从 PCIe IP 示例代码看，BAR1 很像控制寄存器通道。

但当前彩条 DMA 控制模块：

```text
PCIE_DMA_test_color_MES50HP_X1/source/pcie_dma_ctrl.v
```

不是直接接 `o_bar1_wr_en`，而是接：

```text
axis_master_tvalid
axis_master_tdata
axis_master_tkeep
axis_master_tlast
```

它自己从 TLP 里解析：

```verilog
cmd_reg_addr <= axis_master_tdata_d0[75:64];
```

然后只判断低 12 位偏移：

```verilog
cmd_reg_addr == 12'h110
cmd_reg_addr == 12'h130
```

这意味着静态代码只能说明：

```text
BAR1 是高概率正确选择。
但不能只靠代码断言 BAR1 一定正确。
```

原因是：如果 `pcie_dma_ctrl` 收到的是原始 MWr TLP，那么 Linux 写不同 BAR 的同一个偏移：

```text
BAR0 + 0x130
BAR1 + 0x130
BAR2 + 0x130
```

在低 12 位上都可能表现为：

```text
cmd_reg_addr = 0x130
```

最终这些 TLP 是否都会进入 `pcie_dma_ctrl`，取决于 PCIe IP 内部 BAR 过滤、顶层连接、以及 `axis_master_tdata` 输出的到底是哪一路 TLP。

所以最终确认必须看 FPGA 运行时信号：

```text
bar_hit
o_bar1_wr_en
axis_master_tvalid
axis_master_tdata
cmd_reg_addr
dma_stop_flag
dma_addr0..3
```

最直接的判断方法：

```text
Linux 使用 bar=1 执行 --safe-stop。
如果 FPGA 看到：
  bar_hit == 2'b1
  o_bar1_wr_en 跳变
  cmd_reg_addr == 0x130
  dma_stop_flag 跳变
那么 bar=1 可以确认是正确控制 BAR。
```

如果 `bar=1` 下 FPGA 端完全看不到 `0x130` 或 `dma_stop_flag` 不动，再在 FPGA 重新上电/重新加载 bitstream 后安全测试 `bar=0`。

一句话：

```text
FPGA 代码能看出 BAR 配置和大概倾向，但最终哪个 BAR 真的接到 pcie_dma_ctrl，必须靠 FPGA 抓信号确认。
当前静态判断：优先怀疑 BAR1 是对的，但不能跳过运行时验证。
```

### 2. 调试前安全前提

每次做 BAR 调试前，先保证 FPGA 内部没有旧 DMA 地址：

```text
优先方式：FPGA 重新上电或重新加载 bitstream。
目的：让 dma_addr0..3 回到 0，避免旧地址残留。
```

Linux 侧不要加：

```sh
allow_dma_start=1
```

只允许安全模式：

```sh
allow_dma_start=0
```

也就是 insmod 时不写 `allow_dma_start=1`。

### 3. 第一步：Linux 侧看枚举出来的 BAR

运行：

```sh
lspci -vv -s 01:00.0
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

重点看：

```text
BAR0 起始地址、结束地址、大小
BAR1 起始地址、结束地址、大小
BAR2 起始地址、结束地址、大小
```

之前日志里类似：

```text
BAR0: 0xf4200000 - 0xf4201fff  大小 8KB
BAR1: 0xf4204000 - 0xf4204fff  大小 4KB
BAR2: 0xf4202000 - 0xf4203fff  大小 8KB，64-bit
```

这一步只能说明 Linux 看到哪些 BAR，不能证明 FPGA 控制逻辑在哪个 BAR。

### 4. 第二步：先测 bar=1 的 STOP 是否到 FPGA

因为 Pango PCIe IP 代码里有 `o_bar1_wr_en`，并且 BAR1 大小像控制寄存器空间，所以先测 `bar=1`。

Linux 侧执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
dmesg | tail -n 120
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 100
sudo rmmod colorbar_pcie_driver
```

FPGA 端抓这些信号：

```text
bar_hit
o_bar1_wr_en
mwr_addr
cmd_reg_addr
dma_stop_flag
dma_addr0
dma_addr1
dma_addr2
dma_addr3
rc_cfg_ep_flag
fram_start
dma_start
```

判断标准：

```text
看到 cmd_reg_addr = 0x130
看到 dma_stop_flag 跳变
safe-stop 后 dma_addr0..3 全部为 0
rc_cfg_ep_flag = 0
fram_start = 0
dma_start = 0
```

如果这些都成立：

```text
bar=1 基本确认是控制 BAR。
```

如果 FPGA 完全看不到 `0x130` 或 `dma_stop_flag` 不动：

```text
bar=1 可能不是控制 BAR，或者 FPGA 抓取信号位置不对。
```

### 5. 第三步：如果 bar=1 不通，再安全测 bar=0

只有在 FPGA 已重新上电或重新加载 bitstream、确认旧地址为 0 的情况下，才测试 `bar=0`。

Linux 侧执行：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=0 addr_byteswap=1 frame_wait_ms=100
dmesg | tail -n 120
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 100
sudo rmmod colorbar_pcie_driver
```

FPGA 端仍然看：

```text
cmd_reg_addr 是否出现 0x130
dma_stop_flag 是否跳变
dma_addr0..3 是否为 0
```

判断：

```text
bar=0 能触发 0x130，bar=1 不能 -> 控制 BAR 是 BAR0。
bar=1 能触发 0x130，bar=0 不能 -> 控制 BAR 是 BAR1。
两个都不能 -> Linux 写入没有进 pcie_dma_ctrl，需检查 PCIe IP BAR 配置/抓取信号/顶层连接。
两个都能 -> 需要 FPGA 端确认 BAR 解码是否有重叠或两个路径都接到了控制逻辑。
```

### 6. 第四步：确认 allow_dma_start=0 保护是否有效

确认 BAR 后，继续保持不加 `allow_dma_start=1`。

Linux 侧执行：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --safe-stop
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
sudo rmmod colorbar_pcie_driver
```

如果正确，`--once` 应该报：

```text
COLORBAR_IOC_START: Operation not permitted
```

FPGA 端应该看到：

```text
dma_addr0..3 仍然为 0
rc_cfg_ep_flag 仍然为 0
dma_start 不跳变
没有 MWR 发出
```

这一步通过，说明 Linux 安全闸门有效。

### 7. 第五步：确认 addr_byteswap=1 是否正确

当前 FPGA 代码对地址做了字节反转：

```verilog
dma_addr0 <= {data[7:0], data[15:8], data[23:16], data[31:24]};
```

所以 Linux 默认：

```sh
addr_byteswap=1
```

真正写地址时，驱动会打印：

```text
program DMA address: dma=... low32=0xAAAAAAAA written=0xBBBBBBBB addr_byteswap=1
```

FPGA 端要确认：

```text
FPGA 内部 dma_addr0 应该等于 low32=0xAAAAAAAA
不是 written=0xBBBBBBBB
```

注意：这一步会写非零 DMA 地址，存在真正 DMA 风险。只有下面条件满足后才能做：

```text
BAR 已确认正确
STOP 已确认有效
safe-stop 后地址能清零
allow_dma_start=0 保护已确认有效
系统镜像已备份或准备好重刷
FPGA 端正在抓 ILA/SignalTap
```

然后才允许短时间测试：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --safe-stop
sudo ./build/pcie_color_rx --once --output /tmp/frame_addr_test.rgb565
sudo rmmod colorbar_pcie_driver
```

这一步的目的不是看图像，而是让 FPGA 抓到 `dma_addr0..3`，确认地址字节序。

### 8. 第六步：确认 STOP/remove 后不会留下旧地址

真正启动过一次 DMA 后，卸载驱动前后都要确认 FPGA 地址被清掉。

Linux 侧：

```sh
sudo ./build/pcie_color_rx --safe-stop
sudo rmmod colorbar_pcie_driver
```

FPGA 端确认：

```text
dma_addr0 = 0
dma_addr1 = 0
dma_addr2 = 0
dma_addr3 = 0
rc_cfg_ep_flag = 0
dma_start = 0
```

然后重新 insmod 安全模式：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
```

预期仍然：

```text
COLORBAR_IOC_START: Operation not permitted
```

如果这一步也安全，说明旧地址残留问题基本被 Linux 侧控制住。

### 9. 需要调试的问题总表

| 优先级 | 问题 | Linux 看什么 | FPGA 看什么 | 通过标准 |
| --- | --- | --- | --- | --- |
| P0 | BAR 是否正确 | `insmod bar=1/bar=0`、`dmesg` | `cmd_reg_addr=0x130`、`dma_stop_flag` | 写 STOP 时 FPGA 有响应 |
| P0 | STOP 是否有效 | `--safe-stop` 返回成功 | `dma_addr0..3=0`、`rc_cfg_ep_flag=0` | FPGA 清地址、停状态机 |
| P0 | 不加 allow_dma_start 是否安全 | `--once` 返回 `EPERM` | `dma_start=0`、无 MWR | 不启动 DMA |
| P1 | addr_byteswap 是否正确 | `dmesg` 的 `low32/written` | `dma_addr0..3` | FPGA 地址等于 Linux low32 |
| P1 | remove 是否清旧地址 | `rmmod` 后无异常 | `dma_addr0..3=0` | 下次 insmod 不继承旧地址 |
| P2 | frame_wait_ms 是否够 | raw 文件稳定性 | `frame_done/dma_cnt` | 不半帧、不错页 |
| P2 | 图像格式是否正确 | `--validate` 采样点 | FPGA 输出 RGB565 | 8 色采样 OK |

### 10. 当前不要调试什么

这些先不要碰：

```text
不要先调彩条颜色
不要先调实时显示
不要先调 framebuffer/DRM/V4L2
不要反复盲试 bar=0/1 + allow_dma_start=1
不要反复盲试 addr_byteswap=0/1 + allow_dma_start=1
```

原因：

```text
BAR、STOP、清地址、安全闸门没确认前，启动 DMA 仍可能写坏系统。
```

### 11. 最终判断顺序

按这个顺序打勾：

```text
[ ] Linux 能枚举 0755:0755
[ ] Linux 能加载 colorbar_pcie_driver
[ ] Linux 能创建 /dev/colorbar_pcie_rx
[ ] bar=1 或 bar=0 已确认能触发 FPGA dma_stop_flag
[ ] --safe-stop 后 dma_addr0..3 清零
[ ] allow_dma_start=0 时 --once 被拒绝，FPGA 不 DMA
[ ] addr_byteswap=1 时 FPGA dma_addr0..3 等于 Linux low32
[ ] remove/rmmod 后 FPGA 地址仍清零
[ ] allow_dma_start=1 后能保存一帧 raw
[ ] --validate 全部 OK
```

前 8 项没有完成前，不进入真正采集和图像验证。


## 当前只需要执行的调试命令

如果现在重新开始调试，只看这一节。前面的内容是原因分析和背景说明。

当前目标不是采集彩条，而是确认：

```text
PCIe 设备枚举正常
BAR 信息能看到
安全驱动能加载
safe-stop 能执行
不加 allow_dma_start=1 时 --once 会被拒绝
系统不会崩溃
```

### 第 1 组：只看 PCIe 枚举和 BAR 信息，不加载驱动

先插好 FPGA，确认 PCIe link up 后，在鲁班猫 Linux 执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
lspci -nn
lspci -vv -s 01:00.0
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

这组命令只查询信息，不会写 FPGA，不会启动 DMA。

需要记录：

```text
是否能看到 01:00.0 [0755:0755]
BAR0 地址和大小
BAR1 地址和大小
BAR2 地址和大小
lspci -vv 里 Kernel driver in use 是谁
```

### 第 2 组：加载安全驱动，只做 STOP，不启动 DMA

确认第 1 组能看到 `0755:0755` 后，再执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make
sudo rmmod pango_pci_driver 2>/dev/null || true
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
ls -l /dev/colorbar_pcie_rx
dmesg | tail -n 120
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 100
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
sudo rmmod colorbar_pcie_driver
```

注意：这一组 **不要加**：

```sh
allow_dma_start=1
```

预期结果：

```text
/dev/colorbar_pcie_rx 存在
--safe-stop 成功
--once 报 COLORBAR_IOC_START: Operation not permitted
系统不崩溃
```

`--once` 报错是正确现象，说明安全保护生效，没有真正启动 FPGA DMA。

### 第 3 组：FPGA 端同步观察的信号

执行第 2 组命令时，FPGA 端用 ILA/SignalTap 看：

```text
bar_hit
o_bar1_wr_en
axis_master_tvalid
axis_master_tdata
cmd_reg_addr
dma_stop_flag
dma_addr0
dma_addr1
dma_addr2
dma_addr3
rc_cfg_ep_flag
fram_start
dma_start
dma_cnt
```

重点判断：

```text
执行 --safe-stop 时，FPGA 是否看到 cmd_reg_addr = 0x130
执行 --safe-stop 时，dma_stop_flag 是否跳变
执行 --safe-stop 后，dma_addr0..3 是否全为 0
执行不带 allow_dma_start=1 的 --once 时，dma_start 是否仍为 0
```

### 第 4 组：如果 bar=1 没反应，安全测试 bar=0

只有在 FPGA 重新上电或重新加载 bitstream 后，才测试 bar=0：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=0 addr_byteswap=1 frame_wait_ms=100
dmesg | tail -n 120
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 100
sudo ./build/pcie_color_rx --once --output /tmp/frame_test_bar0.rgb565
sudo rmmod colorbar_pcie_driver
```

同样不要加：

```sh
allow_dma_start=1
```

判断：

```text
bar=1 下 --safe-stop 能让 FPGA dma_stop_flag 跳变 -> 控制 BAR 大概率是 BAR1
bar=0 下 --safe-stop 能让 FPGA dma_stop_flag 跳变 -> 控制 BAR 大概率是 BAR0
两个都没反应 -> FPGA 端抓取信号或 PCIe IP/TLP 连接需要继续查
```

### 当前禁止执行的命令

在 BAR、STOP、清地址、安全闸门没有确认前，禁止执行：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
```

也不要盲目尝试：

```text
bar=0/1 + allow_dma_start=1
addr_byteswap=0/1 + allow_dma_start=1
```

### 当前一句话

```text
现在只跑查询命令、安全 insmod、--safe-stop、以及预期失败的 --once。
不加 allow_dma_start=1，不做真正 DMA，不调彩条内容。
```


## Pango PCIe 测试平台 PDF 对当前 BAR 调试有什么帮助

参考文档：

```text
pango_pcie_dma_alloc/doc/PCIe测试平台应用指南_v1.0.pdf
```

当前仓库里文件名可能显示为乱码：

```text
pango_pcie_dma_alloc/doc/PCIe▓т╩╘╞╜╠и╙ж╙├╓╕─╧_v1.0.pdf
```

这个 PDF 对理解 BAR 和 PCIe 测试界面有帮助，但要分清楚它描述的是哪一层。

### 1. PDF 里的“配置空间寄存器列表”是什么

PDF 里的 Config Operation / 配置空间寄存器列表，说的是 PCIe 标准配置空间。

它可以帮助理解这些标准偏移：

```text
0x00  Vendor ID / Device ID
0x04  Command / Status
0x10  BAR0
0x14  BAR1
0x18  BAR2
```

这些是 PCIe 配置空间寄存器，不是彩条工程 `pcie_dma_ctrl.v` 里的控制寄存器。

所以不要把这两类偏移混在一起：

```text
PCIe 配置空间 0x10/0x14/0x18  -> BAR0/BAR1/BAR2 基地址寄存器
FPGA BAR 内部偏移 0x110/0x130 -> 彩条 DMA 地址/STOP 控制寄存器
```

### 2. PDF 能帮我们确认什么

PDF 的 Endpoint Status 界面对应 Linux 下这些命令：

```sh
lspci -vv -s 01:00.0
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

能确认：

```text
Vendor ID / Device ID
PCIe Link 是否 up
Link Speed / Link Width
BAR0 基地址和大小
BAR1 基地址和大小
BAR2 基地址和大小
MPS / MRRS
```

也就是说，它能帮 Linux 侧确认：

```text
这个 PCIe 设备有哪些 BAR，Linux 给每个 BAR 分配了什么地址。
```

但它不能单独证明：

```text
FPGA 彩条控制逻辑 pcie_dma_ctrl 到底接的是 BAR0 还是 BAR1。
```

### 3. PDF 的 PIO Test 和当前 safe-stop 的关系

PDF 里的 PIO Test 界面有：

```text
Bar Addr Switch：选择 BAR0/BAR1/BAR2...
Addr Offset(hex)：选择 BAR 内部偏移
Data(hex)：写入数据
```

这和我们当前要做的安全调试本质一样：

```text
选择 BAR1
写 offset 0x130
观察 FPGA 是否收到 STOP
```

但我们不建议直接用 GUI PIO 或 devmem 裸写，因为容易绕过当前驱动的安全保护。

当前推荐用安全驱动命令代替 PIO Test：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --safe-stop
```

它等价于安全执行：

```text
BAR1 + 0x130 写 0        -> STOP
BAR1 + 0x110 连续写 4 次 0 -> 清 dma_addr0..3
```

如果要测试 BAR0，则使用：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=0 addr_byteswap=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --safe-stop
```

仍然不要加：

```sh
allow_dma_start=1
```

### 4. PDF 对当前问题的帮助边界

有帮助的部分：

```text
理解 BAR 是 PCIe 设备暴露给主机的地址窗口
理解配置空间里 0x10/0x14/0x18 是 BAR 寄存器
理解 Endpoint Status 可以看 BAR 基地址和大小
理解 PIO Test 是“选 BAR + offset + data”的读写模型
```

不能直接解决的部分：

```text
不能直接告诉我们 pcie_dma_ctrl 实际接的是哪个 BAR
不能直接证明 Linux 写 0x130 时 FPGA dma_stop_flag 会跳
不能直接证明 0x110 地址字节序正确
不能替代 FPGA ILA/SignalTap 抓信号
```

### 5. 当前应该怎么结合 PDF 使用

推荐这样用：

```text
1. 用 PDF 理解 Endpoint Status 和 PIO Test 的概念。
2. 用 lspci/resource 查看 Linux 实际枚举到的 BAR 地址和大小。
3. 用当前安全驱动的 --safe-stop 代替 GUI PIO 写 STOP。
4. FPGA 端抓 cmd_reg_addr=0x130 和 dma_stop_flag，确认 BAR 是否正确。
```

当前不要做：

```text
不要用 Config Operation 随便写 PCIe 配置空间。
不要用 PIO Test 往 0x110 写非零 DMA 地址。
不要用 devmem 直接裸写 BAR + 0x110 非零值。
不要在未确认 BAR/STOP 前加 allow_dma_start=1。
```

一句话：

```text
PDF 能帮我们理解 BAR、配置空间和 PIO Test；
但最终确认控制 BAR，仍然要看 FPGA 是否收到 0x130 STOP 并触发 dma_stop_flag。
```


## 2026-7-19 当前安全调试记录

本节记录 2026-7-19 当前已经实际跑过的命令、看到的结果、判断结论、以及下一步只允许做的安全调试。后续调试继续按这种日期标题往下追加。

### 1. 已跑命令：PCIe 枚举和 BAR 信息

执行：

```sh
lspci -nn
```

输出中能看到：

```text
00:00.0 PCI bridge [0604]: Fuzhou Rockchip Electronics Co., Ltd Device [1d87:3566]
01:00.0 Memory controller [0580]: Device [0755:0755]
```

说明：

```text
PCIe Endpoint 已经被 RK3568 Linux 枚举到。
FPGA 设备 Vendor:Device = 0755:0755。
这说明当前不是“完全没有链路”的问题。
```

执行：

```sh
lspci -vv -s 01:00.0
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

当前 BAR 信息：

```text
BAR0: 0xf4200000 - 0xf4201fff，大小 8KB
BAR1: 0xf4204000 - 0xf4204fff，大小 4KB
BAR2: 0xf4202000 - 0xf4203fff，大小 8KB，64-bit
```

链路信息：

```text
Link Speed: 2.5GT/s
Link Width: x1
```

说明：

```text
Linux 侧 PCIe 枚举和 BAR 分配正常。
BAR1 是 4KB，更像控制寄存器空间。
当前 Linux 驱动优先使用 bar=1 是合理的，但还不能只靠 lspci 证明 FPGA 的 pcie_dma_ctrl 一定接在 BAR1。
```

### 2. 已跑命令：安全模式加载驱动

执行安全模式加载：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
```

随后检查：

```sh
ls -l /dev/colorbar_pcie_rx
dmesg | tail -n 120
```

当前看到：

```text
/dev/colorbar_pcie_rx 已创建
colorbar_pcie_rx 0000:01:00.0: sent DMA STOP + cleared dma_addr0..3 on BAR1
colorbar_pcie_rx 0000:01:00.0: colorbar PCIe RX probe ok, BAR1 len=0x1000
colorbar PCIe RX driver loaded
```

说明：

```text
Linux 驱动已经成功绑定 0755:0755。
当前使用 BAR1。
驱动 probe 阶段已经执行 safe-stop：写 0x130 STOP，并连续 4 次写 0 到 0x110。
```

注意：这只能说明 Linux 驱动侧已经执行写操作，还不能单独证明 FPGA 端已经收到。是否收到必须看 FPGA ILA/SignalTap。

### 3. 已跑命令：手动 safe-stop

执行：

```sh
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 100
```

用户态输出：

```text
sent safe stop to /dev/colorbar_pcie_rx
```

内核日志新增：

```text
colorbar_pcie_rx 0000:01:00.0: sent DMA STOP + cleared dma_addr0..3 on BAR1
```

说明：

```text
--safe-stop ioctl 路径正常。
Linux 驱动再次向 BAR1 + 0x130 写 STOP，并向 BAR1 + 0x110 连续写 4 次 0。
```

这一步的意义：

```text
给 FPGA 端提供一次明确的触发点，方便抓 cmd_reg_addr=0x130 和 dma_stop_flag。
这一步不会给 FPGA 写非零 DMA 地址，不会主动启动图像 DMA。
```

### 4. 已跑命令：尝试采一帧但被安全闸门拒绝

执行：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
```

实际输出：

```text
COLORBAR_IOC_START: Operation not permitted
```

这是预期正确结果。

说明：

```text
当前没有加 allow_dma_start=1。
驱动拒绝 COLORBAR_IOC_START。
Linux 没有向 FPGA 写新的非零 DMA 地址。
不会真正启动 FPGA DMA。
```

这一条不是错误，而是当前代码故意设置的保护结果。它说明上次导致系统崩溃的“直接启动 DMA”路径现在已经被挡住。

### 5. 当前结论

当前 Linux 侧安全检查结果：

```text
[x] PCIe Endpoint 0755:0755 已枚举
[x] BAR0/BAR1/BAR2 信息已读取
[x] 当前优先测试 BAR1
[x] colorbar_pcie_driver 能加载
[x] /dev/colorbar_pcie_rx 能创建
[x] probe 阶段 safe-stop 已执行
[x] 用户态 --safe-stop 已执行
[x] allow_dma_start=0 时 --once 被拒绝
[x] Linux 侧没有真正启动 DMA
```

当前判断：

```text
RK3568 Linux 侧目前能正常枚举 FPGA，也能加载驱动。
现在的关键问题不是 link up，也不是驱动是否能绑定。
真正还没确认的是：Linux 写 BAR1 + 0x130 STOP 时，FPGA 的 pcie_dma_ctrl 是否真的收到。
```

### 6. 当前还没有确认的内容

下面这些还没有通过 Linux 输出单独确认，必须 FPGA 端配合看信号：

```text
[ ] Linux 写 BAR1 + 0x130 时，FPGA 是否看到 cmd_reg_addr = 0x130
[ ] FPGA 的 dma_stop_flag 是否跳变
[ ] safe-stop 后 dma_addr0 是否为 0
[ ] safe-stop 后 dma_addr1 是否为 0
[ ] safe-stop 后 dma_addr2 是否为 0
[ ] safe-stop 后 dma_addr3 是否为 0
[ ] rc_cfg_ep_flag 是否为 0
[ ] dma_start 是否为 0
[ ] 不加 allow_dma_start=1 执行 --once 时，FPGA 是否没有 MWR 发出
```

### 7. 下一步只做 FPGA 端确认

下一步不要继续加 `allow_dma_start=1`，也不要真正采图。

请 FPGA 端用 ILA/SignalTap 抓这些信号：

```text
bar_hit
o_bar1_wr_en
axis_master_tvalid
axis_master_tdata
cmd_reg_addr
dma_stop_flag
dma_addr0
dma_addr1
dma_addr2
dma_addr3
rc_cfg_ep_flag
fram_start
dma_start
dma_cnt
```

Linux 侧为了给 FPGA 抓波形，可以重复执行：

```sh
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 50
```

FPGA 端预期看到：

```text
cmd_reg_addr = 0x130
dma_stop_flag 跳变
dma_addr0..3 全部为 0
rc_cfg_ep_flag = 0
dma_start = 0
```

如果 FPGA 端确认以上现象成立：

```text
BAR1 的 STOP/清地址链路基本确认正确。
可以进入下一阶段：地址字节序验证。
```

如果 FPGA 端没有看到 `0x130` 或 `dma_stop_flag` 不跳：

```text
说明 bar=1 可能没有进 pcie_dma_ctrl。
需要在 FPGA 重新上电或重新加载 bitstream 后，安全测试 bar=0。
仍然不能加 allow_dma_start=1。
```

### 8. 当前禁止事项

在 FPGA 端确认 BAR1 的 STOP 链路之前，禁止执行：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output /tmp/frame_0000.rgb565
```

当前一句话：

```text
2026-7-19 当前 Linux 侧安全闸门已经验证有效；下一步必须让 FPGA 端确认 BAR1 + 0x130 是否真的触发 dma_stop_flag。
```

### 9. 当前需要运行的命令

当前只运行下面这些命令，不进入真实 DMA 采图。

如果驱动还没加载：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
ls -l /dev/colorbar_pcie_rx
dmesg | tail -n 80
```

确认设备和 BAR：

```sh
lspci -nn
lspci -vv -s 01:00.0
cat /sys/bus/pci/devices/0000:01:00.0/resource
```

给 FPGA 抓波形用的安全触发命令：

```sh
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 50
```

确认安全闸门仍然生效：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
```

这一条当前应该输出：

```text
COLORBAR_IOC_START: Operation not permitted
```

如果输出这个，表示仍然没有真正启动 DMA，是安全状态。

当前不要运行：

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output /tmp/frame_0000.rgb565
```

## 2026-7-19 FPGA 端不好确认时的安全推进策略

### 1. 先回答结论

如果 FPGA 端暂时不好用 ILA/SignalTap 确认 BAR 和 STOP，那么不建议直接进入完整 DMA 采图。

可以继续推进，但下一步应该是“受控风险测试”，不是默认认为已经安全。

原因是：

```text
PCIe Endpoint 一旦作为 Bus Master 发起 DMA，它可以主动写 RK3568 主机内存。
如果 BAR 选错、DMA 地址字节序错、FPGA 内部旧地址没有清掉，Linux 侧可能被写坏内存，表现为系统卡死、桌面进不去、文件系统异常。
```

这类问题主要是软件/协议/地址配置风险，一般不是板子生命健康风险。正常电源、电平、转接线没有问题的情况下，它更容易伤的是 Linux 当前运行环境和文件系统，不是直接把 FPGA 或鲁班猫硬件打坏。

### 2. 当前已经做的 Linux 侧加固

2026-7-19 已对 `camara_host_computer/Colorbar_image/driver/colorbar_pcie_driver.c` 做了额外安全收紧：

```text
1. 驱动 probe 阶段不再一直打开 PCI Bus Master。
2. probe 阶段只做 safe-stop 和清 dma_addr0..3。
3. 真正 COLORBAR_IOC_START 时才临时 pci_set_master()。
4. COLORBAR_IOC_STOP、COLORBAR_IOC_FREE_BUFS、驱动 remove 时都会 pci_clear_master()。
5. allow_dma_start 默认仍然是 0，不显式打开就不会写非零 DMA 地址。
```

这次修改后的意义：

```text
加载驱动本身不会让 FPGA 长时间具备主动 DMA 写主机内存的能力。
即使后面做一次受控 DMA 测试，测试结束后也会尽快关闭 Bus Master。
```

但注意：

```text
这不是 100% 隔离。
只要 allow_dma_start=1 并执行 --once，就仍然存在 FPGA 写错主机内存的风险。
```

### 3. 现在可以安全做的下一步

先只验证新版驱动加载后 BusMaster 默认关闭。

执行：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
lspci -vv -s 01:00.0 | grep -E "Control:|Region|LnkSta"
dmesg | tail -n 80
```

预期重点看：

```text
Control: ... BusMaster- ...
colorbar PCIe RX probe ok, BAR1 len=0x1000, BusMaster disabled until START
```

如果看到 `BusMaster-`，说明驱动加载后没有给 FPGA 长期开 DMA 主动写权限。

然后继续跑安全停止：

```sh
sudo ./build/pcie_color_rx --safe-stop
dmesg | tail -n 50
```

预期看到：

```text
sent safe stop to /dev/colorbar_pcie_rx
colorbar_pcie_rx 0000:01:00.0: sent DMA STOP + cleared dma_addr0..3 on BAR1
```

再确认安全闸门：

```sh
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
```

当前仍然应该输出：

```text
COLORBAR_IOC_START: Operation not permitted
```

这一步还是安全路径，不会真正启动 DMA。

### 4. 如果一定要在 FPGA 端无法确认的情况下试一帧

这一步有风险，只能作为“受控风险测试”。建议满足这些条件后再做：

```text
[ ] 使用可重刷/可恢复的系统卡或已经备份好系统镜像
[ ] 不在 eMMC/SD 卡上保存关键文件
[ ] 输出文件写到 /tmp
[ ] 当前 lspci 能看到 BusMaster-
[ ] safe-stop 命令执行正常
[ ] 不循环采集，只采一帧
[ ] 采完立刻卸载驱动或 safe-stop
```

受控一帧测试命令：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output /tmp/frame_test.rgb565
sudo ./build/pcie_color_rx --safe-stop
sudo rmmod colorbar_pcie_driver
```

如果这一步再次导致系统异常，优先怀疑：

```text
1. BAR1 并不是 pcie_dma_ctrl 实际接收 0x110/0x130 的 BAR。
2. addr_byteswap=1 与 FPGA 实际解析不匹配。
3. FPGA 写入长度/地址递增方式和 Linux 分配的 buffer 不匹配。
4. FPGA 没有正确响应 STOP，导致持续 DMA。
5. FPGA 的 MWR 目标地址不是 Linux 打印出来的 coherent DMA 地址。
```

### 5. 当前建议

当前最稳的顺序是：

```text
第一步：先加载新版驱动，确认 lspci 里 BusMaster-。
第二步：继续只跑 --safe-stop 和 Operation not permitted 测试。
第三步：如果必须推进，再用备份系统卡做一次 allow_dma_start=1 的单帧测试。
第四步：不要循环测试，不要长时间开着驱动不卸载。
```

一句话：

```text
可以开始下一步，但下一步应先验证 BusMaster 默认关闭；真正 allow_dma_start=1 只能作为有备份前提下的一次性受控测试，不能当作已经完全安全。
```

