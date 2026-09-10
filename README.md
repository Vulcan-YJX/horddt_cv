# horddt_cv

`horddt_cv` 是面向 D-Robotics multimedia SDK 的 C++ 图像处理封装，提供：

- `horddt_resize`：使用 PYM 对 NV12 图像进行硬件缩放；
- `horddt_remap`：使用 GDC 对 NV12 图像进行硬件畸变矫正/坐标映射；
- `horddt_color`：使用 libyuv 将 NV12/NV21 转换为 RGB/BGR（可选）；
- `horddt_codec`：使用 Media Codec 硬件接口完成 NV12/JPEG/H.264 的单帧编解码。

## 目录结构

```text
horddt_cv/
├── CMakeLists.txt
├── README.md
├── data/                              # sample 输入数据
│   ├── input_1920x1080.jpg           # 1920x1080 JPEG
│   └── input_1920x1080.nv12          # 1920x1080、8-bit、半平面 NV12 原始数据
├── include/
│   ├── horddt_codec.hpp
│   ├── horddt_color.hpp
│   ├── horddt_remap.hpp
│   └── horddt_resize.hpp
├── src/
│   ├── horddt_codec.cpp
│   ├── horddt_color.cpp
│   ├── horddt_remap.cpp
│   └── horddt_resize.cpp
└── samples/
    ├── codec_sample.cpp
    ├── color_convert_sample.cpp
    ├── gdc_1920x1080.bin           # 与 sample 图像匹配的 GDC 标定文件
    ├── remap_nv12_sample.cpp
    └── resize_nv12_sample.cpp
```

sample 默认从仓库根目录运行，输入图片统一位于 `data/`。sample 也支持通过命令行
传入输入和输出路径，便于替换为相机或其他测试数据。

## 输入数据格式

`data/input_1920x1080.nv12` 是一帧紧凑排列的 NV12 原始图像：

```text
分辨率：1920 x 1080
格式：8-bit NV12（Y plane + interleaved UV plane）
文件大小：1920 x 1080 x 3 / 2 = 3,110,400 bytes
```

文件本身不包含 stride padding。sample 会把每一行的有效像素复制到 SDK 分配的
带 stride buffer 中。使用其他 NV12 文件时，必须保证它的分辨率和 sample 配置一致，
或者同时修改 sample 中的宽高和 buffer 处理逻辑。

## 环境要求

- D-Robotics multimedia SDK，默认安装路径为 `/usr/hobot`；
- C++14 编译器和 CMake 3.10 或更高版本；
- PYM/GDC/Media Codec 对应的 SDK 头文件和动态库；
- `horddt_color`、`resize_nv12`、`color_convert` 还需要 libyuv 和 OpenCV。

GDC 模块需要以下 SDK 组件：

```text
hb_mem_mgr.h
hbn_vpf_interface.h
gdc_cfg.h
gdc_bin_cfg.h
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

如果系统没有 libyuv 或 OpenCV 开发文件，CMake 会跳过色彩相关目标，但仍会构建
`horddt_resize`、`horddt_remap`、`horddt_codec` 及对应的非色彩 sample。

## 编译

在仓库根目录执行：

```bash
cmake -S . -B build
cmake --build build -j
```

如果 SDK 安装在其他位置：

```bash
cmake -S . -B build \
  -DHOBOT_ROOT=/path/to/hobot
cmake --build build -j
```

如果 libyuv 安装在自定义目录：

```bash
cmake -S . -B build \
  -DHOBOT_ROOT=/path/to/hobot \
  -DLIBYUV_ROOT=/path/to/libyuv
