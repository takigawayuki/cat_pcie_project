# PCIe 调试安全指南与优化建议

本文基于 2026-07-19 系统镜像崩溃事件，对 `camara_host_computer/Colorbar_image` 工程的驱动代码、用户态程序、FPGA 代码进行全面分析，给出崩溃根因、代码修复方案和安全调试流程。

---

## 1. 崩溃根因分析

### 1.1 现象

```sh
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565
```

执行 `--once` 后系统镜像崩溃，重启后桌面无法进入。

### 1.2 直接原因：FPGA 向错误物理地址发起 DMA

FPGA 在 **驱动 probe 阶段（而非 START ioctl 阶段）** 就已经开始向主机内存写数据。具体链条：

```
旧 pango_pci_driver 曾配置 DMA 地址并启动过 DMA
  → rmmod 旧驱动时，pci_disable_device() 关闭了 PCIe 内存空间使能
  → 但 FPGA 内部寄存器 dma_addr0..dma_addr3 仍然保留旧地址
  → 新驱动 probe 调用 pci_enable_device_mem() + pci_set_master()
  → FPGA PCIe Endpoint 恢复活跃，检测到 4 个地址非零
  → rc_cfg_ep_flag 置 1（pcie_dma_ctrl.v:221）
  → 下一个 VS 边沿到达（HDMI 输入持续产生）
  → FPGA 自动向旧 DMA 地址发起 MWR 传输
  → 旧 DMA buffer 已被释放，物理内存已被内核/系统进程复用
  → 内存损坏 → 系统崩溃
```

### 1.3 FPGA 代码证据

`PCIE_DMA_test_color_MES50HP_X1/source/pcie_dma_ctrl.v` 关键逻辑：

```verilog
// 行 221：4 个地址全部非零 → 自动允许 DMA
if((dma_addr0 != 32'b0) && (dma_addr1 != 32'b0) &&
   (dma_addr2 != 32'b0) && (dma_addr3 != 32'b0))
    rc_cfg_ep_flag <= 'd1;

// 行 224：VS 边沿 → 开始一帧 DMA
if(!vs_in_d0 && vs_in_d1 && rc_cfg_ep_flag)
    fram_start <= 'd1;

// 行 145：只有收到 0x130 STOP 才会清除
if(!rstn || dma_stop_flag) begin
    dma_addr0 <= 'd0;
    dma_addr1 <= 'd0;
    dma_addr2 <= 'd0;
    dma_addr3 <= 'd0;
    dma_stop_flag <= 'd0;
end
```

**关键结论**：FPGA 没有显式的"START"命令寄存器。只要 4 个 DMA 地址全非零 + PCIe 链路存活 + VS 信号存在，FPGA 就会自动开始 DMA。**没有 ARM/MAGIC 保护、没有 START/ENABLE 寄存器、没有地址范围检查。**

### 1.4 为什么 `allow_dma_start=0` 没有保护住

`allow_dma_start` 只保护了 `colorbar_start_locked()` 不被调用，阻止驱动向 FPGA **写入新的** DMA 地址。但它不能阻止 FPGA 使用**旧的** DMA 地址 — 这些地址存储在内核无法访问的 FPGA 内部寄存器中。

`colorbar_probe()` 调用 `pci_enable_device_mem()` + `pci_set_master()` 后，FPGA 的 PCIe Endpoint 即恢复总线主控能力。如果旧地址仍在 FPGA 中，DMA 马上开始。

### 1.5 addr_byteswap 验证

FPGA 对收到的 32-bit 数据做了完整的字节反转（`pcie_dma_ctrl.v:161`）：

```verilog
dma_addr0 <= {axis_master_tdata_d0[7:0], axis_master_tdata_d0[15:8],
              axis_master_tdata_d0[23:16], axis_master_tdata_d0[31:24]};
```

推导：
- Linux `iowrite32(X, bar+0x110)` → PCIe 线序 LE → FPGA 收到 X
- FPGA 计算 `byte_reverse(X)` 作为 dma_addr0
- 若 Linux 直接写 `0xF4200000`，FPGA 得到 `0x000020F4`（**错误地址**）
- 若 Linux 先用 `swab32()` 反转再写 `0x000020F4`，FPGA 得到 `0xF4200000`（**正确地址**）

**结论：`addr_byteswap=1` 是正确的。** 用户本次加载时参数正确，不是崩溃原因。

### 1.6 BAR 选择分析

从 Pango PCIe IP 代码 `ipsl_pcie_dma_rx_top.v` 分析，MWr TLP 通过 AXIS master 接口直接送入 `pcie_dma_ctrl` 模块，`pcie_dma_ctrl` 只检查地址的低 12 位（BAR 偏移），不检查 BAR 号。因此 **bar=0 和 bar=1 都可能正确**，取决于 PCIe IP 核配置。

当前无法从代码确定正确 BAR。建议通过安全方式（写 STOP 寄存器后读回状态）验证。

---

## 2. 驱动代码修复方案

以下是具体的代码修改建议，按优先级排列。

### 2.1 【最高优先级】probe 阶段立即停止 FPGA DMA

**问题**：`colorbar_probe()` 没有向 FPGA 写 STOP 命令，FPGA 可能带着旧地址自动启动 DMA。

