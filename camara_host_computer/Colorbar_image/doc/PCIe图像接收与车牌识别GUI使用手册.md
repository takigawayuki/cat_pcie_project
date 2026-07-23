# PCIe图像接收与车牌识别GUI使用手册

本文档只说明 `camara_host_computer/Colorbar_image` 当前 GTK 图形界面的使用方法。目标是让鲁班猫 RK3568 通过 PCIe 接收 FPGA 传来的 1920x1080 RGB565 图像，并在界面中做 YOLO 车牌定位和 LPRNet 车牌识别。

## 1. 当前界面能做什么

当前 GUI 程序是：

```sh
/home/cat/cat_pcie_project/camara_host_computer/Colorbar_image/build/pcie_color_gui
```

主要功能：

```text
连接 PCIe 接收设备
采集一帧 FPGA 图像
连续采集 FPGA 图像
停止采集 / 安全停止 DMA
保存当前图像
加载 YOLO + LPR 模型
识别当前帧中的车牌
采集一帧并识别车牌
切换 RGB565 大小端显示
显示 DMA 状态和运行日志
```

当前图像格式固定按以下参数处理：

```text
分辨率：1920 x 1080
像素格式：RGB565
单帧大小：4147200 字节
默认字节序：RGB565 Little Endian
```

## 2. 使用前准备

### 2.1 确认 PCIe 链路和 FPGA 已准备好

先确认 FPGA 已烧录当前匹配版本，PCIe 设备能被系统枚举。常用检查命令：

```sh
lspci -nn
lspci -vv -s 01:00.0
```

正常情况下能看到类似：

```text
01:00.0 Memory controller: Device 0755:0755
LnkSta: Speed 2.5GT/s, Width x1
```

### 2.2 确认 CMA 足够

当前整帧 DMA 需要连续内存，推荐 `CmaTotal` 至少 128M：

```sh
cat /proc/meminfo | grep -E "MemAvailable|CmaTotal|CmaFree"
cat /proc/cmdline | tr ' ' '\n' | grep cma
```

期望至少看到：

```text
CmaTotal: 131072 kB
cma=128M
```

`CmaFree` 不一定一直很大，重点是驱动加载和连接设备时能成功分配 buffer。

### 2.3 确认推理环境

当前 GUI 默认使用这个 Python 环境运行 RKNN 推理：

```text
/home/cat/miniconda3/envs/fenqusai/bin/python
```

可手动检查依赖：

```sh
/home/cat/miniconda3/envs/fenqusai/bin/python -c "import cv2, numpy, rknnlite.api, ruamel.yaml"
sudo /home/cat/miniconda3/envs/fenqusai/bin/python -c "import cv2, numpy, rknnlite.api, ruamel.yaml"
```

没有输出错误就说明依赖可用。

模型和脚本路径：

```text
/home/cat/cat_pcie_project/model/best.rknn
/home/cat/cat_pcie_project/model/lprnet_unified_p7_yolo_crop_adapt_fp.rknn
/home/cat/cat_pcie_project/model/province_classifier_ft_p8_fp.rknn
/home/cat/cat_pcie_project/test/infer_rknn_plate.py
```

## 3. 编译界面

进入工程目录：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
```

编译 GUI：

```sh
make gui
```

如果只是清理再重新编译：

```sh
make clean
make gui
```

## 4. 加载驱动

每次测试前建议先进入 GUI 工程目录：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
```

安全起见，可以先卸载旧驱动：

```sh
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
```

加载当前驱动并允许 DMA START：

```sh
./scripts/load_driver.sh allow_dma_start=1
```

正常输出应包含：

```text
device ready: /dev/colorbar_pcie_rx
loaded params: ... allow_dma_start=1 ...
```

如果没有 `allow_dma_start=1`，界面采集时会被驱动安全门控拦住，通常出现：

```text
COLORBAR_IOC_START: Operation not permitted
```

## 5. 启动 GUI

推荐启动命令：

```sh
sudo ./build/pcie_color_gui
```

如果 sudo 后图形界面打不开，先执行：

```sh
xhost +local:root
sudo ./build/pcie_color_gui
```

启动后右侧日志会写入界面，同时也会保存到：

```text
/tmp/colorbar_pcie_gui.log
```

查看最近日志：

```sh
tail -n 120 /tmp/colorbar_pcie_gui.log
```

## 6. 界面按钮说明

### 6.1 连接设备

点击 `连接设备` 后，GUI 会打开：

```text
/dev/colorbar_pcie_rx
```

并向驱动申请 DMA buffer。

正常日志类似：

```text
connected: 1920x1080 RGB565 frame=4147200 buffer=4194304
```

