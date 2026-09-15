<p align="center"><strong>HORDDT CV</strong></p>
<p align="center">
  <a href="./LICENSE"><img alt="License" src="https://img.shields.io/badge/License-GPLv3-blue.svg"/></a>
  <img alt="language" src="https://img.shields.io/badge/language-C%2B%2B14-red.svg"/>
  <img alt="platform" src="https://img.shields.io/badge/platform-Linux-lightgrey.svg"/>
  <img alt="SDK" src="https://img.shields.io/badge/SDK-D--Robotics-orange.svg"/>
</p>

<p align="center">
  语言：<strong>中文</strong>
</p>

基于 D-Robotics multimedia SDK 的 C++ 图像处理封装，为 NV12/NV21 图像提供硬件
缩放、多区域裁剪、STITCH 画中画、GDC 旋转/畸变矫正、CPU 色彩转换以及硬件编解码能力。
项目同时提供可以直接运行的 sample，方便验证 SDK 环境和替换为实际相机数据。

---

## Basic Information 基本信息

- **项目名称**：`horddt_cv`
- **开发语言**：C++14
- **运行平台**：Linux + D-Robotics multimedia SDK
- **默认 SDK 路径**：`/usr/hobot`
- **默认 sample 工作目录**：仓库根目录
- **默认测试分辨率**：`1920 x 1080`
- **主要输入格式**：8-bit NV12；色彩模块额外支持 NV21

项目包含以下模块：

| 模块 | 功能 | 实现方式 |
| --- | --- | --- |
| `horddt_resize` | NV12 图像缩放 | D-Robotics PYM 硬件加速 |
| `horddt_pip` | 两张 NV12 图像画中画合成 | D-Robotics STITCH 硬件加速 |
| `horddt_crop` | NV12 单帧多区域裁剪 | D-Robotics PYM ROI 硬件加速 |
| `horddt_rotate` | NV12 旋转 90°/180°/270° | D-Robotics GDC Affine 硬件加速 |
| `horddt_remap` | NV12 畸变矫正/坐标映射 | D-Robotics GDC 硬件加速 |
| `horddt_color` | NV12/NV21 转 RGB/BGR | libyuv CPU/SIMD 转换 |
| `horddt_codec` | NV12/JPEG/H.264 单帧编解码 | D-Robotics Media Codec 硬件接口 |

> 当前 sample 使用 1920x1080 输入。替换为其他分辨率时，需要同步修改 sample 中的
> 宽高、stride 和对应的 GDC bin 配置。`horddt_rotate` 会在初始化时根据输入尺寸和
> 旋转角度动态生成 Affine 配置，不需要外部 GDC bin；90°/270° 输出宽高互换。

## Installation 安装

### 依赖环境

- CMake `3.10` 或更高版本；
- 支持 C++14 的编译器；
- D-Robotics multimedia SDK；
- `horddt_color`、`resize_nv12`、`color_convert` 需要 libyuv 和 OpenCV。

### D-Robotics SDK 依赖

基础硬件图像处理模块需要：

```text
头文件：
hb_mem_mgr.h
hbn_vpf_interface.h
hbn_pym_cfg.h
hbn_sth_cfg.h（部分 SDK 中为 hbn_stitch_cfg.h/stitch_cfg.h）
gdc_cfg.h
gdc_bin_cfg.h

动态库：
libhbmem.so
libvpf.so
libvio.so
libcam.so
```

`realtime_vin_camera_pipeline` 还会直接包含 Camera/VIN 配置接口：

```text
hb_camera_interface.h
hb_deserial_interface.h
hb_camera_data_config.h
vin_cfg.h
hbn_vpf_data_info.h
```

硬件编解码模块还需要：

```text
hb_media_codec.h
hb_media_error.h
libmultimedia.so
```

如果 SDK 安装在非默认路径，请在 CMake 配置时通过 `HOBOT_ROOT` 指定。系统没有
libyuv 或 OpenCV 开发文件时，CMake 会跳过色彩相关模块和 sample，但仍然可以构建
硬件 resize、PIP、remap 和 codec 模块。

## Quick Start 快速开始

### 1. 获取源码并进入目录

```bash
cd horddt_cv
```

### 2. 编译全部可用目标

SDK 默认安装在 `/usr/hobot` 时：

```bash
cmake -S . -B build
cmake --build build -j
```

SDK 安装在其他目录时：

```bash
cmake -S . -B build \
  -DHOBOT_ROOT=/path/to/hobot
cmake --build build -j
```

libyuv 安装在自定义目录时：

```bash
cmake -S . -B build \
  -DHOBOT_ROOT=/path/to/hobot \
  -DLIBYUV_ROOT=/path/to/libyuv
cmake --build build -j
```

不需要色彩模块时：