**修复**：在 `colorbar_probe()` 中，BAR 映射成功后立即写 0x130 停止 DMA：

```c
// 在 pci_iomap 成功后、pci_set_drvdata 之前插入：
// 立即停止 FPGA 可能正在进行的旧 DMA
iowrite32(0, cdev->bar + COLORBAR_REG_DMA_STOP);
wmb();
dev_info(&pdev->dev, "sent DMA STOP to clear any stale FPGA DMA config\n");
```

**完整修改位置**：`driver/colorbar_pcie_driver.c` 第 336 行附近。

### 2.2 【高优先级】probe 阶段验证 BAR 是否正确

**问题**：如果 BAR 选错，写 0x130 也不会到达 FPGA 控制寄存器，STOP 无效。

**修复方案 A（推荐）**：请求 FPGA 端增加 ADDR_ECHO 寄存器（见 5.2 节）。在此之前，可先对 BAR+0x130 写入，对 BAR+0x110 连续 4 次写 0，确保旧地址被清除：

```c
// 在 iowrite32 STOP 之后：
// 连续 4 次向 0x110 写 0，清除 FPGA 内部 dma_addr0..3
int i;
for (i = 0; i < 4; i++) {
    iowrite32(0, cdev->bar + COLORBAR_REG_DMA_ADDR);
}
wmb();
dev_info(&pdev->dev, "cleared FPGA dma_addr0..3\n");
```

注意：如果 BAR 选错，这 4 次写入可能到达错误的 FPGA 内部存储，但至少**不会**把有效的物理地址写入 FPGA DMA 地址寄存器，因此不会造成新的 DMA 危险。

**修复方案 B**：在 insmod 时尝试探测 BAR。先映射 bar=0，写一个已知值到 0x110，再从 FPGA 状态寄存器读回验证（需要 FPGA 有回读寄存器支持）。当前 FPGA 没有回读寄存器，此方案暂不可行。

### 2.3 【高优先级】STOP 时同时清除 DMA 地址

**问题**：`colorbar_stop_locked()` 只写 0x130，FPGA 收到后会清除 dma_addr0..3（pcie_dma_ctrl.v:145-150）。但如果 STOP 命令没到达 FPGA（BAR 错误或 PCIe 链路问题），地址不会清除。

**修复**：无代码修改（FPGA 逻辑已正确处理 STOP），但要加上注释说明：

```c
static void colorbar_stop_locked(struct colorbar_device *cdev)
{
    if (!cdev->bar)
        return;

    // 0x130 会让 FPGA 同时做三件事（pcie_dma_ctrl.v:145-150）：
    // 1. dma_stop_flag=1 → 停止 DMA 状态机
    // 2. dma_addr0..3 全部清零
    // 3. rc_cfg_ep_flag=0 → 禁止自动重启
    iowrite32(0, cdev->bar + COLORBAR_REG_DMA_STOP);
    wmb();
    cdev->started = false;
}
```

### 2.4 【中优先级】remove 时必须确保 STOP 已发送

**问题**：如果 `allow_dma_start=0`（从未启动），remove 仍会调用 `colorbar_stop_locked()`，这没问题。但如果 remove 前 BAR 已被 unmap（多线程/异常路径），可能跳过 STOP。

**当前代码已正确处理**：`colorbar_remove()` 在 `pci_iounmap()` 之前调用 `colorbar_stop_locked()`。确认无修改必要。

### 2.5 【中优先级】增加模块参数 bar_auto_detect

建议新增参数让驱动自动尝试探测 BAR：

```c
static bool bar_auto_detect;
module_param(bar_auto_detect, bool, 0644);
MODULE_PARM_DESC(bar_auto_detect, "auto-detect BAR by probing BAR0 then BAR1 (safe mode only)");
```

自动检测逻辑：在 `allow_dma_start=0` 的前提下，分别映射 BAR0 和 BAR1，各写一次 0x130 和 4 次 0x110(全零)，清除所有可能残留的旧 DMA 配置。这样无论 FPGA 控制寄存器在 BAR0 还是 BAR1，旧 DMA 地址都会被清除。

### 2.6 【低优先级】driver/colorbar_pcie_driver.c 完整修改

以下是 `colorbar_probe()` 建议的完整修改（关键部分）：