如果这里失败，常见原因是：

```text
驱动没加载
/dev/colorbar_pcie_rx 不存在
CMA 不够导致 ALLOC_BUFS failed
PCIe 设备没有正常枚举
```

### 6.2 采集一帧

点击 `采集一帧` 后，流程是：

```text
START
→ 等待 FPGA DONE + IDLE
→ 读取 4147200 字节
→ RGB565 转 RGB888
→ 显示到左侧图像区域
```

正常日志类似：

```text
frame 1 captured: 4147200 bytes, xxx ms, x.xx fps
```

### 6.3 连续采集

点击 `连续采集` 后，GUI 会一帧一帧循环采集显示。

当前连续采集逻辑是：

```text
START
→ 等 DONE + IDLE
→ 读完整帧
→ 显示
→ 再 START 下一帧
```

也就是说，当前帧没有读完之前，不会提前启动下一帧。

连续采集期间建议只观察画面和 DMA 状态。如果要做模型识别，先点 `停止`，再点 `识别当前帧` 或 `采集并识别`。

### 6.4 停止

点击 `停止` 后会关闭连续采集，并发送安全停止命令。

正常日志类似：

```text
safe stop sent
```

如果当前正在采集一帧，停止会等当前帧结束或超时后生效。

### 6.5 保存图片

点击 `保存图片` 可以把当前界面中的 RGB888 图像保存成 PNG。

注意：保存的是当前已经显示出来的图像，不是原始 RGB565 文件。

### 6.6 加载模型

点击 `加载模型` 后，GUI 会检查：

```text
YOLO RKNN 模型是否存在
LPRNet RKNN 模型是否存在
省份分类 RKNN 模型是否存在
推理脚本是否存在
fenqusai Python 是否能导入 cv2/numpy/rknnlite/ruamel
```

正常日志：

```text
inference deps ok: python=/home/cat/miniconda3/envs/fenqusai/bin/python
models ready: YOLO + LPRNet + province verifier, python=/home/cat/miniconda3/envs/fenqusai/bin/python
```

右侧 `模型` 状态会显示：

```text
ready
```

### 6.7 识别当前帧

点击 `识别当前帧` 后，GUI 会对已经采集到的当前帧做车牌识别。

使用前必须已经采集过至少一帧。

流程是：

```text
当前 RGB888 图像
→ 保存为 /tmp/colorbar_pcie_plate_frame.png
→ 调用 test/infer_rknn_plate.py
→ YOLO 检测车牌框
→ LPRNet 识别车牌字符
→ 加载可视化结果图
→ 显示定位框和字符结果
```

正常日志可能类似：

```text
inference started: /tmp/colorbar_pcie_plate_frame.png
inference ok: plates=2 detections=2 result=粤K70399 total=xxxx ms
visualization loaded: /tmp/colorbar_pcie_plate_infer/colorbar_pcie_plate_frame_vis.jpg
```

### 6.8 采集并识别

点击 `采集并识别` 后，GUI 会先采一帧，再对这一帧做推理。

推荐第一次验证模型链路时使用这个按钮：

```text
连接设备
→ 加载模型
→ 采集并识别
```

### 6.9 RGB565 Little / Big Endian

默认选择：

```text
RGB565 Little Endian
```

因为当前 FPGA 侧说明是 RGB565 小端发送。

如果画面颜色明显不对，可以临时切换到：

```text
RGB565 Big Endian
```

切换后 GUI 会用当前缓存帧重新转换显示，不会重新启动 DMA。

## 7. 右侧状态栏怎么看

右侧状态栏包含 DMA 和模型状态。

### 7.1 DMA 状态

```text
帧号：当前驱动返回的 frame_counter
帧率：GUI 计算的显示帧率
DMA_ADDR：Linux 分配并写给 FPGA 的 DMA 地址低 32 位
ACTIVE_ADDR：FPGA 当前使用的 DMA 地址
ACTIVE_LEN：FPGA 当前传输长度
BYTES_SENT：FPGA 已发送字节数
DONE：FPGA 一帧 DMA 是否完成
IDLE：FPGA DMA 引擎是否空闲
BUSY：FPGA DMA 引擎是否忙
FIFO_OVERFLOW：FPGA FIFO 是否溢出
FIFO_UNDERFLOW：FPGA FIFO 是否读空
```

当前一帧成功的核心条件：

```text
BYTES_SENT = 4147200
DONE = 1
IDLE = 1
BUSY = 0
FIFO_OVERFLOW = 0
FIFO_UNDERFLOW = 0
```

如果 `DONE=1` 但 `FIFO_UNDERFLOW=1`，仍然按错误处理，不建议相信这帧图像。