```bash
cmake -S . -B build \
  -DHOBOT_ROOT=/path/to/hobot \
  -DHORDDT_BUILD_COLOR=OFF
cmake --build build -j
```

### 3. 查看生成目标

常见目标如下：

| 目标 | 说明 | 依赖 |
| --- | --- | --- |
| `crop_nv12` | 一次提交将 1920x1080 NV12 从中间裁成左右两张图 | PYM/VPF SDK |
| `remap_nv12` | GDC 畸变矫正 sample | GDC/VPF SDK |
| `rotate_sample` | GDC 旋转 90°/180°/270° sample | GDC/VPF SDK |
| `codec_sample` | NV12/JPEG/H.264 编解码 sample | Media Codec SDK |
| `resize_nv12` | PYM 缩放并转 JPEG | PYM、libyuv、OpenCV |
| `pip_nv12` | JPEG→NV12→半尺寸缩放→STITCH 画中画→JPEG | PYM、STITCH、libyuv、OpenCV |
| `color_convert` | NV12 转 JPEG | libyuv、OpenCV |
| `realtime_camera_pipeline` | V4L2 摄像头实时 GDC→PYM→Color/Codec | 全部模块、V4L2 DMABUF |
| `realtime_vin_camera_pipeline` | GMSL/VIN 摄像头实时 GDC→PYM→Color/Codec | 全部模块、Camera/VIN SDK |
| `realtime_vin_camera_pipeline_simple` | 固定默认参数的精简 GMSL/VIN 实时流水线 | 全部模块、Camera/VIN SDK |

### 4. 准备输出目录

sample 不会自动创建输出目录。使用自定义输出目录前，请先创建：

```bash
mkdir -p output
```

## Example 示例

所有 sample 都支持 `-h` 和 `--help`。不带参数时使用默认输入或默认配置；传入参数后
可以替换为格式匹配的数据。`realtime_vin_camera_pipeline` 默认尝试从
`samples/gdc_1088x2560.bin` 加载 GDC 标定文件，文件缺失时自动旁路 GDC。

### 1. NV12 转 JPEG

#### 功能概述

使用 `horddt_color` 和 libyuv 将一帧 1920x1080 NV12 图像转换为 BGR，再使用
OpenCV 编码为 JPEG。

#### 默认命令

```bash
./build/color_convert
```

#### 默认输入输出

```text
输入：data/input_1920x1080.nv12
输出：./color_output_1920x1080.jpg
```

#### 指定输入输出

```bash
./build/color_convert \
  data/input_1920x1080.nv12 \
  output/color.jpg
```

#### 参数格式

```text
color_convert [input.nv12 output.jpg]
```

查看帮助：

```bash
./build/color_convert --help
```

---

### 2. PYM 图像缩放

#### 功能概述

使用 D-Robotics PYM 将 1920x1080 NV12 图像缩放到 640x360，然后转换为 JPEG。
当前 sample 配置为固定输入和输出尺寸。

#### 默认命令

```bash
./build/resize_nv12
```

#### 默认输入输出

```text
输入：data/input_1920x1080.nv12
输出：./output_640x360.jpg
```

#### 指定输入输出

```bash
./build/resize_nv12 \
  data/input_1920x1080.nv12 \
  output/resized.jpg
```

#### 参数格式

```text
resize_nv12 [input.nv12 output.jpg]
```

> 当前 PYM 实现不支持放大，输出尺寸必须满足 SDK 支持的缩放比例。

---

### 3. STITCH 硬件画中画

#### 功能概述

`pip_nv12` 完成以下流程：

1. OpenCV 加载一张 JPEG，并转换为原始尺寸 NV12 hbmem buffer；
2. `horddt_resize` 使用 PYM 将 NV12 缩小为原宽高的一半；
3. `horddt_pip` 使用 STITCH 的多个非重叠 `BLENDING_MODE_SRC` ROI，将原图背景
   划分到小窗四周，并把半尺寸图像作为右上角不透明小窗，输出新的原尺寸 NV12；
4. `horddt_color` 将合成 NV12 转成 BGR，OpenCV 写出 JPEG。

STITCH 只负责硬件合成，不负责缩放；小窗尺寸必须在调用 `horddt_pip` 前通过 PYM
或其他模块准备好。

#### 默认命令

```bash
./build/pip_nv12
```

#### 默认输入输出

```text
输入：data/input_1920x1080.jpg
中间背景：1920x1080 NV12
中间小窗：960x540 NV12
小窗位置：右上角，距顶部和右侧 32 像素
输出：./pip_output.jpg
```

#### 指定输入输出

```bash
./build/pip_nv12 \
  data/input_1920x1080.jpg \
  output/pip.jpg
```

#### 参数格式

