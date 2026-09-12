/*
 * realtime_vin_camera_pipeline 的精简版本。
 *
 * 假设以下默认资源已经准备好，不解析命令行，也不做 GDC 文件和图像格式预检查：
 *   samples/gdc_1920x1080.bin
 *
 * 数据流：
 *   yx_s397_6010 -> MAX96712 -> VIN 1088x2560
 *     -> libyuv NV12 resize 1920x1080 -> GDC 1920x1080
 *     -> Rotate 180° 1920x1080 -> PYM 960x536
 *                                  |- H.264
 *                                  |- JPEG（每 30 帧）
 *                                  `- BGR（每帧）
 *
 * Camera/VIN 最多初始化三次。每次失败都会销毁未完成的链路，等待三秒后重试；
 * 三次全部失败才退出。GDC/Rotate/PYM 使用 borrowed callback，回调返回前完成
 * 所有下游操作，不保存图像指针，也不创建额外的应用层 NV12 中间副本。
 */
#include "horddt_codec.hpp"
#include "horddt_color.hpp"
#include "horddt_remap.hpp"
#include "horddt_resize.hpp"
#include "horddt_rotate.hpp"

#include <libyuv/scale.h>
#include <opencv2/core.hpp>

extern "C" {
#include "hb_camera_interface.h"
#include "hb_deserial_interface.h"
#include "hbn_vpf_data_info.h"
#include "hbn_vpf_interface.h"
}

#include "camera/yx_s397_6010_sensor.h"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

constexpr std::uint32_t kCaptureWidth = 1088U;
constexpr std::uint32_t kCaptureHeight = 2560U;
constexpr std::uint32_t kRemapWidth = 1920U;
constexpr std::uint32_t kRemapHeight = 1080U;
constexpr std::uint32_t kProcessWidth = 960U;
// Codec 要求宽度 16 对齐、高度 8 对齐。540 不能被 8 整除，因此使用 536。
constexpr std::uint32_t kProcessHeight = 536U;
static_assert(kProcessWidth % 16U == 0U, "codec width must be 16 aligned");
static_assert(kProcessHeight % 8U == 0U, "codec height must be 8 aligned");
constexpr std::uint32_t kCameraBufferCount = 6U;
constexpr std::uint32_t kFrameRate = 30U;
constexpr std::uint32_t kBitRateKbps = 4096U;
constexpr std::uint32_t kJpegEvery = 30U;
constexpr int kJpegQuality = 85;
constexpr int kTimeoutMs = 1000;
constexpr int kLinkPort = 0;
constexpr int kAutoAllocId = -1;
constexpr std::uint32_t kVinMagicNumber = 0x12345678U;
constexpr unsigned int kCameraOpenAttempts = 3U;
constexpr unsigned int kRetryDelaySeconds = 3U;
constexpr const char *kGdcPath = "samples/gdc_1920x1080.bin";
constexpr const char *kH264Path = "vin_camera_output.h264";
constexpr const char *kMjpegPath = "vin_camera_snapshots.mjpg";

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

std::uint32_t align16(std::uint32_t value) { return (value + 15U) & ~15U; }

std::uint32_t align64(std::uint32_t value) { return (value + 63U) & ~63U; }

class hbmem_runtime {
public:
    hbmem_runtime() : status_(hb_mem_module_open()) {}
    ~hbmem_runtime() {
        if (status_ == 0) {
            hb_mem_module_close();
        }
    }

    int status() const noexcept { return status_; }

private:
    int status_;
};

// PYM 只能缩小图像，不能把 1088 宽放大到 1920，因此第一段 resize 使用
// libyuv。输出 hbmem buffer 只申请一次并逐帧复用；CPU 写完后 flush，随后直接
// 交给 GDC，不再创建第二份 1920x1080 NV12 副本。
class cpu_nv12_resize {
public:
    cpu_nv12_resize() {
        constexpr std::int64_t flags =
            HB_MEM_USAGE_MAP_INITIALIZED |
            HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
            HB_MEM_USAGE_CPU_READ_OFTEN |
            HB_MEM_USAGE_CPU_WRITE_OFTEN |
            HB_MEM_USAGE_CACHED |
            HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
        status_ = hb_mem_alloc_graph_buf(
            kRemapWidth, kRemapHeight, MEM_PIX_FMT_NV12, flags,
            align64(kRemapWidth), kRemapHeight, &output_);
        allocated_ = status_ == 0;
    }