cmake --build build -j
```

不需要色彩模块时可以显式关闭：

```bash
cmake -S . -B build -DHORDDT_BUILD_COLOR=OFF
cmake --build build -j
```

构建成功后，常见目标如下：

| 目标 | 功能 | 依赖 |
| --- | --- | --- |
| `remap_nv12` | GDC 畸变矫正 | GDC/VPF SDK |
| `codec_sample` | NV12/JPEG/H.264 硬件编解码 | Media Codec SDK |
| `resize_nv12` | PYM 缩放后转 JPEG | PYM、libyuv、OpenCV |
| `color_convert` | NV12 转 JPEG | libyuv、OpenCV |

## Samples 使用方法

所有 sample 都支持 `-h`/`--help`。不带参数时使用下面列出的默认路径；带参数时，
输入文件和输出文件可以放在任意位置。输出目录不会自动创建，请提前执行
`mkdir -p output`。

### 1. NV12 转 JPEG：`color_convert`

默认将 `data/input_1920x1080.nv12` 转换为当前目录下的
`color_output_1920x1080.jpg`：

```bash
./build/color_convert
```

查看帮助或指定路径：

```bash
./build/color_convert --help
./build/color_convert data/input_1920x1080.nv12 output/color.jpg
```

### 2. PYM 缩放：`resize_nv12`

默认将 1920x1080 缩放为 640x360，并输出 `output_640x360.jpg`：

```bash
./build/resize_nv12
```

指定输入和输出：

```bash
./build/resize_nv12 data/input_1920x1080.nv12 output/resized.jpg
```

当前 sample 的输入尺寸固定为 1920x1080，输出尺寸固定为 640x360；若要使用其他
尺寸，需要同步修改 `samples/resize_nv12_sample.cpp` 中的配置。

### 3. GDC 畸变矫正：`remap_nv12`

默认配置为：

```text
输入：data/input_1920x1080.nv12
GDC bin：samples/gdc_1920x1080.bin
输出：remap_output_1920x1080.nv12
```

运行：

```bash
./build/remap_nv12
```

也可以指定输入、GDC bin 和输出文件：

```bash
./build/remap_nv12 \
  data/input_1920x1080.nv12 \
  samples/gdc_1920x1080.bin \
  output/remap.nv12
```

GDC bin 不只是分辨率配置，还包含镜头和标定参数。实际部署时必须使用与摄像头、
镜头、输入输出尺寸匹配的 bin，否则结果可能无效或节点初始化失败。

### 4. 硬件编解码：`codec_sample`

默认执行三种转换：

1. NV12 -> JPEG；
2. NV12 -> Annex-B H.264；
3. JPEG -> Annex-B H.264。

默认输入和输出如下：

```text
输入：data/input_1920x1080.nv12
输入：data/input_1920x1080.jpg
输出：./codec_output_1920x1080.jpg
输出：./nv12_output_1920x1080.h264
输出：./jpeg_output_1920x1080.h264
```

运行：

```bash
./build/codec_sample
```

指定两个输入文件和输出目录：

```bash
mkdir -p output
./build/codec_sample \
  data/input_1920x1080.nv12 \
  data/input_1920x1080.jpg \
  output
```

`codec_sample` 会在指定目录写入上述三个固定名称的输出文件。为了让 NV12->H.264
和 JPEG->H.264 的单帧码流都能独立解码，sample 会在两次 H.264 转换之间关闭第一
个 codec 对象，并创建新的 codec 对象。

## C++ 接口说明

### PYM 缩放

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

输入和输出均为调用者分配的 `hb_mem_graphic_buf_t`，格式必须是 NV12。当前 PYM
实现不支持放大，输出尺寸必须满足 SDK 支持的缩放比例。

### GDC 畸变矫正

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

类只管理 GDC 节点和 bin buffer，不释放调用者传入的图像 buffer；类会处理内部
GDC buffer 的 cache flush/invalidate，并按各自 stride 将结果复制到调用者的输出
buffer。

### 色彩转换

`horddt_color` 使用 libyuv 在 CPU 上完成 NV12/NV21 到三通道图像的转换，OpenCV
只负责承载输出 `cv::Mat`。支持：

```text
NV12 -> BGR
NV21 -> BGR
NV12/NV21 -> RGB
```

输入 buffer 必须提供 CPU 可访问的 `virt_addr[0]` 和 `virt_addr[1]`。如果输入由
VIO、VSE、GDC、Codec 等硬件模块写入，调用者应在转换前按 buffer 缓存属性完成
必要的 cache 同步。libyuv 是 CPU/SIMD 软件转换，不依赖 Nano2D。

### 硬件编解码

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
codec.nv12_to_jpeg(input_nv12, jpeg);
codec.nv12_to_h264(input_nv12, h264);
codec.jpeg_to_h264(jpeg_data, jpeg_size, h264);
```

JPEG/H.264 输出保存在 `std::vector<std::uint8_t>` 中，H.264 输出为 Annex-B 码流。
类不会释放调用者传入的 `hb_mem_graphic_buf_t`。

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