```text
pip_nv12 [input.jpg output.jpg]
```

输入宽高会向下裁剪到 4 的倍数，最多裁掉 3 个像素，以确保原图 NV12 和半尺寸
NV12 的宽高、ROI 坐标均满足偶数对齐。当前封装要求 STITCH 输入 buffer 的实际
stride 与初始化配置一致，sample 使用 64 字节对齐 stride。

---

### 4. PYM 硬件裁剪

#### 功能概述

`crop_nv12` 使用 `horddt_crop` 的两个 PYM DS ROI 输出通道。两个通道都选择原始
SRC 层，一帧输入只提交一次，在垂直中线处裁成两张 960x1080 NV12 图像：

```text
左图：x=0,   y=0, width=960, height=1080
右图：x=960, y=0, width=960, height=1080
```

#### 默认命令

```bash
./build/crop_nv12
```

#### 默认输入输出

```text
输入：data/input_1920x1080.nv12
输出：./crop_left_960x1080.nv12
输出：./crop_right_960x1080.nv12
```

#### 指定输入输出

输出到仓库根目录时，可以直接执行：

```bash
./build/crop_nv12 \
  data/input_1920x1080.nv12 \
  crop_left.nv12 \
  crop_right.nv12
```

执行成功后会输出：

```text
Crop complete:
  left : crop_left.nv12 (960x1080)
  right: crop_right.nv12 (960x1080)
```

如果需要输出到 `output/` 目录，程序不会自动创建目录，必须先执行：

```bash
mkdir -p output
./build/crop_nv12 \
  data/input_1920x1080.nv12 \
  output/crop_left.nv12 \
  output/crop_right.nv12
```

否则会出现：

```text
Cannot open output file 'output/crop_left.nv12': No such file or directory
Hardware crop failed: -1
```

#### 参数格式

```text
crop_nv12 [input.nv12 left.nv12 right.nv12]
```

> NV12 裁剪区域的 x、y、width、height 必须为偶数，区域不能越界，且当前 PYM
> 输出宽高不能小于 32x32。

---

### 5. GDC 畸变矫正

#### 功能概述

使用 D-Robotics GDC 对一帧 NV12 图像进行硬件畸变矫正或坐标映射。

#### 默认命令

```bash
./build/remap_nv12
```

#### 默认输入输出

```text
输入：data/input_1920x1080.nv12
GDC bin：samples/gdc_1920x1080.bin
输出：./remap_output_1920x1080.nv12
```

#### 指定输入、GDC bin 和输出

```bash
./build/remap_nv12 \
  data/input_1920x1080.nv12 \
  samples/gdc_1920x1080.bin \
  output/remap.nv12
```

#### 参数格式

```text
remap_nv12 [input.nv12 gdc.bin output.nv12]
```

#### GDC bin 注意事项

GDC bin 不只是分辨率配置，还包含镜头和标定参数。实际部署时必须使用与摄像头、
镜头以及输入输出尺寸匹配的 bin，否则可能出现以下问题：

- GDC 节点初始化失败；
- 输出图像无效或几何变形不正确；
- 输入输出尺寸或 stride 校验失败。

---

### 6. GDC 图像旋转

`rotate_sample` 使用 `horddt_rotate` 对固定为 1920x1080 的一帧 NV12 图像执行
90°、180°或 270°硬件旋转。旋转配置在初始化时动态生成，不需要外部 GDC bin；
90°和 270°旋转后的输出尺寸为 1080x1920，180°仍为 1920x1080。

默认执行 180°旋转：

```bash
./build/rotate_sample
```

默认输入输出：

```text
输入：data/input_1920x1080.nv12
输出：./rotate_output_1920x1080_180.nv12
角度：180
```

指定输入、输出和角度：

```bash
./build/rotate_sample \
  data/input_1920x1080.nv12 \
  output/rotate_90.nv12 \
  90
```

参数格式：

```text
rotate_sample [input.nv12 output.nv12 angle]
```

其中 `angle` 只能为 `90`、`180` 或 `270`。

---

### 7. 硬件编解码

#### 功能概述

`codec_sample` 使用 D-Robotics Media Codec 完成三种转换：

1. NV12 -> JPEG；
2. NV12 -> Annex-B H.264；
3. JPEG -> Annex-B H.264。

#### 默认命令

```bash
./build/codec_sample
```

#### 默认输入输出

```text
输入：data/input_1920x1080.nv12
输入：data/input_1920x1080.jpg
输出：./codec_output_1920x1080.jpg
输出：./nv12_output_1920x1080.h264
输出：./jpeg_output_1920x1080.h264
```

#### 指定输入和输出目录

```bash
mkdir -p output

./build/codec_sample \
  data/input_1920x1080.nv12 \
  data/input_1920x1080.jpg \
  output
```