```c
static int colorbar_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct colorbar_device *cdev;
    int ret;
    int i;

    ret = pci_enable_device_mem(pdev);
    if (ret)
        return ret;

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
    if (ret) {
        dev_err(&pdev->dev, "failed to set 32-bit DMA mask: %d\n", ret);
        goto disable_device;
    }

    pci_set_master(pdev);

    if (bar < 0 || bar > 5) {
        ret = -EINVAL;
        goto disable_device;
    }

    ret = pci_request_region(pdev, bar, COLORBAR_DEVICE_NAME);
    if (ret) {
        dev_err(&pdev->dev, "failed to request BAR%d: %d\n", bar, ret);
        goto disable_device;
    }

    cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
    if (!cdev) {
        ret = -ENOMEM;
        goto release_region;
    }

    cdev->pdev = pdev;
    cdev->bar_index = bar;
    cdev->bar_len = pci_resource_len(pdev, bar);
    mutex_init(&cdev->lock);
    cdev->bar = pci_iomap(pdev, bar, 0);
    if (!cdev->bar) {
        ret = -ENOMEM;
        goto free_cdev;
    }

    // ===== 新增：立即停止 FPGA 可能的旧 DMA =====
    // 1. 先写 STOP 寄存器
    iowrite32(0, cdev->bar + COLORBAR_REG_DMA_STOP);
    wmb();
    // 2. 连续 4 次向 DMA_ADDR 寄存器写 0，清除旧地址
    for (i = 0; i < 4; i++) {
        iowrite32(0, cdev->bar + COLORBAR_REG_DMA_ADDR);
    }
    wmb();
    dev_info(&pdev->dev,
        "sent DMA STOP + cleared dma_addr0..3 on BAR%d to prevent stale DMA\n",
        bar);
    // ===== 新增结束 =====

    pci_set_drvdata(pdev, cdev);
    g_cdev = cdev;

    dev_info(&pdev->dev, "colorbar PCIe RX probe ok, BAR%d len=0x%pa\n",
         bar, &cdev->bar_len);
    return 0;

free_cdev:
    kfree(cdev);
release_region:
    pci_release_region(pdev, bar);
disable_device:
    pci_disable_device(pdev);
    return ret;
}
```

---

## 3. 用户态程序改进建议

### 3.1 `pcie_color_rx.c` 当前无安全问题

`capture_once()` 中，如果 `COLORBAR_IOC_START` 返回 `-EPERM`（因为 `allow_dma_start=0`），程序会：
1. 打印 "Operation not permitted"
2. 跳转到 `out_unmap` 标签
3. 正常 munmap + free bufs 后退出

**没有代码路径会在 `allow_dma_start=0` 时启动 DMA。** 用户态程序不需要修改。

### 3.2 建议新增 --safe-stop 命令

新增一个命令，只做 STOP（写 0x130）+ 清除地址（4 次写 0x110），不做任何 DMA 启动。用于在 insmod 后就安全停止任何可能的旧 DMA：

```c
// 新增 ioctl: COLORBAR_IOC_SAFE_STOP
// 驱动端：写 0x130，再连续 4 次写 0 到 0x110
// 用户态：./pcie_color_rx --safe-stop
```

这可以进一步降低风险：即使 probe 阶段的 STOP 没有到达 FPGA（BAR 选错），用户也可以快速尝试另一个 BAR。

### 3.3 建议增加 --probe-bar 命令

```sh
# 安全探测：分别用 bar=0 和 bar=1 加载驱动，找出正确 BAR
./pcie_color_rx --probe-bar
```

实现思路：
1. 不加载驱动，直接用 UIO 或 /sys/bus/pci/devices/ 下的 resource 文件做 PIO 测试
2. 或者加载驱动但不做任何 BAR 写入，通过读取 PCIe 配置空间获取 BAR 信息

---

## 4. 安全调试 SOP（标准操作流程）

### 阶段 0：系统恢复

如果系统已崩溃：

```text
1. 鲁班猫和 FPGA 都断电
2. 拔掉 FPGA/转接板（物理隔离，确保 FPGA 不再发 DMA）
3. 用另一台 Linux 机器对 SD 卡 rootfs 分区做 fsck
   sudo fsck.ext4 -f /dev/sdX3
   注意：/dev/sdX3 必须换成实际分区，不确定不要执行
4. 或直接重刷系统镜像
5. 重刷后先不插 FPGA，启动一次确认桌面正常
6. 重新插入 FPGA 前，确保 FPGA 内部 bitstream 不含旧 DMA 地址
   （给 FPGA 重新上电/重新加载 bitstream 即可清除）
```

### 阶段 1：安全检查（不启动 DMA）

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
make

# 确认 PCIe 设备存在
lspci -nn
# 预期看到: 01:00.0 ... [0755:0755]

# 卸载旧驱动
sudo rmmod pango_pci_driver 2>/dev/null || true

# 安全模式加载（allow_dma_start 默认=0，不会写 DMA 地址）
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 frame_wait_ms=100

# 检查 probe 日志
dmesg | tail -20
# 应该看到 "sent DMA STOP + cleared dma_addr0..3" 的日志
# 这表示驱动已尝试清除 FPGA 旧 DMA 配置

# 确认设备节点
ls -l /dev/colorbar_pcie_rx

# 尝试 --once（应该失败，因为 allow_dma_start=0）
sudo ./build/pcie_color_rx --once --output frame_test.rgb565
# 预期输出: COLORBAR_IOC_START: Operation not permitted
# 这证明保护生效

# 卸载
sudo rmmod colorbar_pcie_driver
```

### 阶段 2：与 FPGA 端联合验证 BAR 和字节序

**这一步必须在 FPGA 端配合下完成，不能靠盲试。**

```text
前提：FPGA 端准备好 ILA/SignalTap，抓取以下信号：
  - tlp_fmt, tlp_type, mwr_addr, cmd_reg_addr（pcie_dma_ctrl.v:66-70）
  - dma_addr0, dma_addr1, dma_addr2, dma_addr3（pcie_dma_ctrl.v:77-80）
  - dma_stop_flag, rc_cfg_ep_flag（pcie_dma_ctrl.v:82,72）