### 7.2 YOLO + LPR 状态

```text
模型：ready / missing
车牌：识别到的车牌字符，或 no valid plate
车牌/目标：有效车牌数量 / YOLO 检测目标数量
推理ms：推理脚本返回的耗时
```

如果车牌栏显示：

```text
no valid plate
```

说明模型运行了，但没有输出有效车牌。

如果显示：

```text
infer failed
```

说明推理脚本执行失败，要看右侧日志或 `/tmp/colorbar_pcie_gui.log`。

## 8. 推荐操作流程

### 8.1 只看 FPGA 图像视频

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1
sudo ./build/pcie_color_gui
```

界面操作：

```text
连接设备
→ 连续采集
→ 停止
```

### 8.2 单帧采图保存

```text
连接设备
→ 采集一帧
→ 保存图片
```

### 8.3 单帧采图并识别车牌

```text
连接设备
→ 加载模型
→ 采集并识别
```

### 8.4 对当前画面重新识别

```text
连接设备
→ 采集一帧
→ 加载模型
→ 识别当前帧
```

或者：

```text
连接设备
→ 连续采集
→ 停止
→ 识别当前帧
```

不要在连续采集还运行时点识别。当前设计故意禁止连续采集和模型推理同时抢同一帧数据。

## 9. 中间文件和日志

GUI 日志：

```text
/tmp/colorbar_pcie_gui.log
```

推理输入图：

```text
/tmp/colorbar_pcie_plate_frame.png
```

推理输出目录：

```text
/tmp/colorbar_pcie_plate_infer/
```

带框可视化图：

```text
/tmp/colorbar_pcie_plate_infer/colorbar_pcie_plate_frame_vis.jpg
```

车牌裁剪图：

```text
/tmp/colorbar_pcie_plate_infer/crops/
```

如果 GUI 右侧没有显示定位框，可以先检查可视化图是否存在：

```sh
ls -l /tmp/colorbar_pcie_plate_infer/colorbar_pcie_plate_frame_vis.jpg
file /tmp/colorbar_pcie_plate_infer/colorbar_pcie_plate_frame_vis.jpg
```

## 10. 常见问题

### 10.1 点击采集提示 Operation not permitted

典型原因是驱动没有允许 DMA START。

重新加载：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
./scripts/load_driver.sh allow_dma_start=1
```

### 10.2 连接设备失败或 ALLOC_BUFS failed

可能是 DMA buffer 分配失败。

检查 CMA：

```sh
cat /proc/meminfo | grep -E "MemAvailable|CmaTotal|CmaFree"
```

当前建议 `CmaTotal` 为 128M。

### 10.3 加载模型失败

看右侧日志，如果出现：

```text
missing YOLO model
missing LPR model
missing province model
missing infer script
missing inference python
inference deps missing
```

就按日志提示检查对应路径或 Python 依赖。

当前默认 Python：

```text
/home/cat/miniconda3/envs/fenqusai/bin/python
```

### 10.4 推理失败 No module named ruamel

说明 GUI 使用的 Python 环境不对，或该环境缺依赖。

当前推荐直接使用默认新 GUI。如果要手动指定：

```sh
sudo COLORBAR_INFER_PYTHON=/home/cat/miniconda3/envs/fenqusai/bin/python ./build/pcie_color_gui
```

### 10.5 推理成功但没有定位框

看日志是否有：

```text
visualization loaded: /tmp/colorbar_pcie_plate_infer/colorbar_pcie_plate_frame_vis.jpg
```

如果有这行但画面没有框，检查可视化图本身：

```sh
ls -l /tmp/colorbar_pcie_plate_infer/colorbar_pcie_plate_frame_vis.jpg
```

如果日志是：

```text
inference ok: plates=0 detections=0 result=no valid plate
```

说明模型正常跑了，但当前图像没有检测到车牌目标。

### 10.6 RKNN dynamic range warning

运行推理时可能看到：

```text
Query dynamic range failed. Ret code: RKNN_ERR_MODEL_INVALID
```

当前可按静态 shape RKNN 模型警告处理。只要脚本最后输出 JSON、检测框、车牌字符，并生成可视化图，这个警告不是当前阻塞问题。

## 11. 安全退出

退出 GUI 前建议：

```text
先点“停止”
再关闭窗口
```

退出后可以卸载驱动：

```sh
cd /home/cat/cat_pcie_project/camara_host_computer/Colorbar_image
sudo rmmod colorbar_pcie_driver 2>/dev/null || true
```

如果只是继续下一轮 GUI 测试，也可以重新运行：

```sh
./scripts/load_driver.sh allow_dma_start=1
sudo ./build/pcie_color_gui
```