#### 参数格式

```text
codec_sample [input.nv12 input.jpg output_dir]
```

`output_dir` 必须在运行前存在。程序会在该目录下生成固定名称的三个输出文件。
为了让 NV12->H.264 和 JPEG->H.264 的单帧码流能够独立解码，sample 会在两次 H.264
转换之间关闭第一个 codec 对象，并创建新的 codec 对象。

---

### 8. 实时摄像头零中间拷贝流水线

`realtime_camera_pipeline` 使用 V4L2 的 `V4L2_MEMORY_DMABUF`，让摄像头直接写入
预分配的 `hb_mem_graphic_buf_t`。一帧数据按如下顺序处理：

```text
V4L2 camera -> hbmem NV12
             -> GDC borrowed output
             -> PYM borrowed 640x360 output
                ├-> H.264 encoder -> borrowed bitstream -> file
                ├-> JPEG encoder（按间隔）-> borrowed bitstream -> MJPEG file
                └-> libyuv BGR（按间隔，复用 cv::Mat）
```

运行：

```bash
./build/realtime_camera_pipeline
```

完整参数：

```text
realtime_camera_pipeline \
  [camera gdc.bin output.h264 snapshots.mjpg \
   frame_limit color_every jpeg_every]
```

例如处理 300 帧，每帧执行颜色转换，每 30 帧保存一张 JPEG：

```bash
./build/realtime_camera_pipeline \
  /dev/video0 \
  samples/gdc_1920x1080.bin \
  output/camera.h264 \
  output/snapshots.mjpg \
  300 1 30
```

其中 `frame_limit=0` 表示一直运行到 `Ctrl-C`；`color_every=0` 或 `jpeg_every=0`
可以关闭对应分支。sample 默认要求摄像头支持单平面 NV12、streaming 和 DMABUF；若
驱动只支持 `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`，需要将 capture 类改成 multiplanar
API。GDC bin 必须与 1920x1080 摄像头和镜头标定匹配。

该 sample 避免的拷贝包括：

- 摄像头不再先采集到 mmap/userptr 再复制到 hbmem；
- GDC 输出不复制到应用层 NV12 buffer；
- PYM 输出不复制到应用层 NV12 buffer；
- JPEG/H.264 码流不复制到临时 `std::vector`，直接在 SDK buffer 有效期内写出；
- `cv::Mat` 在循环外创建并复用。

调度上，H.264 在采集线程执行，Color 和 JPEG 使用两个常驻 worker 并行处理；主线程在
PYM borrowed callback 返回前等待两个 worker，既共享同一份 PYM 输出，又不会越过
SDK buffer 生命周期。通过 `color_every=0` 或 `jpeg_every=0` 关闭分支时，对应算子、
输出文件和 worker 均不会创建。Camera 也会等所有持久化算子、文件和 worker 初始化
完成后再执行 `VIDIOC_STREAMON`，避免启动阶段积压旧帧。

sample 中采用的资源配置如下：

| 环节 | 配置 | 目的 |
| --- | --- | --- |
| Camera | 4 个 DMABUF | 当前帧处理时仍给驱动保留采集队列，减少断流风险 |
| GDC | `output_buffer_count=2` | 同步 borrowed 流水线所需的较小输出池 |
| PYM | `output_buffer_count=2`、`feedback_buffer_count=2` | 降低常驻图像内存，同时保留双缓冲 |
| PYM | `enable_file_io=false`、`enable_extra_layers=false` | 不分配文件输入 buffer，不生成未消费的额外层 |
| Codec | H.264/JPEG 使用独立对象，各 2 个 frame/bitstream buffer | 两路编码可并行，且不初始化 JPEG decoder |
| Cache | Camera→GDC、GDC→PYM 不做 CPU cache 同步；PYM 输出只 invalidate 一次 | 避免硬件链路上的重复 flush/invalidate |
| CPU 分支 | `color_every`、`jpeg_every` | 降低 libyuv CPU 计算量和 JPEG 编码/写盘频率 |

这里使用“帧内并行、帧间同步”的方式：一帧的所有消费者结束后才归还 PYM 和 Camera
buffer。相比把 borrowed 指针直接放入异步队列，这种方式不会产生悬空引用，也不需要
额外的引用计数和跨帧 buffer 池。若后续改为跨帧异步流水线，应把 GDC/PYM/Codec buffer
数量提高到 3 或更多，并在最后一个消费者完成后再显式归还上游 buffer。

目前 Media Codec SDK 接口仍由编码器管理自己的输入 frame buffer，因此
`PYM -> codec` 内部仍有一次 NV12 拷贝。要彻底移除这一次，需要板端 SDK 明确支持并
验证 `external_frame_buf`/物理地址导入；项目没有在未知 SDK ABI 上强行开启该模式。


