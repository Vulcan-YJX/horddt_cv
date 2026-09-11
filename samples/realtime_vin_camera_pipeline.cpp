/*
 * yx_s397_6010 GMSL 摄像头实时处理示例。
 *
 * 数据路径：
 *   Camera/Deserializer -> VIN -> [GDC 去畸变，可选] -> PYM 缩放
 *                                                     |- H.264 编码并写文件
 *                                                     |- JPEG 编码并写 MJPEG 文件
 *                                                     `- NV12 转 BGR
 *
 * VIN、GDC 和 PYM 输出均以 borrowed buffer 的形式向下游传递。所有消费者
 * 必须在对应回调返回前完成，随后才可把 buffer 归还给 SDK；因此主数据路径
 * 不需要创建应用层 NV12 中间副本。
 *
 * H.264 在采集线程执行，JPEG 和颜色转换各使用一个常驻 worker，使同一帧的
 * 三个分支可以并行处理，同时避免逐帧创建/销毁线程。
 */
#include "horddt_codec.hpp"
#include "horddt_color.hpp"
#include "horddt_remap.hpp"
#include "horddt_resize.hpp"

#include <opencv2/core.hpp>

extern "C" {
#include "hb_camera_interface.h"
#include "hb_deserial_interface.h"
#include "hbn_vpf_data_info.h"
#include "hbn_vpf_interface.h"
}

#include "camera/yx_s397_6010_sensor.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

// 摄像头/VIN 固定输出为 1088x2560 NV12，PYM 将其缩放到一半尺寸。
constexpr std::uint32_t kCaptureWidth = 1088U;
constexpr std::uint32_t kCaptureHeight = 2560U;
constexpr std::uint32_t kProcessWidth = 544U;
constexpr std::uint32_t kProcessHeight = 1280U;

// VIN 使用多个连续物理内存 buffer，避免某帧处理中阻塞摄像头采集。
constexpr std::uint32_t kCameraBufferCount = 6U;
constexpr int kLinkPort = 0;
constexpr int kAutoAllocId = -1;
constexpr std::uint32_t kVinMagicNumber = 0x12345678U;

// 编码器和各硬件算子的运行参数。
constexpr std::uint32_t kFrameRate = 30U;
constexpr std::uint32_t kBitRateKbps = 4096U;
constexpr int kJpegQuality = 85;
constexpr int kTimeoutMs = 1000;

// Deserializer 尚未就绪时 SDK 返回 -65672，等待后重建整条 Camera/VIN 链路。
constexpr int kDeserialAttachRetryError = -65672;
constexpr unsigned int kCameraRetryDelaySeconds = 3U;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

std::uint32_t align16(std::uint32_t value) { return (value + 15U) & ~15U; }

// hbmem 是所有硬件图像 buffer 的底层运行时。该 RAII 对象只管理模块级
// open/close；每一帧的 buffer 生命周期仍由 VIN/GDC/PYM 各自管理。
class hbmem_runtime {
public:
    hbmem_runtime() : status_(hb_mem_module_open()) {
        if (status_ != 0) {
            std::fprintf(stderr, "hb_mem_module_open failed: %d\n", status_);
        }
    }

    ~hbmem_runtime() {
        if (status_ == 0) {
            hb_mem_module_close();
        }
    }

    int status() const noexcept { return status_; }

private:
    int status_;
};

// 将编码器回调给出的 bitstream 直接写入文件。
// H.264 文件保存 Annex-B 码流，MJPEG 文件连续保存每个 JPEG 帧。这里使用
// stdio 的 1 MiB 缓冲减少小 NAL/JPEG fragment 导致的 write 系统调用。
class stream_file {
public:
    explicit stream_file(const std::string &path)
        : file_(std::fopen(path.c_str(), "wb")), path_(path) {
        if (file_ == nullptr) {
            std::fprintf(stderr, "Cannot create '%s': %s\n", path.c_str(),
                         std::strerror(errno));
            return;
        }
        // stdio 持有该缓冲区，并在 fclose 时自动 flush。
        std::setvbuf(file_, nullptr, _IOFBF, 1U << 20U);
    }

