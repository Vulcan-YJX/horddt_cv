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
缩放、GDC 畸变矫正、CPU 色彩转换以及硬件编解码能力。项目同时提供可以直接运行
的 sample，方便验证 SDK 环境和替换为实际相机数据。

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
| `horddt_remap` | NV12 畸变矫正/坐标映射 | D-Robotics GDC 硬件加速 |
| `horddt_color` | NV12/NV21 转 RGB/BGR | libyuv CPU/SIMD 转换 |
| `horddt_codec` | NV12/JPEG/H.264 单帧编解码 | D-Robotics Media Codec 硬件接口 |

> 当前 sample 使用 1920x1080 输入。替换为其他分辨率时，需要同步修改 sample 中的
> 宽高、stride 和对应的 GDC bin 配置。

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
gdc_cfg.h
gdc_bin_cfg.h

动态库：
libhbmem.so
libvpf.so
libvio.so
libcam.so
```

硬件编解码模块还需要：

```text
hb_media_codec.h
hb_media_error.h
libmultimedia.so
```

如果 SDK 安装在非默认路径，请在 CMake 配置时通过 `HOBOT_ROOT` 指定。系统没有
libyuv 或 OpenCV 开发文件时，CMake 会跳过色彩相关模块和 sample，但仍然可以构建
硬件 resize、remap 和 codec 模块。

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
| `remap_nv12` | GDC 畸变矫正 sample | GDC/VPF SDK |
| `codec_sample` | NV12/JPEG/H.264 编解码 sample | Media Codec SDK |
| `resize_nv12` | PYM 缩放并转 JPEG | PYM、libyuv、OpenCV |
| `color_convert` | NV12 转 JPEG | libyuv、OpenCV |

### 4. 准备输出目录

sample 不会自动创建输出目录。使用自定义输出目录前，请先创建：

```bash
mkdir -p output
```

## Example 示例

所有 sample 都支持 `-h` 和 `--help`。不带参数时使用默认输入；传入参数后，可以
替换为任意路径下、格式匹配的测试数据。

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

### 3. GDC 畸变矫正

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

### 4. 硬件编解码

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
调用者传入的图像 buffer。

### 2. GDC Remap API

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

类只管理 GDC 节点和 bin buffer，不释放调用者传入的图像 buffer。类会处理 GDC 内部
buffer 的 cache flush/invalidate，并按照输入输出 stride 复制结果。

### 3. Color Convert API

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

### 4. Codec API

```cpp
horddt_codec::config cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.frame_rate = 30;
cfg.bit_rate = 8192;
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
码流。类不会释放调用者传入的 `hb_mem_graphic_buf_t`。

## Data 数据文件

### 目录结构

```text
horddt_cv/
├── data/
│   ├── input_1920x1080.jpg
│   └── input_1920x1080.nv12
└── samples/
    └── gdc_1920x1080.bin
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

sample 不会自动创建输出目录，请提前执行：

```bash
mkdir -p output
```

## Contents 目录

以下为项目目录说明：

- [头文件接口](./include)
  - [Resize API](./include/horddt_resize.hpp)
  - [Remap API](./include/horddt_remap.hpp)
  - [Color API](./include/horddt_color.hpp)
  - [Codec API](./include/horddt_codec.hpp)
- [源码实现](./src)
- [Sample 程序](./samples)
- [测试数据](./data)
- [构建配置](./CMakeLists.txt)