### 9. autocube_media Camera/VIN 实时流水线

`realtime_vin_camera_pipeline` 是第二种实时相机入口。它参考
`autocube_media/autocube_gstcamera/src/stereo_camera_reader.cpp`，不经过
`/dev/video*`，而是直接使用 D-Robotics Camera、Deserializer、VIN 和 VFlow API：

```text
yx_s397_6010 (GMSL link 0)
  -> hbn_camera_create + MAX96712 deserializer
  -> VIN DDR output (VIN-owned hbmem NV12, 1088x2560)
  -> GDC borrowed output (1088x2560)
  -> PYM borrowed output (544x1280)
     ├-> H.264 encoder -> borrowed bitstream -> file
     ├-> JPEG encoder（按间隔）-> borrowed bitstream -> MJPEG file
     └-> libyuv BGR（按间隔，复用 cv::Mat）
```

相机固定配置位于：

```text
samples/camera/yx_s397_6010_sensor.h
samples/camera/yx_s397_6010_sensor.c
```

它保留了参考工程中的关键参数：`1088x2560@30`、YUV422 MIPI 输入、MIPI RX 4、
MAX96712、GMSL link 0、VIN DDR 输出和 6 个连续 buffer。Camera/VIN 初始化顺序为：

```text
hbn_camera_create
hbn_vnode_open(HB_VIN) + VIN attrs/buffer pool
hbn_vflow_create + hbn_vflow_add_vnode
hbn_deserial_create
camera -> deserializer -> VIN attach
hbn_vflow_start
hbn_vnode_getframe / hbn_vnode_releaseframe
```

可以不带参数直接启动：

```bash
./build/realtime_vin_camera_pipeline
```

程序默认查找与该摄像头、标定以及 **1088x2560 输入/输出尺寸匹配** 的文件：

```text
samples/gdc_1088x2560.bin
```

如果默认文件不存在，sample 会打印警告并自动使用零拷贝的 `VIN -> PYM` 路径继续
运行，避免因为仓库没有设备专用标定文件而退出；此时只是不执行畸变矫正。显式传入
GDC 路径时，该文件是必需的，打开失败会立即退出。仓库内现有
`samples/gdc_1920x1080.bin` 不匹配，不能用于这个 sample：

```bash
./build/realtime_vin_camera_pipeline /path/to/gdc_1088x2560.bin
```

完整参数：

```text
realtime_vin_camera_pipeline \
  [gdc_1088x2560.bin output.h264 snapshots.mjpg \
   frame_limit color_every jpeg_every]
```

例如处理 300 帧，每帧执行 Color 和 H.264，每 30 帧执行一次 JPEG：

```bash
./build/realtime_vin_camera_pipeline \
  /path/to/gdc_1088x2560.bin \
  output/vin_camera.h264 \
  output/vin_snapshots.mjpg \
  300 1 30
```

`frame_limit=0` 表示持续运行到 `Ctrl-C`；`color_every=0` 或 `jpeg_every=0` 可关闭对应
分支。H.264 分支始终启用。存在有效的默认或显式 GDC bin 时，会调用 GDC、PYM、
Color、JPEG 和 H.264 全部功能单元；默认 GDC 文件缺失时仅旁路 GDC。

MAX96712 在 attach 阶段可能暂时返回 `-65672`。sample 现在与 `autocube_media` 一致：
销毁本次未完成的 Camera/VIN pipeline，等待 3 秒后自动重试，不需要人工反复重启。
`Ctrl-C` 可以终止重试。PYM 每层缩放范围按硬件要求使用 `(1/2, 1]`；
`1088x2560 -> 544x1280` 会选择下一层 BL 的 1:1 输出，避免
`hbn_vnode_set_attr` 返回 `-983049`。

这个版本沿用 borrowed 生命周期：VIN frame 在 GDC、PYM、Color 和两个 Codec 分支
全部完成后才调用 `hbn_vnode_releaseframe()`。因此 VIN→GDC→PYM 之间不创建应用层 NV12
副本，GDC/PYM 输出也不会跨回调保存。PYM 输出的 cache 只在进入 CPU/Codec 消费回调前
invalidate 一次。H.264 运行在采集线程，Color/JPEG 使用常驻 worker 帧内并行。

> `autocube_media` 的双目展示流程还会把 1088x2560 图像顺时针旋转并切成左右两幅
> 1280x1088 图像。完整版 `realtime_vin_camera_pipeline` 当前未接入 rotate/split
> 阶段，因此仍对完整的 1088x2560 VIN 帧执行 GDC，并缩放为 544x1280；它不会模拟
> 双目旋转和切分。
>
> Camera/VIN/Deserializer 配置与板卡、线束、I2C 地址和 SDK ABI 强相关。请在目标
> D-Robotics 板端确认 sensor 配置及 GDC bin，不能只在普通 Linux 主机验证。