    ~cpu_nv12_resize() {
        if (allocated_) {
            (void)hb_mem_free_buf(output_.fd[0]);
        }
    }

    int status() const noexcept { return status_; }

    const hb_mem_graphic_buf_t &output() const noexcept { return output_; }

    int resize(const hb_mem_graphic_buf_t &input) {
        if (status_ != 0) {
            return status_;
        }

        // VIN 刚通过 DMA 写入 cached buffer；CPU 读取前先 invalidate。
        int ret = sync_for_cpu(input);
        if (ret != 0) {
            return ret;
        }

        ret = libyuv::NV12Scale(
            static_cast<const std::uint8_t *>(input.virt_addr[0]),
            input.stride,
            static_cast<const std::uint8_t *>(input.virt_addr[1]),
            input.stride, static_cast<int>(kCaptureWidth),
            static_cast<int>(kCaptureHeight),
            static_cast<std::uint8_t *>(output_.virt_addr[0]), output_.stride,
            static_cast<std::uint8_t *>(output_.virt_addr[1]), output_.stride,
            static_cast<int>(kRemapWidth), static_cast<int>(kRemapHeight),
            libyuv::kFilterBilinear);
        if (ret != 0) {
            return ret;
        }

        // 输出由 CPU 写入，GDC DMA 读取前需要 clean cache。
        return sync_for_device(output_);
    }

private:
    static int sync_for_cpu(const hb_mem_graphic_buf_t &buffer) {
        const std::uint64_t y_bytes =
            static_cast<std::uint64_t>(buffer.stride) * buffer.height;
        const std::uint64_t uv_bytes = y_bytes / 2U;
        int ret = hb_mem_invalidate_buf_with_vaddr(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                buffer.virt_addr[0])),
            y_bytes);
        if (ret == 0) {
            ret = hb_mem_invalidate_buf_with_vaddr(
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                    buffer.virt_addr[1])),
                uv_bytes);
        }
        return ret;
    }

    static int sync_for_device(const hb_mem_graphic_buf_t &buffer) {
        const std::uint64_t y_bytes =
            static_cast<std::uint64_t>(buffer.stride) * buffer.height;
        const std::uint64_t uv_bytes = y_bytes / 2U;
        int ret = hb_mem_flush_buf_with_vaddr(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                buffer.virt_addr[0])),
            y_bytes);
        if (ret == 0) {
            ret = hb_mem_flush_buf_with_vaddr(
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                    buffer.virt_addr[1])),
                uv_bytes);
        }
        return ret;
    }

    hb_mem_graphic_buf_t output_{};
    int status_ = -1;
    bool allocated_ = false;
};

extern "C" {
// 某些 SDK 头文件没有导出这两个接口声明。
int32_t hbn_deserial_create(deserial_config_t *config,
                            deserial_handle_t *handle);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t deserial,
                                   camera_des_link_t link, vpf_handle_t vin);
}

// 只保留本示例需要的 Camera/Deserializer/VIN 生命周期管理。
class simple_vin_camera {
public:
    ~simple_vin_camera() { close(); }

