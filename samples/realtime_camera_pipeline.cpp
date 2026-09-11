#include "horddt_codec.hpp"
#include "horddt_color.hpp"
#include "horddt_remap.hpp"
#include "horddt_resize.hpp"

#include <opencv2/core.hpp>

#include <linux/videodev2.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint32_t kCaptureWidth = 1920U;
constexpr std::uint32_t kCaptureHeight = 1080U;
constexpr std::uint32_t kProcessWidth = 640U;
constexpr std::uint32_t kProcessHeight = 360U;
constexpr std::uint32_t kCameraBufferCount = 4U;
constexpr std::uint32_t kFrameRate = 30U;
constexpr std::uint32_t kBitRateKbps = 4096U;
constexpr int kJpegQuality = 85;
constexpr int kTimeoutMs = 1000;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int)
{
    g_stop = 1;
}

int retry_ioctl(int fd, unsigned long request, void *argument)
{
    int ret = 0;
    do {
        ret = ioctl(fd, request, argument);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

std::uint32_t align16(std::uint32_t value)
{
    return (value + 15U) & ~15U;
}

class hbmem_runtime {
public:
    hbmem_runtime() : status_(hb_mem_module_open())
    {
        if (status_ != 0) {
            std::fprintf(stderr, "hb_mem_module_open failed: %d\n", status_);
        }
    }

    ~hbmem_runtime()
    {
        if (status_ == 0) {
            hb_mem_module_close();
        }
    }

    int status() const noexcept { return status_; }

private:
    int status_;
};

class stream_file {
public:
    explicit stream_file(const std::string &path)
        : file_(std::fopen(path.c_str(), "wb")), path_(path)
    {
        if (file_ == nullptr) {
            std::fprintf(stderr, "Cannot create '%s': %s\n",
                         path.c_str(), std::strerror(errno));
            return;
        }
        // Avoid one write syscall per small NAL/JPEG fragment. stdio owns this
        // fixed buffer and flushes it when the file is closed.
        std::setvbuf(file_, nullptr, _IOFBF, 1U << 20U);
    }

    ~stream_file()
    {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    bool valid() const noexcept { return file_ != nullptr; }

    int write(const std::uint8_t *data, std::size_t size)
    {
        if (file_ == nullptr || data == nullptr || size == 0U) {
            return -1;
        }
        if (std::fwrite(data, 1U, size, file_) != size) {
            std::fprintf(stderr, "Failed to write '%s': %s\n",
                         path_.c_str(), std::strerror(errno));
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

class v4l2_dmabuf_camera {
public:
    struct frame {
        const hb_mem_graphic_buf_t *buffer = nullptr;
        std::uint32_t index = 0U;
        std::uint32_t sequence = 0U;
    };

    ~v4l2_dmabuf_camera() { close_camera(); }

    int open_camera(const std::string &device,
                    std::uint32_t width,
                    std::uint32_t height,
                    std::uint32_t buffer_count)
    {
        if (fd_ >= 0) {
            return -1;
        }
        fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            std::fprintf(stderr, "Cannot open camera '%s': %s\n",
                         device.c_str(), std::strerror(errno));
            return -1;
        }

        v4l2_capability capability{};
        if (retry_ioctl(fd_, VIDIOC_QUERYCAP, &capability) != 0) {
            return fail("VIDIOC_QUERYCAP");
        }
        const std::uint32_t caps =
            (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U
                ? capability.device_caps
                : capability.capabilities;
        if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0U ||
            (caps & V4L2_CAP_STREAMING) == 0U) {
            std::fprintf(stderr,
                         "%s is not a streaming single-plane capture device.\n",
                         device.c_str());
            return -1;
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        format.fmt.pix.bytesperline = align16(width);
        if (retry_ioctl(fd_, VIDIOC_S_FMT, &format) != 0) {
            return fail("VIDIOC_S_FMT(NV12)");
        }
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
            format.fmt.pix.width != width || format.fmt.pix.height != height) {
            std::fprintf(stderr,
                         "Camera rejected NV12 %ux%u (actual fourcc=%c%c%c%c "
                         "%ux%u).\n",
                         width, height,
                         static_cast<int>(format.fmt.pix.pixelformat & 0xff),
                         static_cast<int>((format.fmt.pix.pixelformat >> 8U) & 0xff),
                         static_cast<int>((format.fmt.pix.pixelformat >> 16U) & 0xff),
                         static_cast<int>((format.fmt.pix.pixelformat >> 24U) & 0xff),
                         format.fmt.pix.width, format.fmt.pix.height);
            return -1;
        }

        width_ = width;
        height_ = height;
        stride_ = format.fmt.pix.bytesperline != 0U
                      ? format.fmt.pix.bytesperline
                      : width;
        size_image_ = format.fmt.pix.sizeimage;
        if (stride_ < width_ || (stride_ & 15U) != 0U) {
            std::fprintf(stderr,
                         "Camera stride %u must be >= width and 16-byte aligned.\n",
                         stride_);
            return -1;
        }

        v4l2_requestbuffers request{};
        request.count = buffer_count;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_DMABUF;
        if (retry_ioctl(fd_, VIDIOC_REQBUFS, &request) != 0) {
            return fail("VIDIOC_REQBUFS(DMABUF)");
        }
        if (request.count < 2U) {
            std::fprintf(stderr, "Camera returned too few DMABUF slots: %u\n",
                         request.count);
            return -1;
        }

        buffers_.resize(request.count);
        constexpr std::int64_t flags =
            HB_MEM_USAGE_MAP_INITIALIZED |
            HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
            HB_MEM_USAGE_CPU_READ_OFTEN |
            HB_MEM_USAGE_CPU_WRITE_OFTEN |
            HB_MEM_USAGE_CACHED |
            HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

        for (std::uint32_t i = 0; i < request.count; ++i) {
            std::memset(&buffers_[i], 0, sizeof(buffers_[i]));
            buffers_[i].fd[0] = -1;
            buffers_[i].fd[1] = -1;
            const int ret = hb_mem_alloc_graph_buf(
                width_, height_, MEM_PIX_FMT_NV12, flags,
                stride_, height_, &buffers_[i]);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hb_mem_alloc_graph_buf(camera[%u]) failed: %d\n",
                             i, ret);
                return ret;
            }
            const std::uint64_t capacity =
                buffers_[i].size[0] + buffers_[i].size[1];
            if (buffers_[i].fd[0] < 0 || capacity < size_image_) {
                std::fprintf(stderr,
                             "Camera hbmem[%u] cannot hold frame: fd=%d, "
                             "capacity=%llu, sizeimage=%u.\n",
                             i, buffers_[i].fd[0],
                             static_cast<unsigned long long>(capacity),
                             size_image_);
                return -1;
            }
            if (queue(i) != 0) {
                return -1;
            }
        }

        return 0;
    }

    int start()
    {
        if (fd_ < 0 || buffers_.empty() || streaming_) {
            return -1;
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (retry_ioctl(fd_, VIDIOC_STREAMON, &type) != 0) {
            return fail("VIDIOC_STREAMON");
        }
        streaming_ = true;
        return 0;
    }

    int stop()
    {
        if (!streaming_) {
            return 0;
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (retry_ioctl(fd_, VIDIOC_STREAMOFF, &type) != 0) {
            return fail("VIDIOC_STREAMOFF");
        }
        streaming_ = false;
        return 0;
    }

    int acquire(frame &captured, int timeout_ms)
    {
        if (!streaming_) {
            std::fprintf(stderr, "Camera stream is not running.\n");
            return -1;
        }
        pollfd descriptor{};
        descriptor.fd = fd_;
        descriptor.events = POLLIN;
        int poll_ret = 0;
        do {
            poll_ret = ::poll(&descriptor, 1, timeout_ms);
        } while (poll_ret < 0 && errno == EINTR && g_stop == 0);
        if (poll_ret == 0) {
            std::fprintf(stderr, "Camera capture timed out.\n");
            return -1;
        }
        if (poll_ret < 0) {
            return fail("poll(camera)");
        }

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_DMABUF;
        if (retry_ioctl(fd_, VIDIOC_DQBUF, &buffer) != 0) {
            if (errno == EAGAIN) {
                return 1;
            }
            return fail("VIDIOC_DQBUF");
        }
        if (buffer.index >= buffers_.size()) {
            std::fprintf(stderr, "Camera returned invalid buffer index %u.\n",
                         buffer.index);
            return -1;
        }
        captured.buffer = &buffers_[buffer.index];
        captured.index = buffer.index;
        captured.sequence = buffer.sequence;
        return 0;
    }

    int release(const frame &captured) { return queue(captured.index); }

    std::uint32_t stride() const noexcept { return stride_; }

private:
    int queue(std::uint32_t index)
    {
        if (index >= buffers_.size()) {
            return -1;
        }
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_DMABUF;
        buffer.index = index;
        buffer.m.fd = buffers_[index].fd[0];
        buffer.length = size_image_;
        if (retry_ioctl(fd_, VIDIOC_QBUF, &buffer) != 0) {
            return fail("VIDIOC_QBUF(DMABUF)");
        }
        return 0;
    }

    int fail(const char *operation) const
    {
        std::fprintf(stderr, "%s failed: %s\n",
                     operation, std::strerror(errno));
        return -1;
    }

    void close_camera()
    {
        (void)stop();
        for (auto &buffer : buffers_) {
            if (buffer.fd[0] >= 0) {
                hb_mem_free_buf(buffer.fd[0]);
                buffer.fd[0] = -1;
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
    bool streaming_ = false;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::uint32_t stride_ = 0U;
    std::uint32_t size_image_ = 0U;
    std::vector<hb_mem_graphic_buf_t> buffers_;
};

class synchronous_worker {
public:
    synchronous_worker() : thread_(&synchronous_worker::run, this) {}

    ~synchronous_worker()
    {
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

    void submit(std::function<int()> task)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_ = std::move(task);
        result_ = -1;
        ready_ = true;
        done_ = false;
        ready_cv_.notify_one();
    }

    int wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this] { return done_; });
        return result_;
    }

private:
    void run()
    {
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
    std::string camera = "/dev/video0";
    std::string gdc_bin = "samples/gdc_1920x1080.bin";
    std::string h264_path = "camera_output.h264";
    std::string mjpeg_path = "camera_snapshots.mjpg";
    std::uint64_t frame_limit = 0U;
    std::uint32_t color_every = 1U;
    std::uint32_t jpeg_every = 30U;
};

bool parse_unsigned(const char *text, std::uint64_t &value)
{
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

void print_usage(const char *program)
{
    std::fprintf(
        stderr,
        "Usage: %s [camera gdc.bin output.h264 snapshots.mjpg "
        "frame_limit color_every jpeg_every]\n"
        "Defaults: /dev/video0 samples/gdc_1920x1080.bin "
        "camera_output.h264 camera_snapshots.mjpg 0 1 30\n"
        "  frame_limit=0 runs until Ctrl-C. color_every/jpeg_every=0 disables "
        "that branch.\n",
        program);
}

int parse_options(int argc, char **argv, options &result)
{
    if (argc == 2 &&
        (std::strcmp(argv[1], "-h") == 0 ||
         std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc != 1 && argc != 8) {
        print_usage(argv[0]);
        return -1;
    }
    if (argc == 1) {
        return 0;
    }
    result.camera = argv[1];
    result.gdc_bin = argv[2];
    result.h264_path = argv[3];
    result.mjpeg_path = argv[4];
    std::uint64_t color_every = 0U;
    std::uint64_t jpeg_every = 0U;
    if (!parse_unsigned(argv[5], result.frame_limit) ||
        !parse_unsigned(argv[6], color_every) ||
        !parse_unsigned(argv[7], jpeg_every) ||
        color_every > UINT32_MAX || jpeg_every > UINT32_MAX) {
        print_usage(argv[0]);
        return -1;
    }
    result.color_every = static_cast<std::uint32_t>(color_every);
    result.jpeg_every = static_cast<std::uint32_t>(jpeg_every);
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    options opts;
    const int parse_ret = parse_options(argc, argv, opts);
    if (parse_ret != 0) {
        return parse_ret > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    hbmem_runtime memory;
    if (memory.status() != 0) {
        return EXIT_FAILURE;
    }

    v4l2_dmabuf_camera camera;
    if (camera.open_camera(opts.camera, kCaptureWidth, kCaptureHeight,
                           kCameraBufferCount) != 0) {
        return EXIT_FAILURE;
    }

    horddt_remap::config remap_cfg;
    remap_cfg.input_width = kCaptureWidth;
    remap_cfg.input_height = kCaptureHeight;
    remap_cfg.output_width = kCaptureWidth;
    remap_cfg.output_height = kCaptureHeight;
    remap_cfg.input_stride = camera.stride();
    remap_cfg.output_stride = align16(kCaptureWidth);
    remap_cfg.gdc_bin_path = opts.gdc_bin;
    remap_cfg.timeout_ms = kTimeoutMs;
    remap_cfg.output_buffer_count = 2U;
    remap_cfg.sync_input_for_device = false;       // camera DMA wrote it
    remap_cfg.sync_borrowed_output_for_cpu = false; // next consumer is PYM

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
    resize_cfg.sync_input_for_device = false;       // GDC DMA wrote it
    resize_cfg.sync_borrowed_output_for_cpu = true; // CPU/codec read it

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

    horddt_remap remap(remap_cfg);
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
    if (!remap.is_initialized() || !resize.is_initialized() ||
        (color && !color->is_initialized()) || !h264_codec.is_initialized() ||
        (jpeg_codec && !jpeg_codec->is_initialized())) {
        std::fprintf(stderr,
                     "Operator initialization failed: gdc=%d pym=%d "
                     "color=%d h264=%d jpeg=%d\n",
                     remap.initialization_status(),
                     resize.initialization_status(),
                     color ? color->initialization_status() : 0,
                     h264_codec.initialization_status(),
                     jpeg_codec ? jpeg_codec->initialization_status() : 0);
        return EXIT_FAILURE;
    }

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
    // Start capture only after every persistent context, output and worker is
    // ready, so the driver queue does not fill with stale startup frames.
    if (camera.start() != 0) {
        return EXIT_FAILURE;
    }

    std::uint64_t frame_count = 0U;
    std::uint64_t color_count = 0U;
    std::uint64_t jpeg_count = 0U;
    auto report_start = std::chrono::steady_clock::now();
    std::uint64_t report_frames = 0U;

    while (g_stop == 0 &&
           (opts.frame_limit == 0U || frame_count < opts.frame_limit)) {
        v4l2_dmabuf_camera::frame camera_frame;
        const int acquire_ret = camera.acquire(camera_frame, kTimeoutMs);
        if (acquire_ret > 0) {
            continue;
        }
        if (acquire_ret < 0) {
            break;
        }

        const std::uint64_t current_frame = frame_count;
        int process_ret = remap.remap_borrowed(
            *camera_frame.buffer,
            [&](const hb_mem_graphic_buf_t &gdc_frame) {
                return resize.resize_borrowed(
                    gdc_frame,
                    [&](const hb_mem_graphic_buf_t &pym_frame) {
                        // PYM output is invalidated once before this callback.
                        // All branches read the same buffer; none copies it to
                        // an intermediate application-owned NV12 frame.
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
                                color_worker->submit(
                                    [&color, &pym_frame, &bgr] {
                                        return color->convert(
                                            pym_frame, bgr,
                                            horddt_color::output_format::bgr);
                                    });
                                color_pending = true;
                            }
                            if (run_jpeg) {
                                jpeg_worker->submit(
                                    [&jpeg_codec, &pym_frame, &jpeg_output] {
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
                            // A submitted worker still owns a read of the PYM
                            // buffer, so it must be joined before returning.
                            std::fprintf(stderr,
                                         "Failed to schedule a pipeline branch.\n");
                            schedule_ret = -1;
                        }

                        int ret = schedule_ret;
                        if (schedule_ret == 0) {
                            try {
                                // H.264 runs on the capture thread while the
                                // independent CPU color and JPEG branches run
                                // on persistent workers.
                                ret = h264_codec.nv12_to_h264_borrowed(
                                    pym_frame,
                                    [&](const std::uint8_t *data,
                                        std::size_t size) {
                                        return h264_output.write(data, size);
                                    });
                            } catch (...) {
                                std::fprintf(stderr,
                                             "H.264 branch threw an exception.\n");
                                ret = -1;
                            }
                        }

                        // Wait for every submitted reader before PYM releases
                        // its borrowed output buffer, including error paths.
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
            });

        const int release_ret = camera.release(camera_frame);
        if (process_ret != 0 || release_ret != 0) {
            std::fprintf(stderr,
                         "Frame %llu failed: process=%d requeue=%d\n",
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

    const int stop_ret = camera.stop();
    std::printf("Stopped after %llu frames; H.264=%llu bytes, JPEG frames=%llu, "
                "color frames=%llu.\n",
                static_cast<unsigned long long>(frame_count),
                static_cast<unsigned long long>(h264_output.bytes()),
                static_cast<unsigned long long>(jpeg_count),
                static_cast<unsigned long long>(color_count));
    return frame_count != 0U && stop_ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