#### 精简版本

`realtime_vin_camera_pipeline_simple` 使用
Camera→VIN→CPU Resize→GDC→Rotate 180°→PYM→Codec/Color 处理路径，并删除命令行解析、
GDC 文件存在性检查、输入图像格式检查、可选分支和并行 worker，适合直接阅读最基本的
调用顺序。
它固定使用：

```text
GDC:   samples/gdc_1920x1080.bin
H.264: vin_camera_output.h264
MJPEG: vin_camera_snapshots.mjpg
```

准备好仓库默认的 1920x1080 GDC 文件后，直接执行：

```bash
./build/realtime_vin_camera_pipeline_simple
```

程序最多尝试初始化 Camera/Deserializer/VIN 三次，两次重试之间等待 3 秒。初始化成功
后，先使用 libyuv 将 VIN 的 1088x2560 NV12 resize 为 1920x1080，再使用仓库现有的
`gdc_1920x1080.bin` 执行 GDC，再以 borrowed 方式旋转 180°，随后由 PYM 缩放到
960x536，并执行 H.264、JPEG 和 BGR 分支。这里使用 536 而不是 540，是因为
H.264/JPEG 要求输出高度按 8 对齐。
JPEG 每 30 帧执行一次，程序运行到按下 `Ctrl-C`。由于 PYM 不支持把宽度
从 1088 放大到 1920，第一段 resize 必须使用 CPU/libyuv；其 1920x1080 hbmem 输出
buffer 只申请一次并循环复用。精简版仍保留必要的 SDK 返回值判断和 buffer 归还。

## C++ API 说明

### 1. PYM Resize API

```cpp
horddt_resize::config cfg;
cfg.input_width = 1920;
cfg.input_height = 1080;
cfg.output_width = 640;
cfg.output_height = 360;
cfg.timeout_ms = 2000;

horddt_resize resizer(cfg);
if (!resizer.is_initialized()) {
    return resizer.initialization_status();
}

int ret = resizer.resize(input_nv12_buffer, output_nv12_buffer);
```

输入和输出均为调用者分配的 `hb_mem_graphic_buf_t`，格式必须为 NV12。类不会释放
调用者传入的图像 buffer。实时流水线可以使用借用接口，避免 PYM 输出拷贝：

```cpp
resizer.resize_borrowed(input_nv12_buffer,
    [&](const hb_mem_graphic_buf_t &pym_output) {
        // pym_output 只在回调期间有效，禁止保存或释放。
        return consume(pym_output);
    });
```

`enable_file_io=false` 可取消实时场景不需要的内部文件输入 buffer；
`enable_extra_layers=false` 只启用到目标 channel 所需的金字塔层。

### 2. STITCH PIP API

```cpp
#include "horddt_pip.hpp"

horddt_pip::config cfg;
cfg.background_width = 1920;
cfg.background_height = 1080;
cfg.overlay_width = 960;
cfg.overlay_height = 540;
cfg.overlay_x = 928;
cfg.overlay_y = 32;
cfg.background_stride = 1920;
cfg.overlay_stride = 960;
cfg.output_stride = 1920;

horddt_pip pip(cfg);
int ret = pip.compose(background_nv12, overlay_nv12, output_nv12);
```

`compose()` 将 STITCH 内部输出复制到调用方 buffer；实时链路可使用
`compose_borrowed()` 在 callback 内直接消费 STITCH-owned buffer。两个输入和输出均为
NV12，尺寸、坐标必须为偶数，输入/输出实际 stride 必须与配置一致。当前实现使用
STITCH 外部 buffer 回灌模式（`mode=0`），背景 ROI 与小窗 ROI 均为 Src Copy。
提交一帧时先使用 `hbn_vnode_sendframe_async()` 提交小窗通道 1，再使用
`hbn_vnode_sendframe()` 提交背景通道 0 触发硬件处理，与官方
`sample_gdc_stitch` 的多输入提交顺序一致。

### 3. PYM Crop API

```cpp
horddt_crop::config cfg;
cfg.input_width = 1920;
cfg.input_height = 1080;
cfg.regions = {
    {0,   0, 960, 1080},
    {960, 0, 960, 1080},
};
cfg.enable_file_io = false;

horddt_crop cropper(cfg);
int ret = cropper.crop_borrowed(input_nv12_buffer,
    [&](const std::vector<hb_mem_graphic_buf_t> &crops) {
        // crops[0]/crops[1] 只在回调期间有效，像素数据没有发生 CPU 拷贝。
        return consume(crops);
    });
```