    int open() {
        const horddt_vin_sensor_config_t &sensor =
            horddt_yx_s397_6010_linear_1088x2560_yuv422_30fps;
        camera_config_ = *sensor.camera_config;
        deserial_config_ = *sensor.deserial_config;
        vin_node_attr_ = *sensor.vin_node_attr;
        vin_ichn_attr_ = *sensor.vin_ichn_attr;
        vin_ochn_attr_ = *sensor.vin_ochn_attr;

        // link 0 使用 sensor 配置基地址之后的第一组 alias。
        camera_config_.addr =
            static_cast<std::uint8_t>(camera_config_.addr + 1 + kLinkPort);
        camera_config_.serial_addr = static_cast<std::uint8_t>(
            camera_config_.serial_addr + 1 + kLinkPort);
        camera_config_.eeprom_addr = static_cast<std::uint8_t>(
            camera_config_.eeprom_addr + 1 + kLinkPort);

        int ret = hbn_camera_create(&camera_config_, &camera_handle_);
        if (ret != 0) {
            return fail("hbn_camera_create", ret);
        }
        camera_created_ = true;

        vin_node_attr_.magicNumber = kVinMagicNumber;
        vin_node_attr_.cim_attr.cim_isp_flyby = 0;
        vin_node_attr_.cim_attr.vc_index = kLinkPort;
        vin_ochn_attr_.magicNumber = kVinMagicNumber;
        vin_ochn_attr_.ddr_en = 1;

        ret = hbn_vnode_open(HB_VIN, vin_node_attr_.cim_attr.mipi_rx,
                             kAutoAllocId, &vin_handle_);
        if (ret != 0) {
            return fail("hbn_vnode_open(HB_VIN)", ret);
        }
        vin_opened_ = true;

        ret = hbn_vnode_set_attr(vin_handle_, &vin_node_attr_);
        if (ret != 0) {
            return fail("hbn_vnode_set_attr(VIN)", ret);
        }
        ret = hbn_vnode_set_ichn_attr(vin_handle_, 0, &vin_ichn_attr_);
        if (ret != 0) {
            return fail("hbn_vnode_set_ichn_attr(VIN)", ret);
        }
        ret = hbn_vnode_set_ochn_attr(vin_handle_, 0, &vin_ochn_attr_);
        if (ret != 0) {
            return fail("hbn_vnode_set_ochn_attr(VIN)", ret);
        }

        hbn_buf_alloc_attr_t buffer_attr{};
        buffer_attr.buffers_num = static_cast<int>(kCameraBufferCount);
        buffer_attr.is_contig = 1;
        buffer_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
                            HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
        ret = hbn_vnode_set_ochn_buf_attr(vin_handle_, 0, &buffer_attr);
        if (ret != 0) {
            return fail("hbn_vnode_set_ochn_buf_attr(VIN)", ret);
        }

        ret = hbn_vflow_create(&vflow_handle_);
        if (ret != 0) {
            return fail("hbn_vflow_create", ret);
        }
        vflow_created_ = true;

        ret = hbn_vflow_add_vnode(vflow_handle_, vin_handle_);
        if (ret != 0) {
            return fail("hbn_vflow_add_vnode(VIN)", ret);
        }

        ret = hbn_deserial_create(&deserial_config_, &deserial_handle_);
        if (ret != 0) {
            return fail("hbn_deserial_create", ret);
        }
        deserial_created_ = true;

        ret = hbn_camera_attach_to_deserial(
            camera_handle_, deserial_handle_,
            static_cast<camera_des_link_t>(kLinkPort));
        if (ret != 0) {
            return fail("hbn_camera_attach_to_deserial", ret);
        }

        ret = hbn_deserial_attach_to_vin(
            deserial_handle_, static_cast<camera_des_link_t>(kLinkPort),
            vin_handle_);
        if (ret != 0) {
            return fail("hbn_deserial_attach_to_vin", ret);
        }
        return 0;
    }

    int start() {
        const int ret = hbn_vflow_start(vflow_handle_);
        if (ret == 0) {
            streaming_ = true;
        }
        return ret;
    }

    int get(hbn_vnode_image_t &image) {
        std::memset(&image, 0, sizeof(image));
        return hbn_vnode_getframe(vin_handle_, 0, kTimeoutMs, &image);
    }

    int put(hbn_vnode_image_t &image) {
        return hbn_vnode_releaseframe(vin_handle_, 0, &image);
    }

    void close() {
        if (streaming_) {
            (void)hbn_vflow_stop(vflow_handle_);
            streaming_ = false;
        }
        if (vin_opened_) {
            (void)hbn_vnode_close(vin_handle_);
            vin_opened_ = false;
        }
        if (camera_created_) {
            (void)hbn_camera_destroy(camera_handle_);
            camera_created_ = false;
        }
        if (vflow_created_) {
            (void)hbn_vflow_destroy(vflow_handle_);
            vflow_created_ = false;
        }
        if (deserial_created_) {
            (void)hbn_deserial_destroy(deserial_handle_);
            deserial_created_ = false;
        }
    }

private:
    int fail(const char *operation, int ret) {
        std::fprintf(stderr, "%s failed: %d\n", operation, ret);
        close();
        return ret;
    }