```

验证步骤：

```sh
# 步骤 A：安全加载（不自动清除 STOP，以便 FPGA 抓取波形）
# 注意：如果已经应用了 2.1 的修复，probe 会自动 STOP + 清除地址。
# 以下验证需要临时注释掉 probe 中的 STOP 逻辑，仅用于这一次验证。
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=0 frame_wait_ms=100

# 查看 dmesg，记录 DMA 地址（后面用于对比）
dmesg | grep -E "buffer[0-3]|dma="

# 步骤 B：通过 /sys 手动做 PIO 读写测试
# 找到 BAR 物理地址
cat /sys/bus/pci/devices/0000:01:00.0/resource
# 输出类似：
# 0x00000000f4200000 0x00000000f4201fff 0x0000000000040200
# 0x00000000f4204000 0x00000000f4204fff 0x0000000000040200
# 0x00000000f4202000 0x00000000f4203fff 0x0000000000040200

# 步骤 C：用 devmem 或自定义小程序做单次 PIO 写测试
# 向 BAR+0x130 写 STOP 命令，FPGA 端抓 dma_stop_flag 是否置 1
# 向 BAR+0x110 写已知测试值（如 0xA5A5A5A5），FPGA 端抓 dma_addr0
# 对比后确认 BAR 和 addr_byteswap

# 步骤 D：确认后，再启用 DMA 采集
sudo rmmod colorbar_pcie_driver

# 如果 FPGA 确认 addr_byteswap=1 正确（推荐）
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=1 allow_dma_start=1 frame_wait_ms=100

# 如果 FPGA 确认 addr_byteswap=0 正确
sudo insmod driver/colorbar_pcie_driver.ko bar=1 addr_byteswap=0 allow_dma_start=1 frame_wait_ms=100
```

### 阶段 3：正式采集一帧

```sh
# 在确认 BAR 和字节序后，正式采集
sudo ./build/pcie_color_rx --once --output frame_0000.rgb565

# 检查文件大小
ls -l frame_0000.rgb565
# 预期: 4147200 bytes

# 校验彩条数据
./build/pcie_color_rx --validate frame_0000.rgb565
# 预期: 8 个采样点全部 OK

# 测试完毕，卸载驱动
sudo rmmod colorbar_pcie_driver
```

### 阶段 4：如果数据不对，安全排查

```text
不要反复改 bar 和 addr_byteswap 盲试！
每次盲试都在冒险：如果 BAR 正确但 addr_byteswap 错误，
FPGA 会向错误物理地址写数据，可能再次损坏系统。

正确做法：
1. 回到阶段 2，用 FPGA ILA 抓波形对比
2. 先确认 STOP 能否到达 FPGA（写 0x130，抓 dma_stop_flag）
3. 再确认地址写入是否正确（写已知测试值，抓 dma_addr0）
4. 完全确认后才 allow_dma_start=1

如果文件全 0：
- FPGA 可能没有真正写内存
- BAR 可能选错
- FPGA 内部视频数据路径可能未通

如果文件有数据但内容错误：
- 检查 addr_byteswap 是否正确
- 检查 DMA 地址是否 32-bit 范围内
- 检查 buffer 是否被错误覆盖（4 buffer 轮转逻辑）
- 检查 frame_wait_ms 是否够长（尝试 200ms 或 500ms）