每个 ROI 使用一个 PYM DS 输出通道，并且都直接选择 SRC 层。因此一次
`hbn_vnode_sendframe()` 可得到所有裁剪结果。`crop_borrowed()` 是实时流水线的推荐接口；
传入调用方 output buffer 的 `crop()` 会执行逐行复制，文件接口则将 stride 输出写成
紧凑 NV12 文件。

### 4. GDC Remap API

```cpp
horddt_remap::config cfg;
cfg.input_width = 1920;
cfg.input_height = 1080;
cfg.output_width = 1920;
cfg.output_height = 1080;
cfg.input_stride = 1920;   // 0 表示 align16(input_width)
cfg.output_stride = 1920;  // 0 表示 align16(output_width)
cfg.gdc_bin_path = "camera_1920x1080_gdc.bin";
cfg.timeout_ms = 2000;

horddt_remap remapper(cfg);
if (!remapper.is_initialized()) {
    return remapper.initialization_status();
}

int ret = remapper.remap(input_nv12_buffer, output_nv12_buffer);
```

类只管理 GDC 节点和 bin buffer，不释放调用者传入的图像 buffer。复制式 API 会按照
stride 拷贝结果；实时流水线可改用 `remap_borrowed()`，在 GDC 内部输出释放前直接调用
下游。`sync_input_for_device=false` 仅适用于输入刚由 DMA 硬件产生且 CPU 未修改的情况；
`sync_borrowed_output_for_cpu=false` 仅适用于下游仍是硬件消费者。

### 5. GDC Rotate API

```cpp
horddt_rotate::config cfg;
cfg.input_width = 1920;
cfg.input_height = 1080;
cfg.rotation = horddt_rotate::angle::rotate_180;
cfg.input_stride = 1920;
cfg.output_stride = 1920;

horddt_rotate rotator(cfg);
int ret = rotator.rotate(input_nv12_buffer, output_nv12_buffer);
```

90°和 270°会交换输出宽高，可通过 `output_width()` 和 `output_height()` 查询。
实时流水线可使用 `rotate_borrowed()`，在 GDC 输出 buffer 释放前直接执行下游处理。

### 6. Color Convert API

```cpp
horddt_color color_converter;
cv::Mat output_image;

int ret = color_converter.convert(
    input_nv12_buffer,
    output_image,
    horddt_color::output_format::bgr);
```

支持的转换关系：

```text
NV12 -> BGR
NV21 -> BGR
NV12 -> RGB
NV21 -> RGB
```

`horddt_color` 使用 libyuv 完成 CPU/SIMD 色彩转换，OpenCV 仅用于承载输出的
`cv::Mat`。输入 buffer 必须提供 CPU 可访问的 `virt_addr[0]` 和 `virt_addr[1]`。
如果输入由 VIO、VSE、GDC 或 Codec 等硬件模块写入，调用者应在转换前完成必要的
cache 同步。

### 7. Codec API

```cpp
horddt_codec::config cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.frame_rate = 30;
cfg.bit_rate = 8192;
cfg.h264_vbv_buffer_size = 20;  // 低码率/高质量场景可适当增大
cfg.h264_mb_level_rc_enable = true;
cfg.jpeg_quality = 90;
cfg.timeout_ms = 2000;

horddt_codec codec(cfg);
std::vector<std::uint8_t> jpeg;
std::vector<std::uint8_t> h264;

codec.nv12_to_jpeg(input_nv12_buffer, jpeg);
codec.nv12_to_h264(input_nv12_buffer, h264);
codec.jpeg_to_h264(jpeg_data, jpeg_size, h264);
```

JPEG 和 H.264 输出保存在 `std::vector<std::uint8_t>` 中。H.264 输出为 Annex-B
码流。类不会释放调用者传入的 `hb_mem_graphic_buf_t`。实时输出建议使用
`nv12_to_jpeg_borrowed()` / `nv12_to_h264_borrowed()`，直接消费 SDK 码流 buffer。
还可以关闭未使用的 codec context，并调整 frame/bitstream buffer 数量：

```cpp
cfg.enable_jpeg_encoder = false;
cfg.enable_h264_encoder = true;
cfg.enable_jpeg_decoder = false;
cfg.frame_buffer_count = 2;
cfg.bitstream_buffer_count = 2;
```

借用的图像或码流指针都只在回调期间有效；回调返回后底层 buffer 会立即归还 SDK。

## Data 数据文件

### 目录结构

```text
horddt_cv/
├── data/
│   ├── input_1920x1080.jpg
│   └── input_1920x1080.nv12
└── samples/
    ├── camera/
    │   ├── yx_s397_6010_sensor.c
    │   └── yx_s397_6010_sensor.h
    ├── crop_nv12_sample.cpp
    ├── gdc_1920x1080.bin
    ├── realtime_camera_pipeline.cpp
    ├── remap_nv12_sample.cpp
    ├── rotate_sample.cpp
    ├── realtime_vin_camera_pipeline.cpp
    └── realtime_vin_camera_pipeline_simple.cpp
```