    ~stream_file() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    bool valid() const noexcept { return file_ != nullptr; }

    int write(const std::uint8_t *data, std::size_t size) {
        if (file_ == nullptr || data == nullptr || size == 0U) {
            return -1;
        }
        if (std::fwrite(data, 1U, size, file_) != size) {
            std::fprintf(stderr, "Failed to write '%s': %s\n", path_.c_str(),
                         std::strerror(errno));
            return -1;
        }
        bytes_ += size;
        return 0;
    }

    std::uint64_t bytes() const noexcept { return bytes_; }

private:
    FILE *file_ = nullptr;
    std::string path_;
    std::uint64_t bytes_ = 0U;
};

extern "C" {
// 部分 SDK 版本的 hb_deserial_interface.h 未导出这两个声明；
// autocube_media 也采用了相同的补充声明方式。
int32_t hbn_deserial_create(deserial_config_t *config,
                            deserial_handle_t *handle);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t deserial,
                                   camera_des_link_t link, vpf_handle_t vin);
}

// 管理 Camera、MAX96712 Deserializer、VIN vnode 和 VFlow 的完整生命周期。
// acquire() 获得的帧归 VIN 所有，调用方必须在所有下游读取结束后调用
// release()，不能把 image/buffer 指针保存在当前帧处理范围之外。
class vin_gmsl_camera {
public:
    struct frame {
        // image 只描述 VIN buffer，不拥有图像内存。
        hbn_vnode_image_t image{};
        bool acquired = false;
    };

    ~vin_gmsl_camera() { close_camera(); }