如果只有部分行有数据：
- FPGA 可能发完部分 TLP 后停止
- 检查 dma_cnt 和 DMA_TRAN_TIMES 计数
```

---

## 5. 建议 FPGA 端增加的硬件保护

当前 FPGA 逻辑过于"信任"软件写入的地址，没有任何保护层。建议增加以下寄存器：

### 5.1 ARM/MAGIC 寄存器

| 偏移 | 名称 | 说明 |
|------|------|------|
| 0x100 | DMA_ARM | 必须先写 `0xA55A5AA5` 才允许后续 0x110 地址写入生效 |

**作用**：即使软件误写了 0x110，只要没先写 MAGIC，地址就不会被 FPGA 接受。相当于硬件"使能"开关。

### 5.2 ADDR_ECHO 寄存器（强烈建议）

| 偏移 | 名称 | 说明 |
|------|------|------|
| 0x114 | ADDR0_ECHO | Linux 可读回 FPGA 实际保存的 dma_addr0 |
| 0x118 | ADDR1_ECHO | 同上，dma_addr1 |
| 0x11C | ADDR2_ECHO | 同上，dma_addr2 |
| 0x120 | ADDR3_ECHO | 同上，dma_addr3 |

**作用**：Linux 写完 4 个地址后，可以读回验证 FPGA 实际保存的值。这是验证 BAR 和 addr_byteswap 最直接的方式，**不需要 ILA/SignalTap**。

实现方式：FPGA 端在收到 MRd TLP 时返回对应 dma_addr 寄存器值。

### 5.3 STATUS 寄存器

| 偏移 | 名称 | 说明 |
|------|------|------|
| 0x170 | DMA_STATUS | [0] dma_busy, [1] frame_done, [2] fifo_overflow, [3] addr_error |
| 0x174 | FRAME_COUNTER | 已完成帧计数 |
| 0x178 | CURRENT_PAGE | 当前写入 buffer 页号 (0-3) |

**作用**：Linux 可以轮询 `frame_done` 位来判断一帧是否完成，替代固定 `frame_wait_ms` 延时。

### 5.4 START 寄存器

| 偏移 | 名称 | 说明 |
|------|------|------|
| 0x104 | DMA_START | 写 1 启动 DMA（替代当前"4 地址非零即自动启动"） |

**作用**：彻底分离"配置地址"和"启动 DMA"两个动作。减少误启动风险。

### 5.5 LEN 寄存器

| 偏移 | 名称 | 说明 |
|------|------|------|
| 0x108 | DMA_LEN | 限制每帧最多传输字节数，超过则自动停止并报错 |

**作用**：如果地址配置错误，最多只会写 `DMA_LEN` 字节到错误地址，而不是无限写。

### 5.6 STOP 后自动清地址

当前 FPGA 代码（`pcie_dma_ctrl.v:145`）已经做到 STOP 清零地址。确认这个逻辑在综合后仍有效。

---

## 6. DMA_TRAN_TIMES 问题

### 6.1 当前值

```verilog
parameter DMA_TRAN_TIMES = 'd64801;  // pcie_dma_ctrl.v:56
```

一帧 RGB565 1080p = 1920 × 1080 × 2 = 4,147,200 bytes
每个 TLP = 64 bytes
理论 TLP 数 = 4,147,200 / 64 = 64,800

**FPGA 实际发 64,801 个 64B TLP，比一帧多 64 bytes。**

### 6.2 影响

- 第 64801 个 TLP（最后一个）的内容不是彩条数据，而是帧标记 `{16{fram_cnt}}`（pcie_dma_ctrl.v:339）
- 所以实际写入 host 内存的数据是 64,801 × 64 = 4,147,264 bytes
- 比有效图像数据（4,147,200 bytes）多 64 bytes

### 6.3 Linux 端应对

当前驱动已正确处理：
- `COLORBAR_BUFFER_SIZE = PAGE_ALIGN(4,147,200 + 64) = PAGE_ALIGN(4,147,264) = 4,149,248 bytes`
- 用户态保存时只取前 4,147,200 bytes（`COLORBAR_FRAME_SIZE`）
- 多出的 64 bytes 留在 buffer 尾部，不会覆盖其他内存

但应与 FPGA 端确认：`DMA_TRAN_TIMES = 64801` 是**有意**的（帧标记包），还是 **off-by-one bug**（应为 64800）。

如果想确认，可以用 `--validate` 检查 frame_0000.rgb565 的最后 64 bytes 是否是 `0x0101...`（帧标记）。

---

## 7. 字节序完整分析

### 7.1 数据路径字节序

FPGA 不仅在地址上做了字节反转，在 TLP 数据载荷上也做了字节重排（`pcie_dma_ctrl.v:332-336`）：

```verilog
r_axis_s_tdata <= {
    pcie_dma_data[103:96], pcie_dma_data[111:104], ...
    pcie_dma_data[71:64],  pcie_dma_data[79:72],   ...
    pcie_dma_data[39:32],  pcie_dma_data[47:40],   ...
    pcie_dma_data[7:0],    pcie_dma_data[15:8],    ...
};
```

这是对 128-bit 数据块内**每字节**位置的重新排列。

### 7.2 RGB565 像素字节序

如果收到的 RGB565 像素字节序不对（高字节和低字节交换），可能的原因：
- FPGA 内部 HDMI 输入到 FIFO 的数据排列
- `pcie_dma_data` 的位域映射（`pcie_data_out = {R[7:3], G[7:2], B[7:3]}`）
- TLP 数据载荷的字节重排

**验证方法**：收到第一帧后，检查采样点的实际值。如果每个 16-bit 像素的字节交换了，则 Linux 端做一次 `swab16()`。如果 16-bit 像素正确但 128-bit 块内排列异常，则需要更详细的数据对比。

### 7.3 建议：让 FPGA 发已知测试图案

与其猜测字节序，不如让 FPGA 第一版先发已知图案：
- 第一行全白（0xFFFF）
- 第二行全红（0xF800）
- 第三行全绿（0x07E0）
- 第四行全蓝（0x001F）

这样 Linux 端收到后一目了然，不需要猜测字节序。

---

## 8. 硬件安全注意事项

### 8.1 不会损坏硬件，但会损坏系统

FPGA MWR 写错物理地址会损坏 Linux 内核内存和文件系统，**但不会烧坏鲁班猫或 FPGA 芯片**。

真正威胁硬件寿命的因素是：
- 热插拔 PCIe 转接板
- 供电电压不匹配
- PERST#、REFCLK、GND 接触不良
- FPGA IO 电平标准与转接板不匹配
- 长时间短路、过流、过热

### 8.2 操作守则

```text
1. 不热插拔（先断电，再插拔转接板和 FPGA）
2. 系统恢复前先拔掉 FPGA，避免旧 bitstream 继续 DMA
3. 不确认 BAR 和 addr_byteswap 前不用 allow_dma_start=1
4. 不靠 bar=0/1、addr_byteswap=0/1 盲试
5. 每次重新加载 FPGA bitstream 都会清除内部 DMA 地址
6. 如果怀疑 FPGA 正在 DMA，最快停止方法是：
   - 给 FPGA 断电
   - 或写 0x130 STOP 寄存器（需要 BAR 正确）
   - 或拔掉 HDMI 输入（停止 VS 信号，DMA 会停在一帧末尾）