### NV12 文件格式

`data/input_1920x1080.nv12` 是一帧紧凑排列的 NV12 原始图像：

```text
分辨率：1920 x 1080
格式：8-bit NV12（Y plane + interleaved UV plane）
文件大小：1920 x 1080 x 3 / 2 = 3,110,400 bytes
```

文件本身不包含 stride padding。sample 会按行把有效像素复制到 SDK 分配的带 stride
buffer 中。替换其他 NV12 文件时，必须保证文件分辨率和 sample 配置一致。

## PYM Crop 实现参考

`horddt_crop` 参考：

```text
multimedia_samples/sample_pym/sample_pym.c
```

实现复用 PYM M2M 生命周期，并通过 `ds_roi_sel[]`、`ds_roi_en` 和
`ds_roi_info[]` 配置多路 SRC ROI。输出使用 `hbn_vnode_getframe_group()` 一次获取，
借用接口在 callback 返回后统一调用 `hbn_vnode_releaseframe_group()` 归还硬件 buffer。

## GDC 实现参考

`horddt_remap` 参考 D-Robotics multimedia samples：

```text
multimedia_samples/sample_pipeline/common/vp_pipeline.c
multimedia_samples/sample_gdc/3-gdc_static_valid/gdc_static_valid.c
```

节点属性采用 pipeline/GDC sample 使用的 `gdc_settings_t` 聚合结构，并设置 SDK 要求
的 `magicNumber = 0x12345678`。独立的 buffer-to-buffer 调用使用 GDC feedback 模式：

```text
hbn_vnode_open(HB_GDC)
hbn_vnode_set_attr / set_ichn_attr / set_ochn_attr
hbn_vnode_start
hbn_vnode_sendframe
hbn_vnode_getframe
hbn_vnode_releaseframe
hbn_vnode_stop / close
```

## FAQ 常见问题

### 1. CMake 提示找不到 D-Robotics SDK

确认 SDK 头文件和库文件已安装，并通过 `HOBOT_ROOT` 指向 SDK 根目录：

```bash
cmake -S . -B build -DHOBOT_ROOT=/path/to/hobot
```

SDK 根目录通常应包含 `include/` 和 `lib/` 子目录。

### 2. CMake 跳过 `color_convert` 或 `resize_nv12`

这通常表示缺少 libyuv、OpenCV 头文件或对应库文件。检查：

```bash
cmake -S . -B build \
  -DHOBOT_ROOT=/path/to/hobot \
  -DLIBYUV_ROOT=/path/to/libyuv
```

也可以关闭色彩模块，只编译硬件图像处理和编解码模块：

```bash
cmake -S . -B build -DHORDDT_BUILD_COLOR=OFF
```

### 3. Sample 找不到输入文件

sample 默认从仓库根目录运行：

```bash
cd horddt_cv
./build/color_convert
```

或者直接传入输入文件路径：

```bash
./build/color_convert /path/to/input.nv12 /path/to/output.jpg
```

### 4. GDC 初始化失败或输出结果异常

检查以下项目：

- GDC bin 是否与摄像头和镜头标定参数匹配；
- GDC bin 是否与输入输出分辨率匹配；
- 输入文件是否为正确的 NV12 格式；
- 输入输出 stride 是否满足 SDK 要求；
- 当前硬件是否支持对应的 GDC 配置。

### 5. 输出目录不存在

包括 `crop_nv12` 在内的 sample 不会自动创建输出目录。以下命令中的
`output/crop_left.nv12` 和 `output/crop_right.nv12` 只有在 `output/` 已存在时才能打开：

```bash
mkdir -p output
./build/crop_nv12 \
  data/input_1920x1080.nv12 \
  output/crop_left.nv12 \
  output/crop_right.nv12
```

如果不希望创建目录，可以将输出文件直接写到当前目录：

```bash
./build/crop_nv12 \
  data/input_1920x1080.nv12 \
  crop_left.nv12 \
  crop_right.nv12
```

## Contents 目录

以下为项目目录说明：

- [头文件接口](./include)
  - [Resize API](./include/horddt_resize.hpp)
  - [Crop API](./include/horddt_crop.hpp)
  - [Remap API](./include/horddt_remap.hpp)
  - [Color API](./include/horddt_color.hpp)
  - [Codec API](./include/horddt_codec.hpp)
- [源码实现](./src)
- [Sample 程序](./samples)
- [测试数据](./data)
- [构建配置](./CMakeLists.txt)