    int open_camera(std::uint32_t buffer_count) {
        if (camera_created_ || vin_opened_ || vflow_created_ ||
            deserial_created_) {
            return -1;
        }
        if (buffer_count < 2U) {
            std::fprintf(stderr, "VIN needs at least two output buffers.\n");
            return -1;
        }

        // 1. 复制静态 sensor 配置，后续只修改本实例的副本。
        const horddt_vin_sensor_config_t &sensor =
            horddt_yx_s397_6010_linear_1088x2560_yuv422_30fps;
        camera_config_ = *sensor.camera_config;
        deserial_config_ = *sensor.deserial_config;
        vin_node_attr_ = *sensor.vin_node_attr;
        vin_ichn_attr_ = *sensor.vin_ichn_attr;
        vin_ochn_attr_ = *sensor.vin_ochn_attr;

        // 2. 与 autocube_media 保持一致：每条 GMSL link 使用基地址 + 1 + link
        //    的 serializer/sensor/eeprom alias。本示例固定使用 link 0。
        camera_config_.addr =
            static_cast<std::uint8_t>(camera_config_.addr + 1 + kLinkPort);
        camera_config_.serial_addr = static_cast<std::uint8_t>(
            camera_config_.serial_addr + 1 + kLinkPort);
        camera_config_.eeprom_addr = static_cast<std::uint8_t>(
            camera_config_.eeprom_addr + 1 + kLinkPort);

        // 3. 创建 Camera 对象，此时尚未启动采流。
        int ret = hbn_camera_create(&camera_config_, &camera_handle_);
        if (check("hbn_camera_create", ret) != 0) {
            close_camera();
            return ret;
        }
        camera_created_ = true;

        // 4. 配置 VIN：关闭 ISP fly-by，让图像输出到 DDR；vc_index 对应
        //    GMSL link 0。Sensor 输入为 YUV422，VIN DDR 输出配置来自头文件。
        vin_node_attr_.magicNumber = kVinMagicNumber;
        vin_node_attr_.cim_attr.cim_isp_flyby = 0;
        vin_node_attr_.cim_attr.vc_index = kLinkPort;
        vin_ochn_attr_.magicNumber = kVinMagicNumber;
        vin_ochn_attr_.ddr_en = 1;

        ret = hbn_vnode_open(HB_VIN, vin_node_attr_.cim_attr.mipi_rx,
                             kAutoAllocId, &vin_handle_);
        if (check("hbn_vnode_open(HB_VIN)", ret) != 0) {
            close_camera();
            return ret;
        }
        vin_opened_ = true;

        ret = hbn_vnode_set_attr(vin_handle_, &vin_node_attr_);
        if (check("hbn_vnode_set_attr(VIN)", ret) != 0) {
            close_camera();
            return ret;
        }
        ret = hbn_vnode_set_ichn_attr(vin_handle_, 0, &vin_ichn_attr_);
        if (check("hbn_vnode_set_ichn_attr(VIN)", ret) != 0) {
            close_camera();
            return ret;
        }
        ret = hbn_vnode_set_ochn_attr(vin_handle_, 0, &vin_ochn_attr_);
        if (check("hbn_vnode_set_ochn_attr(VIN)", ret) != 0) {
            close_camera();
            return ret;
        }

        // 5. 为 VIN 输出申请连续、cached 的 DDR buffer 池。多 buffer 可让
        //    Camera DMA 与当前帧处理重叠，降低采集链路被阻塞的概率。
        hbn_buf_alloc_attr_t buffer_attr{};
        buffer_attr.buffers_num = static_cast<int>(buffer_count);
        buffer_attr.is_contig = 1;
        buffer_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
                            HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
        ret = hbn_vnode_set_ochn_buf_attr(vin_handle_, 0, &buffer_attr);
        if (check("hbn_vnode_set_ochn_buf_attr(VIN)", ret) != 0) {
            close_camera();
            return ret;
        }

        // 6. 创建 VFlow，并将 VIN vnode 加入该执行流。
        ret = hbn_vflow_create(&vflow_handle_);
        if (check("hbn_vflow_create", ret) != 0) {
            close_camera();
            return ret;
        }
        vflow_created_ = true;

        ret = hbn_vflow_add_vnode(vflow_handle_, vin_handle_);
        if (check("hbn_vflow_add_vnode(VIN)", ret) != 0) {
            close_camera();
            return ret;
        }

        // 7. 创建 MAX96712 Deserializer。
        ret = hbn_deserial_create(&deserial_config_, &deserial_handle_);
        if (check("hbn_deserial_create", ret) != 0) {
            close_camera();
            return ret;
        }
        deserial_created_ = true;

        // 8. 按 Camera -> Deserializer -> VIN 的顺序连接硬件节点。
        ret = hbn_camera_attach_to_deserial(
            camera_handle_, deserial_handle_,
            static_cast<camera_des_link_t>(kLinkPort));
        if (check("hbn_camera_attach_to_deserial", ret) != 0) {
            close_camera();
            return ret;
        }
        ret = hbn_deserial_attach_to_vin(
            deserial_handle_, static_cast<camera_des_link_t>(kLinkPort),
            vin_handle_);
        if (check("hbn_deserial_attach_to_vin", ret) != 0) {
            close_camera();
            return ret;
        }
        return 0;
    }

    int start() {
        if (!vflow_created_ || streaming_) {
            return -1;
        }
        const int ret = hbn_vflow_start(vflow_handle_);
        if (check("hbn_vflow_start", ret) != 0) {
            return ret;
        }
        streaming_ = true;
        return 0;
    }

    int stop() {
        if (!streaming_) {
            return 0;
        }
        const int ret = hbn_vflow_stop(vflow_handle_);
        if (check("hbn_vflow_stop", ret) == 0) {
            streaming_ = false;
        }
        return ret;
    }

    int acquire(frame &captured, int timeout_ms) {
        if (!streaming_ || captured.acquired) {
            std::fprintf(stderr, "VIN stream/frame state is invalid.\n");
            return -1;
        }
        std::memset(&captured.image, 0, sizeof(captured.image));
        // getframe 只是借出 VIN buffer；成功后必须与 releaseframe 成对。
        const int ret =
            hbn_vnode_getframe(vin_handle_, 0, timeout_ms, &captured.image);
        if (ret != 0) {
            (void)check("hbn_vnode_getframe(VIN)", ret);
            return -1;
        }
        captured.acquired = true;

        // 在进入后续算子前验证格式和 stride，防止按错误布局读取内存。
        const hb_mem_graphic_buf_t &buffer = captured.image.buffer;
        if (buffer.format != MEM_PIX_FMT_NV12 || buffer.plane_cnt < 2 ||
            buffer.width != static_cast<int>(kCaptureWidth) ||
            buffer.height != static_cast<int>(kCaptureHeight) ||
            buffer.stride != static_cast<int>(align16(kCaptureWidth))) {
            std::fprintf(
                stderr,
                "Unexpected VIN buffer: format=%d planes=%d size=%dx%d "
                "stride=%d; expected NV12 %ux%u stride=%u.\n",
                buffer.format, buffer.plane_cnt, buffer.width, buffer.height,
                buffer.stride, kCaptureWidth, kCaptureHeight,
                align16(kCaptureWidth));
            const int release_ret = release(captured);
            return release_ret != 0 ? release_ret : -1;
        }
        return 0;
    }