```

---

## 9. 已确认的关键信息汇总

| 项目 | 确认值 | 来源 |
|------|--------|------|
| FPGA Vendor:Device ID | 0755:0755 | lspci / pcie_dma_ctrl.v |
| 控制寄存器 BAR 偏移 | 0x110 (DMA地址), 0x130 (STOP) | pcie_dma_ctrl.v:40-41 |
| DMA 地址数量 | 4 个，连续写 4 次 0x110 | pcie_dma_ctrl.v:157-178 |
| DMA 地址位宽 | 32-bit | pcie_dma_ctrl.v:77-80 |
| FPGA 字节序处理 | 32-bit 字节反转 | pcie_dma_ctrl.v:161 |
| addr_byteswap 正确值 | 1（Linux 先 swab32 再写） | 数学推导 |
| DMA 启动条件 | 4 地址非零 + VS 边沿（无显式 START） | pcie_dma_ctrl.v:221,224 |
| DMA 停止方式 | 写 0x130，FPGA 清零地址+状态 | pcie_dma_ctrl.v:145-151,181-182 |
| 每帧 TLP 数 | 64,801（比理论多 1 个标记包） | pcie_dma_ctrl.v:56 |
| 单帧大小 | 4,147,200 bytes 有效 + 64 bytes 标记 | 计算 |
| Buffer 大小 | 4,149,248 bytes (PAGE_ALIGN) | colorbar_pcie_driver.h |
| 图像格式 | 1920×1080 RGB565 | 多处确认 |
| PCIe 链路 | Gen2 x1, RK3568 RC ↔ Pango FPGA EP | dmesg |

---

## 10. 总结：当前最推荐的行动顺序

```text
[已完成] 1. 分析崩溃根因 → 旧 DMA 地址残留
[已完成] 2. probe 阶段增加 colorbar_hw_safe_stop() 保护
[已完成] 3. 新增 COLORBAR_IOC_SAFE_STOP ioctl + --safe-stop 命令
[已完成] 4. addr_byteswap 默认值改为 true（匹配 FPGA 字节反转逻辑）
[待执行] 5. 系统恢复（重刷或 fsck SD 卡）
[待执行] 6. 重新上电 FPGA（清除旧 bitstream 中的 DMA 地址）
[待执行] 7. 按阶段 1 做安全加载测试（allow_dma_start=0）
[待执行] 8. FPGA 端用 ILA 抓波形，确认正确 BAR
[待执行] 9. 确认后 allow_dma_start=1 正式采集一帧
[待执行] 10. --validate 校验彩条数据
[建议]   11. 请求 FPGA 端增加 ADDR_ECHO/ARM/STATUS 寄存器
[建议]   12. 用 frame_done 状态寄存器替代固定 frame_wait_ms
[后续]   13. 连续帧采集和显示
```

---

## 11. 当前代码安全性评估（2026-07-19 更新）

本章对当前版本的完整代码做逐层安全性审计，回答一个问题：

> **"以当前代码状态，执行 `insmod` + `--once` 还会不会崩系统镜像？"**

### 11.1 攻击面模型

FPGA 向主机内存发起破坏性 DMA 写入，需要**三个条件同时满足**：

| 条件 | 描述 | FPGA 代码位置 |
|------|------|---------------|
| **A** | FPGA 内部 `dma_addr0..3` 不全为零 | `pcie_dma_ctrl.v:221` |
| **B** | PCIe Endpoint 处于活跃状态（`pci_enable_device_mem` + `pci_set_master` 已调用） | - |
| **C** | VS 信号产生边沿翻转 → 触发 `fram_start` → DMA 状态机启动 | `pcie_dma_ctrl.v:224` |

**三个条件缺一不可。** 任一条件不满足，FPGA 不会发起 MWR TLP。

Linux 端的任务就是确保条件 A 不被满足（即 FPGA 内部的 4 个 DMA 地址始终为零），或至少有一个为 0。

### 11.2 当前代码的保护层（已实现）

以下按时间顺序排列，从驱动加载到卸载：

#### 保护 L1：`colorbar_hw_safe_stop()` 函数

```c
// driver/colorbar_pcie_driver.c:125-142
static void colorbar_hw_safe_stop(struct colorbar_device *cdev)
{
    int i;
    if (!cdev->bar) return;
    iowrite32(0, cdev->bar + COLORBAR_REG_DMA_STOP);   // 写 0x130 STOP
    wmb();
    for (i = 0; i < COLORBAR_BUFFER_COUNT; i++)
        iowrite32(0, cdev->bar + COLORBAR_REG_DMA_ADDR); // 4 次写 0x110 = 0
    wmb();
    cdev->started = false;
}
```

**FPGA 响应**：
- 收到 0x130 STOP → `dma_stop_flag=1` → `dma_addr0..3=0` + `rc_cfg_ep_flag=0` + `fram_start=0` + `dma_start=0` + `dma_cnt=0`
- 后续 4 次 0x110 写 0 → 无效果（地址已经是 0），但提供冗余保护

**安全效果**：彻底清除 FPGA 内部的条件 A。

#### 保护 L2：probe 阶段调用 `colorbar_hw_safe_stop(cdev)`

```c
// driver/colorbar_pcie_driver.c:356-360
/*
 * 安全保护：立即停止 FPGA 可能正在进行的旧 DMA。
 * FPGA 内部 dma_addr0..3 不会因 Linux rmmod 自动清零。
 */