    camera_config_t camera_config_{};
    deserial_config_t deserial_config_{};
    vin_node_attr_t vin_node_attr_{};
    vin_ichn_attr_t vin_ichn_attr_{};
    vin_ochn_attr_t vin_ochn_attr_{};
    camera_handle_t camera_handle_{};
    deserial_handle_t deserial_handle_{};
    hbn_vnode_handle_t vin_handle_{};
    hbn_vflow_handle_t vflow_handle_{};
    bool camera_created_ = false;
    bool deserial_created_ = false;
    bool vin_opened_ = false;
    bool vflow_created_ = false;
    bool streaming_ = false;
};

int write_stream(FILE *file, const std::uint8_t *data, std::size_t size) {
    return std::fwrite(data, 1U, size, file) == size ? 0 : -1;
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // 精简版固定使用默认文件和参数，不做命令行、GDC 文件或帧格式检查。
    std::printf("Simple VIN pipeline: VIN 1088x2560 -> resize 1920x1080 "
                "-> GDC(%s) -> rotate 180 -> PYM 960x536 -> %s + %s\n",
                kGdcPath, kH264Path, kMjpegPath);

    hbmem_runtime memory;
    if (memory.status() != 0) {
        return 1;
    }

    int exit_code = 1;
    {
        horddt_remap::config remap_cfg;
        remap_cfg.input_width = kRemapWidth;
        remap_cfg.input_height = kRemapHeight;
        remap_cfg.output_width = kRemapWidth;
        remap_cfg.output_height = kRemapHeight;
        remap_cfg.input_stride = align16(kRemapWidth);
        remap_cfg.output_stride = align16(kRemapWidth);
        remap_cfg.gdc_bin_path = kGdcPath;
        remap_cfg.timeout_ms = kTimeoutMs;
        remap_cfg.output_buffer_count = 2U;
        remap_cfg.sync_input_for_device = false;
        remap_cfg.sync_borrowed_output_for_cpu = false;

        horddt_rotate::config rotate_cfg;
        rotate_cfg.input_width = kRemapWidth;
        rotate_cfg.input_height = kRemapHeight;
        rotate_cfg.rotation = horddt_rotate::angle::rotate_180;
        rotate_cfg.input_stride = align16(kRemapWidth);
        rotate_cfg.output_stride = align16(kRemapWidth);
        rotate_cfg.timeout_ms = kTimeoutMs;
        rotate_cfg.output_buffer_count = 2U;
        // GDC output and Rotate output are passed directly between DMA engines.
        rotate_cfg.sync_input_for_device = false;
        rotate_cfg.sync_borrowed_output_for_cpu = false;

        horddt_resize::config resize_cfg;
        resize_cfg.input_width = kRemapWidth;
        resize_cfg.input_height = kRemapHeight;
        resize_cfg.output_width = kProcessWidth;
        resize_cfg.output_height = kProcessHeight;
        resize_cfg.timeout_ms = kTimeoutMs;
        resize_cfg.enable_file_io = false;
        resize_cfg.output_buffer_count = 2U;
        resize_cfg.feedback_buffer_count = 2U;
        resize_cfg.enable_extra_layers = false;
        resize_cfg.sync_input_for_device = false;
        resize_cfg.sync_borrowed_output_for_cpu = true;

        horddt_codec::config h264_cfg;
        h264_cfg.width = kProcessWidth;
        h264_cfg.height = kProcessHeight;
        h264_cfg.frame_rate = kFrameRate;
        h264_cfg.bit_rate = kBitRateKbps;
        h264_cfg.jpeg_quality = kJpegQuality;
        h264_cfg.timeout_ms = kTimeoutMs;
        h264_cfg.enable_jpeg_encoder = false;
        h264_cfg.enable_h264_encoder = true;
        h264_cfg.enable_jpeg_decoder = false;
        h264_cfg.frame_buffer_count = 2U;
        h264_cfg.bitstream_buffer_count = 2U;

        horddt_codec::config jpeg_cfg = h264_cfg;
        jpeg_cfg.enable_jpeg_encoder = true;
        jpeg_cfg.enable_h264_encoder = false;

        // 所有算子只构造一次，循环中复用其硬件上下文和 buffer 池。
        cpu_nv12_resize input_resize;
        horddt_remap remap(remap_cfg);
        horddt_rotate rotate(rotate_cfg);
        horddt_resize resize(resize_cfg);
        horddt_color color;
        horddt_codec h264(h264_cfg);
        horddt_codec jpeg(jpeg_cfg);
        cv::Mat bgr(static_cast<int>(kProcessHeight),
                    static_cast<int>(kProcessWidth), CV_8UC3);

        FILE *h264_file = std::fopen(kH264Path, "wb");
        FILE *mjpeg_file = std::fopen(kMjpegPath, "wb");
        if (h264_file == nullptr || mjpeg_file == nullptr) {
            if (h264_file != nullptr) {
                std::fclose(h264_file);
            }
            if (mjpeg_file != nullptr) {
                std::fclose(mjpeg_file);
            }
            return 1;
        }

        if (input_resize.status() != 0) {
            std::fprintf(stderr, "Cannot allocate 1920x1080 resize buffer: %d\n",
                         input_resize.status());
            std::fclose(mjpeg_file);
            std::fclose(h264_file);
            return 1;
        }
        if (!rotate.is_initialized()) {
            std::fprintf(stderr, "Cannot initialize 180-degree rotation: %d\n",
                         rotate.initialization_status());
            std::fclose(mjpeg_file);
            std::fclose(h264_file);
            return 1;
        }

        simple_vin_camera camera;
        int camera_ret = -1;
        for (unsigned int attempt = 1U; attempt <= kCameraOpenAttempts;
             ++attempt) {
            std::printf("Open camera attempt %u/%u\n", attempt,
                        kCameraOpenAttempts);
            camera_ret = camera.open();
            if (camera_ret == 0) {
                std::printf("Camera opened on attempt %u/%u\n", attempt,
                            kCameraOpenAttempts);
                break;
            }
            if (attempt < kCameraOpenAttempts) {
                std::this_thread::sleep_for(
                    std::chrono::seconds(kRetryDelaySeconds));
            }
        }

        if (camera_ret == 0 && camera.start() == 0) {
            std::printf("Camera streaming started. Press Ctrl-C to stop.\n");
            std::uint64_t frame_count = 0U;
            while (g_stop == 0) {
                hbn_vnode_image_t vin_frame{};
                if (camera.get(vin_frame) != 0) {
                    break;
                }

                // 1088x2560 VIN 先转换为现有 GDC 文件匹配的 1920x1080。
                int process_ret = input_resize.resize(vin_frame.buffer);
                if (process_ret == 0) {
                    process_ret = remap.remap_borrowed(
                        input_resize.output(),
                        [&](const hb_mem_graphic_buf_t &gdc_frame) {
                            return rotate.rotate_borrowed(
                                gdc_frame,
                                [&](const hb_mem_graphic_buf_t &rotated_frame) {
                                    return resize.resize_borrowed(
                                        rotated_frame,
                                        [&](const hb_mem_graphic_buf_t &pym_frame) {
                                            int ret =
                                                h264.nv12_to_h264_borrowed(
                                                    pym_frame,
                                                    [&](const std::uint8_t *data,
                                                        std::size_t size) {
                                                        return write_stream(
                                                            h264_file, data,
                                                            size);
                                                    });
                                            if (ret != 0) {
                                                return ret;
                                            }

                                            if (frame_count % kJpegEvery ==
                                                0U) {
                                                ret =
                                                    jpeg.nv12_to_jpeg_borrowed(
                                                        pym_frame,
                                                        [&](const std::uint8_t *data,
                                                            std::size_t size) {
                                                            return write_stream(
                                                                mjpeg_file,
                                                                data, size);
                                                        });
                                                if (ret != 0) {
                                                    return ret;
                                                }
                                            }

                                            return color.convert(
                                                pym_frame, bgr,
                                                horddt_color::output_format::bgr);
                                        });
                                });
                        });
                }

                // 所有 borrowed callback 已返回，现在才归还 VIN frame。
                const int release_ret = camera.put(vin_frame);
                if (process_ret != 0 || release_ret != 0) {
                    break;
                }

                ++frame_count;
                if (frame_count % kFrameRate == 0U) {
                    std::printf("processed frames: %llu\n",
                                static_cast<unsigned long long>(frame_count));
                }
            }
            camera.close();
            exit_code = 0;
        }

        std::fclose(mjpeg_file);
        std::fclose(h264_file);
    }

    return exit_code;
}