    int release(frame &captured) {
        if (!captured.acquired) {
            return 0;
        }
        // 只有 GDC/PYM/Codec/Color 都不再读取此帧时才能归还给 VIN。
        const int ret = hbn_vnode_releaseframe(vin_handle_, 0, &captured.image);
        if (check("hbn_vnode_releaseframe(VIN)", ret) == 0) {
            captured.acquired = false;
        }
        return ret;
    }

private:
    static int check(const char *operation, int ret) {
        if (ret != 0) {
            std::fprintf(stderr, "%s failed: %d\n", operation, ret);
        }
        return ret;
    }

    void close_camera() {
        (void)stop();
        // 沿用 autocube_media 验证过的 teardown 顺序。正常处理和 attach
        // 重试都会进入这里，并且此时不能存在仍被下游借用的 VIN 帧。
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

// 单槽、同步回收的常驻 worker。
//
// 它不是跨帧任务队列：每个 worker 同时最多处理一个任务。主线程对当前帧
// submit 后，必须在 PYM borrowed callback 返回前 wait。这样既能让同一帧的
// Color/JPEG/H.264 并行，又不会让 worker 持有已经归还给 PYM 的悬空指针，
// 也不会因积压多帧任务而耗尽硬件 buffer 池。
class synchronous_worker {
public:
    synchronous_worker() : thread_(&synchronous_worker::run, this) {}

    ~synchronous_worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            ready_ = true;
        }
        ready_cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    synchronous_worker(const synchronous_worker &) = delete;
    synchronous_worker &operator=(const synchronous_worker &) = delete;

    void submit(std::function<int()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_ = std::move(task);
        result_ = -1;
        ready_ = true;
        done_ = false;
        ready_cv_.notify_one();
    }

    int wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this] { return done_; });
        return result_;
    }

private:
    void run() {
        for (;;) {
            std::function<int()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_cv_.wait(lock, [this] { return ready_; });
                if (stopping_) {
                    return;
                }
                task = std::move(task_);
                ready_ = false;
            }

            int result = -1;
            try {
                result = task ? task() : -1;
            } catch (...) {
                std::fprintf(stderr, "Pipeline worker threw an exception.\n");
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_ = result;
                done_ = true;
            }
            done_cv_.notify_one();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::condition_variable done_cv_;
    std::function<int()> task_;
    std::thread thread_;
    int result_ = -1;
    bool ready_ = false;
    bool done_ = true;
    bool stopping_ = false;
};

struct options {
    // 默认 GDC 文件不存在时允许旁路；用户显式传入的路径不存在则报错。
    std::string gdc_bin = "samples/gdc_1088x2560.bin";
    std::string h264_path = "vin_camera_output.h264";
    std::string mjpeg_path = "vin_camera_snapshots.mjpg";
    // 0 表示持续运行到收到 SIGINT/SIGTERM。
    std::uint64_t frame_limit = 0U;
    // 分支执行周期：1=每帧，N=每 N 帧，0=完全禁用该分支。
    std::uint32_t color_every = 1U;
    std::uint32_t jpeg_every = 30U;
    bool gdc_explicit = false;
};

bool file_readable(const std::string &path) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