colorbar_hw_safe_stop(cdev);
```

**在 `pci_iomap` 之后、`pci_set_drvdata` 之前**立即执行。这是 probe 期间最早可能的硬件操作。

**安全效果**：如果 BAR 正确，条件 A 在 `pci_set_master` 之后的几个微秒内就被清零，远在第一个 VS 边沿到达之前。解决了上次崩溃的直接根因。

#### 保护 L3：`addr_byteswap = true`（默认值）

```c
// driver/colorbar_pcie_driver.c:43-44
static bool addr_byteswap = true;
```

**推导**：`addr_byteswap=1` 使 `colorbar_write_dma_addr()` 调用 `swab32()` 后再 `iowrite32()`。经过 FPGA 的 `byte_reverse()` 后，FPGA 内部 dma_addr 与 Linux dma_addr 一致。这是数学上正确的值。

**安全效果**：如果将来 `allow_dma_start=1` 时启动 DMA，写入 FPGA 的地址一定是正确的物理地址，不会因字节序错误而产生无效地址。无效地址虽然也会被 FPGA 当作目标地址写数据，但至少比"看似有效实则偏移的地址"更容易被注意到（全零 buffer vs 部分写乱的内存）。

#### 保护 L4：`allow_dma_start = false`（默认值）

```c
// driver/colorbar_pcie_driver.c:47-48
static bool allow_dma_start;
```

`colorbar_start_locked()` 在 `allow_dma_start=false` 时返回 `-EPERM`，**不调用** `colorbar_write_dma_addr()`，不向 0x110 写入任何值。

**安全效果**：保护 L2 将 FPGA 条件 A 清零后，L4 确保用户不会意外重新写入非零地址。FPGA 保持条件 A = 0，DMA 不会启动。

#### 保护 L5：`COLORBAR_IOC_SAFE_STOP` ioctl + `--safe-stop` 命令

```c
// driver/colorbar_pcie_driver.c:244-248
case COLORBAR_IOC_SAFE_STOP:
    mutex_lock(&cdev->lock);
    colorbar_hw_safe_stop(cdev);
    mutex_unlock(&cdev->lock);
    return 0;