bool wait_for_camera_retry() {
    // 分段 sleep，使 Ctrl-C 最多约 100 ms 就能中断三秒重试等待。
    constexpr unsigned int kSlicesPerSecond = 10U;
    const unsigned int slices =
        kCameraRetryDelaySeconds * kSlicesPerSecond;
    for (unsigned int index = 0; index < slices && g_stop == 0; ++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return g_stop == 0;
}

bool parse_unsigned(const char *text, std::uint64_t &value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

void print_usage(const char *program) {
    std::fprintf(
        stderr,
        "Usage: %s [gdc_1088x2560.bin output.h264 snapshots.mjpg "
        "frame_limit color_every jpeg_every]\n"
        "Defaults: samples/gdc_1088x2560.bin vin_camera_output.h264 "
        "vin_camera_snapshots.mjpg 0 1 30\n"
        "  A missing default GDC file is bypassed; an explicit path is "
        "required to exist.\n"
        "  Uses yx_s397_6010 GMSL link 0, 1088x2560@30, MIPI RX 4.\n"
        "  frame_limit=0 runs until Ctrl-C. color_every/jpeg_every=0 disables "
        "that branch.\n",
        program);
}

int parse_options(int argc, char **argv, options &result) {
    if (argc == 2 && (std::strcmp(argv[1], "-h") == 0 ||
                      std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc != 1 && argc != 2 && argc != 7) {
        print_usage(argv[0]);
        return -1;
    }
    if (argc == 1) {
        return 0;
    }
    result.gdc_bin = argv[1];
    result.gdc_explicit = true;
    if (argc == 2) {
        return 0;
    }
    result.h264_path = argv[2];
    result.mjpeg_path = argv[3];
    std::uint64_t color_every = 0U;
    std::uint64_t jpeg_every = 0U;
    if (!parse_unsigned(argv[4], result.frame_limit) ||
        !parse_unsigned(argv[5], color_every) ||
        !parse_unsigned(argv[6], jpeg_every) || color_every > UINT32_MAX ||
        jpeg_every > UINT32_MAX) {
        print_usage(argv[0]);
        return -1;
    }
    result.color_every = static_cast<std::uint32_t>(color_every);
    result.jpeg_every = static_cast<std::uint32_t>(jpeg_every);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // 阶段 1：解析命令行并安装退出信号。无参数时直接使用默认配置运行。
    options opts;
    const int parse_ret = parse_options(argc, argv, opts);
    if (parse_ret != 0) {
        return parse_ret > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // 阶段 2：决定是否启用 GDC。默认标定文件缺失时仍可运行 VIN -> PYM；
    // 显式路径通常代表用户要求特定标定，因此缺失时立即失败。
    const bool gdc_enabled = file_readable(opts.gdc_bin);
    if (!gdc_enabled && opts.gdc_explicit) {
        std::fprintf(stderr,
                     "Cannot open explicitly requested GDC binary '%s': %s\n",
                     opts.gdc_bin.c_str(), std::strerror(errno));
        return EXIT_FAILURE;
    }
    if (!gdc_enabled) {
        std::fprintf(
            stderr,
            "Warning: default GDC binary '%s' is unavailable; continuing "
            "with VIN -> PYM bypass. Supply a matching 1088x2560 GDC bin "
            "to enable distortion correction.\n",
            opts.gdc_bin.c_str());
    }

    std::printf("VIN pipeline: gdc=%s h264=%s mjpeg=%s "
                "frame_limit=%llu color_every=%u jpeg_every=%u\n",
                gdc_enabled ? opts.gdc_bin.c_str() : "bypass",
                opts.h264_path.c_str(), opts.mjpeg_path.c_str(),
                static_cast<unsigned long long>(opts.frame_limit),
                opts.color_every, opts.jpeg_every);

    // 阶段 3：初始化 hbmem 和所有长生命周期算子上下文。上下文只创建一次，
    // 主循环中反复复用，避免每帧分配硬件资源或重建编码器。
    hbmem_runtime memory;
    if (memory.status() != 0) {
        return EXIT_FAILURE;
    }

    horddt_remap::config remap_cfg;
    remap_cfg.input_width = kCaptureWidth;
    remap_cfg.input_height = kCaptureHeight;
    remap_cfg.output_width = kCaptureWidth;
    remap_cfg.output_height = kCaptureHeight;
    remap_cfg.input_stride = align16(kCaptureWidth);
    remap_cfg.output_stride = align16(kCaptureWidth);
    remap_cfg.gdc_bin_path = opts.gdc_bin;
    remap_cfg.timeout_ms = kTimeoutMs;
    remap_cfg.output_buffer_count = 2U;
    // 输入刚由 VIN DMA 写入，CPU 未修改，因此不需要额外 clean cache。
    remap_cfg.sync_input_for_device = false;
    // GDC 输出紧接着由 PYM 硬件读取，CPU 不访问，无需 invalidate cache。
    remap_cfg.sync_borrowed_output_for_cpu = false;

    horddt_resize::config resize_cfg;
    resize_cfg.input_width = kCaptureWidth;
    resize_cfg.input_height = kCaptureHeight;
    resize_cfg.output_width = kProcessWidth;
    resize_cfg.output_height = kProcessHeight;
    resize_cfg.timeout_ms = kTimeoutMs;
    resize_cfg.enable_file_io = false;
    resize_cfg.output_buffer_count = 2U;
    resize_cfg.feedback_buffer_count = 2U;
    resize_cfg.enable_extra_layers = false;
    // 输入由 GDC DMA 写入（旁路时由 VIN DMA 写入），CPU 未修改。
    resize_cfg.sync_input_for_device = false;
    // PYM 输出会被 libyuv/codec 路径读取，借出前需同步到 CPU 可见。
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

    // GDC 和 Color/JPEG 分支按配置按需创建；禁用后不占用对应资源。
    std::unique_ptr<horddt_remap> remap;
    if (gdc_enabled) {
        remap.reset(new horddt_remap(remap_cfg));
    }
    horddt_resize resize(resize_cfg);
    std::unique_ptr<horddt_color> color;
    if (opts.color_every != 0U) {
        color.reset(new horddt_color());
    }
    horddt_codec h264_codec(h264_cfg);
    std::unique_ptr<horddt_codec> jpeg_codec;
    if (opts.jpeg_every != 0U) {
        jpeg_codec.reset(new horddt_codec(jpeg_cfg));
    }
    // 所有算子必须在启动摄像头前完成初始化，避免 VIN 开始出帧后再进行
    // 慢速初始化并长期占住有限的 VIN buffer。
    if ((remap && !remap->is_initialized()) || !resize.is_initialized() ||
        (color && !color->is_initialized()) || !h264_codec.is_initialized() ||
        (jpeg_codec && !jpeg_codec->is_initialized())) {
        std::fprintf(stderr,
                     "Operator initialization failed: gdc=%d pym=%d "
                     "color=%d h264=%d jpeg=%d\n",
                     remap ? remap->initialization_status() : 0,
                     resize.initialization_status(),
                     color ? color->initialization_status() : 0,
                     h264_codec.initialization_status(),
                     jpeg_codec ? jpeg_codec->initialization_status() : 0);
        return EXIT_FAILURE;
    }

    // 阶段 4：创建输出文件和常驻 worker。编码回调中的 bitstream 直接写盘，
    // 不先复制到 std::vector；禁用的分支不会创建输出文件或 worker。
    stream_file h264_output(opts.h264_path);
    std::unique_ptr<stream_file> jpeg_output;
    if (opts.jpeg_every != 0U) {
        jpeg_output.reset(new stream_file(opts.mjpeg_path));
    }
    if (!h264_output.valid() || (jpeg_output && !jpeg_output->valid())) {
        return EXIT_FAILURE;
    }

    cv::Mat bgr;
    if (opts.color_every != 0U) {
        bgr.create(static_cast<int>(kProcessHeight),
                   static_cast<int>(kProcessWidth), CV_8UC3);
    }
    std::unique_ptr<synchronous_worker> color_worker;
    std::unique_ptr<synchronous_worker> jpeg_worker;
    if (color) {
        color_worker.reset(new synchronous_worker());
    }
    if (jpeg_codec) {
        jpeg_worker.reset(new synchronous_worker());
    }
    // 阶段 5：创建 Camera/Deserializer/VIN。-65672 通常表示 MAX96712 链路
    // 暂时未就绪；与 autocube_media 一致，销毁未完成实例、等待三秒，再从头
    // 重建整条采集链路，而不是要求用户手动反复重启进程。
    vin_gmsl_camera camera;
    unsigned int camera_attempt = 1U;
    while (g_stop == 0) {
        std::printf("Starting yx_s397_6010 GMSL link 0, attempt %u\n",
                    camera_attempt);
        const int camera_ret = camera.open_camera(kCameraBufferCount);
        if (camera_ret == 0) {
            break;
        }
        if (camera_ret != kDeserialAttachRetryError) {
            return EXIT_FAILURE;
        }
        std::fprintf(
            stderr,
            "hbn_deserial_attach_to_vin returned %d; retrying in %u "
            "seconds. Press Ctrl-C to stop.\n",
            camera_ret, kCameraRetryDelaySeconds);
        if (!wait_for_camera_retry()) {
            return EXIT_SUCCESS;
        }
        ++camera_attempt;
    }
    if (g_stop != 0) {
        return EXIT_SUCCESS;
    }

    // 所有持久化上下文、文件和 worker 就绪后再启动 VIN，避免初始化期间
    // 占住 Camera/VIN buffer。
    if (camera.start() != 0) {
        return EXIT_FAILURE;
    }

    std::uint64_t frame_count = 0U;
    std::uint64_t color_count = 0U;
    std::uint64_t jpeg_count = 0U;
    auto report_start = std::chrono::steady_clock::now();
    std::uint64_t report_frames = 0U;

    // 阶段 6：逐帧执行实时 pipeline。这里采用“帧内并行、帧间同步”：
    // 当前帧的所有分支结束并归还 VIN buffer 后，才获取下一帧。
    while (g_stop == 0 &&
           (opts.frame_limit == 0U || frame_count < opts.frame_limit)) {
        vin_gmsl_camera::frame camera_frame;
        // 从 VIN 借出一帧。后面的异常路径也必须执行 camera.release()。
        const int acquire_ret = camera.acquire(camera_frame, kTimeoutMs);
        if (acquire_ret != 0) {
            break;
        }

        // 使用获取成功的帧序号决定低频分支是否执行。例如 jpeg_every=30
        // 表示第 0、30、60... 帧生成快照；值为 0 时完全关闭该分支。
        const std::uint64_t current_frame = frame_count;
        int process_ret = -1;
        try {
            // process_pym 同时服务于两条输入路径：GDC 输出或 VIN 直接旁路。
            // 传入和传出的 hb_mem_graphic_buf_t 都只是 buffer 描述符，不复制
            // NV12 像素数据。
            const auto process_pym =
                [&](const hb_mem_graphic_buf_t &pym_input) {
                    return resize.resize_borrowed(
                        pym_input, [&](const hb_mem_graphic_buf_t &pym_frame) {
                            // PYM 输出进入回调前只做一次 cache invalidate。
                            // 三个分支共同只读同一个 buffer，不创建应用层 NV12
                            // 中间帧；回调返回后 pym_frame 指向的数据立即失效。
                            const bool run_color =
                                opts.color_every != 0U &&
                                current_frame % opts.color_every == 0U;
                            const bool run_jpeg =
                                opts.jpeg_every != 0U &&
                                current_frame % opts.jpeg_every == 0U;

                            bool color_pending = false;
                            bool jpeg_pending = false;
                            int schedule_ret = 0;
                            try {
                                if (run_color) {
                                    color_worker->submit([&color, &pym_frame,
                                                          &bgr] {
                                        return color->convert(
                                            pym_frame, bgr,
                                            horddt_color::output_format::bgr);
                                    });
                                    color_pending = true;
                                }
                                if (run_jpeg) {
                                    jpeg_worker->submit([&jpeg_codec,
                                                         &pym_frame,
                                                         &jpeg_output] {
                                        return jpeg_codec
                                            ->nv12_to_jpeg_borrowed(
                                                pym_frame,
                                                [&jpeg_output](
                                                    const std::uint8_t *data,
                                                    std::size_t size) {
                                                    return jpeg_output->write(
                                                        data, size);
                                                });
                                    });
                                    jpeg_pending = true;
                                }
                            } catch (...) {
                                // 即使后续任务提交失败，已提交 worker 仍在读取
                                // PYM buffer，必须在返回前 wait。
                                std::fprintf(
                                    stderr,
                                    "Failed to schedule a pipeline branch.\n");
                                schedule_ret = -1;
                            }

                            int ret = schedule_ret;
                            if (schedule_ret == 0) {
                                try {
                                    // H.264 在采集线程运行；此时 Color 和 JPEG
                                    // 已在两个常驻 worker 上并行执行。
                                    ret = h264_codec.nv12_to_h264_borrowed(
                                        pym_frame, [&](const std::uint8_t *data,
                                                       std::size_t size) {
                                            return h264_output.write(data,
                                                                     size);
                                        });
                                } catch (...) {
                                    std::fprintf(
                                        stderr,
                                        "H.264 branch threw an exception.\n");
                                    ret = -1;
                                }
                            }

                            // PYM callback 返回即归还 borrowed output，因此正常
                            // 和错误路径都要先等待所有已提交的 reader 完成。
                            const int color_ret =
                                color_pending ? color_worker->wait() : 0;
                            const int jpeg_ret =
                                jpeg_pending ? jpeg_worker->wait() : 0;
                            if (run_color && color_ret == 0) {
                                ++color_count;
                            }
                            if (run_jpeg && jpeg_ret == 0) {
                                ++jpeg_count;
                            }
                            if (ret != 0) {
                                return ret;
                            }
                            if (color_ret != 0) {
                                return color_ret;
                            }
                            return jpeg_ret;
                        });
                };

            if (remap) {
                // GDC borrowed output 只在此回调内有效。process_pym 返回时，
                // PYM 及其三个消费者均已完成，随后 GDC 才归还输出 buffer。
                process_ret = remap->remap_borrowed(
                    camera_frame.image.buffer,
                    [&](const hb_mem_graphic_buf_t &gdc_frame) {
                        return process_pym(gdc_frame);
                    });
            } else {
                // VIN DMA 输出本身就是 hbmem NV12；没有匹配的 GDC 标定文件时
                // 可直接送入 PYM，旁路同样不会产生 CPU 图像拷贝。
                process_ret = process_pym(camera_frame.image.buffer);
            }
        } catch (...) {
            // 算子或回调意外抛异常时仍必须执行 releaseframe，否则 VIN buffer
            // 池会逐渐耗尽并停止出帧。
            std::fprintf(stderr, "Pipeline processing threw an exception.\n");
            process_ret = -1;
        }

        // borrowed 生命周期到此结束：GDC、PYM、H.264、JPEG、Color 均已完成，
        // 现在才能把原始帧归还 VIN。
        const int release_ret = camera.release(camera_frame);
        if (process_ret != 0 || release_ret != 0) {
            std::fprintf(stderr, "Frame %llu failed: process=%d release=%d\n",
                         static_cast<unsigned long long>(frame_count),
                         process_ret, release_ret);
            break;
        }

        ++frame_count;
        ++report_frames;
        const auto now = std::chrono::steady_clock::now();
        const double seconds =
            std::chrono::duration<double>(now - report_start).count();
        if (seconds >= 1.0) {
            std::printf("frames=%llu fps=%.2f h264=%llu bytes jpeg=%llu "
                        "color=%llu\n",
                        static_cast<unsigned long long>(frame_count),
                        static_cast<double>(report_frames) / seconds,
                        static_cast<unsigned long long>(h264_output.bytes()),
                        static_cast<unsigned long long>(jpeg_count),
                        static_cast<unsigned long long>(color_count));
            report_start = now;
            report_frames = 0U;
        }
    }

    // 阶段 7：停止 VFlow；其余对象由 RAII 按构造逆序释放并 flush 输出文件。
    const int stop_ret = camera.stop();
    std::printf(
        "Stopped after %llu frames; H.264=%llu bytes, JPEG frames=%llu, "
        "color frames=%llu.\n",
        static_cast<unsigned long long>(frame_count),
        static_cast<unsigned long long>(h264_output.bytes()),
        static_cast<unsigned long long>(jpeg_count),
        static_cast<unsigned long long>(color_count));
    return frame_count != 0U && stop_ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