```

```c
// pcie_color_rx.c:282-283, 304-305
} else if (!strcmp(argv[i], "--safe-stop")) {
    safe_stop = true;
...
if (safe_stop) {
    return safe_stop_device(device) == 0 ? 0 : 1;
}
```

**安全效果**：用户可随时执行 `sudo ./build/pcie_color_rx --safe-stop` 来显式清除 FPGA DMA 配置。如果怀疑 probe 阶段的 L2 因 BAR 不对而未生效，可以通过 rmmod + 换 bar 参数重新 insmod 来覆盖。

#### 保护 L6：remove 阶段调用 `colorbar_stop_locked()`

```c
// driver/colorbar_pcie_driver.c:385-388
mutex_lock(&cdev->lock);
colorbar_stop_locked(cdev);       // → colorbar_hw_safe_stop(cdev)
colorbar_free_buffers_locked(cdev);
mutex_unlock(&cdev->lock);
// 然后才 pci_iounmap → pci_release_region → pci_disable_device
```

**安全效果**：在 rmmod 时，BAR 尚未 unmap，PCIe 链路尚存活，STOP 命令能送达 FPGA。清除 FPGA 内部地址后，下一次 insmod 时 FPGA 内部状态是干净的（即使不同驱动实例之间没有重新上电 FPGA）。

#### 保护 L7：32-bit DMA mask

```c
// driver/colorbar_pcie_driver.c:321
ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
```

**安全效果**：确保 `dma_alloc_coherent` 分配的地址在 32-bit 范围内，FPGA 的 32-bit DMA 地址寄存器可以完整表示，不会有高位截断带来的地址错误。

#### 保护 L8：Buffer 大小有余量

```
COLORBAR_FRAME_SIZE  = 4,147,200 bytes
COLORBAR_MARK_SIZE   = 64 bytes
COLORBAR_BUFFER_SIZE = PAGE_ALIGN(4,147,264) = 4,149,248 bytes
FPGA 最大写入量      = 64,801 × 64 = 4,147,264 bytes
余量                 = 4,149,248 - 4,147,264 = 1,984 bytes
```

**安全效果**：即使 FPGA 因为 `DMA_TRAN_TIMES=64801` 而多写 64 bytes，也不会越界覆写内存。

### 11.3 残余风险分析

逐一排查在当前代码状态下，还有什么情况会导致 FPGA 获得非零 DMA 地址：

| 风险场景 | 条件 A（地址非零） | 条件 B（EP 活跃） | 条件 C（VS 信号） | 危险？ |
|---------|-------------------|------------------|------------------|--------|
| FPGA 刚上电/刚加载 bitstream | ❌ 地址全零 | ✅ insmod 后活跃 | ✅ | **否** |
| 上次 rmmod 正常（L6 已清地址），再次 insmod | ❌ 地址全零 | ✅ | ✅ | **否** |
| **上次 rmmod 异常**（内核 panic/强行断电），旧地址残留，当前 BAR 正确 | ✅ 旧地址非零 | ✅ | ✅ | **否** ← L2 清除 |
| **上次 rmmod 异常**，旧地址残留，**当前 BAR 错误** | ✅ 旧地址非零 | ✅ | ✅ | **⚠️ 是** |
| 用户显式 `allow_dma_start=1`，但 addr_byteswap 被人错误覆盖 | ✅ 新地址非零 | ✅ | ✅ | **⚠️ 是** |

**关键结论**：

当前代码状态下，**唯一的残余风险路径是 BAR 选错了**。如果 `bar=1` 不是 FPGA 控制寄存器所在的 BAR：

1. L2（probe 阶段的 `colorbar_hw_safe_stop`）写入错误的 BAR 地址空间 → FPGA 收不到 STOP + 清地址命令 → 条件 A 不变（旧地址仍非零）
2. 条件 B 满足（`pci_enable_device_mem` + `pci_set_master` 已调用）
3. 条件 C 满足（HDMI 信号源持续产生 VS）
4. → FPGA 用旧地址自动发起 MWR → 系统崩溃

**反之，如果 BAR 正确**（`bar=1` 确实是 FPGA 控制寄存器所在 BAR）：

1. L2 生效 → 条件 A 被清零
2. L4 生效 → 用户不加 `allow_dma_start=1` 无法写入新地址
3. 三个条件无法同时满足 → **绝对安全**
4. 即使加了 `allow_dma_start=1`，`addr_byteswap=1`（默认）确保写入的地址是正确的，数据写入正确的 DMA buffer → **安全**

### 11.4 BAR 正确的概率分析

从 Pango PCIe IP 代码 `ipsl_pcie_dma_rx_top.v:246-252` 分析：

```verilog
// bar1 interface: only active when bar_hit == 2'b1 (BAR1)
if(bar_hit == 2'b1 && (DEVICE_TYPE == 3'b000 || DEVICE_TYPE == 3'b001))
    o_bar1_wr_en = 1'b1;
```

FPGA 的 PCIe IP 核配置了 BAR1 作为 MWr 接收接口。Linux 通过 `pci_iomap(pdev, bar_index, 0)` 映射的 BAR 物理地址，需要与 FPGA IP 核中 `bar_hit` 信号对应的 BAR 号一致。

**无法仅从代码 100% 确定正确 BAR。** 需要以下任一方式确认：
- 用 FPGA ILA/SignalTap 抓取 `bar_hit` 信号和 `o_bar1_wr_en`
- 在已知 BAR 正确的前提下，FPGA 端确认 `dma_stop_flag` 随 Linux 写 0x130 而变化
- 用 `lspci -vv -s 01:00.0` 查看 BAR 大小，与 FPGA 工程预期的寄存器空间做对比
- 请求 FPGA 端增加 ADDR_ECHO 可读寄存器（Linux 写地址后读回验证）

### 11.5 最终结论

> **当前代码在 BAR 正确的前提下已做到足够安全，执行 `insmod` + `--once` 不会崩系统镜像。**

具体前提条件：

```text
1. [必须] FPGA 重新上电或重新加载 bitstream → 确保旧 DMA 地址为零
2. [必须] BAR 参数正确（bar=1 还是 bar=0 需经 ILA/测试确认）
3. [已满足] addr_byteswap=1 是默认值（正确匹配 FPGA 字节反转）
4. [已满足] allow_dma_start 默认=0（不被意外写地址）
5. [已满足] probe 阶段 colorbar_hw_safe_stop() 清除残留
6. [已满足] remove 阶段 colorbar_stop_locked() 清除地址
```

如果条件 1 和 2 都满足，**当前代码就是防线完备的**。如果条件 2 不确定（BAR 未知），则**在确认 BAR 之前绝对不要加 `allow_dma_start=1`**，并且在 FPGA 重新上电之后（条件 1 满足），即使 BAR 错了也不会崩溃（因为 FPGA 内部没有旧地址残留）。

**BAR 确认的推荐方案**（不需要 ILA）：

给 FPGA 重新上电后立即 `insmod bar=1`，执行 `--safe-stop`，然后正常执行不带 `allow_dma_start=1` 的 `--once`。如果 `--once` 能在 `COLORBAR_IOC_START` 处返回 `-EPERM`（保护生效），说明设备节点和 ioctl 路径正常工作。这本身不验证 BAR，但确认了"没有 allow_dma_start=1 就不会写地址"的安全路径。

验证 BAR 正确性的唯一无硬件辅助方式：比较 `dmesg` 中 `bar_len` 和 `lspci -vv` 中对应 BAR 的大小是否匹配 FPGA 预期（几千字节的寄存器空间 vs 几 MB 的 DMA buffer 空间）。控制寄存器所在 BAR 通常较小（如 4KB），而 DMA buffer 所在的 BAR 较大。