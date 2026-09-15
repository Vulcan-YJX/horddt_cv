#include "horddt_color.hpp"
#include "horddt_pip.hpp"
#include "horddt_resize.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr const char *kDefaultInputPath = "data/input_1920x1080.jpg";
constexpr const char *kDefaultOutputPath = "pip_output.jpg";
constexpr std::uint32_t kOverlayMargin = 32U;
constexpr int kTimeoutMs = 2000;
constexpr int kJpegQuality = 95;

std::uint32_t align64(std::uint32_t value)
{
    return (value + 63U) & ~63U;
}

class graphic_buffer {
public:
    graphic_buffer() = default;
    ~graphic_buffer()
    {
        reset();
    }

    graphic_buffer(const graphic_buffer &) = delete;
    graphic_buffer &operator=(const graphic_buffer &) = delete;

    int allocate(std::uint32_t width, std::uint32_t height)
    {
        reset();
        constexpr std::int64_t flags =
            HB_MEM_USAGE_MAP_INITIALIZED |
            HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
            HB_MEM_USAGE_CPU_READ_OFTEN |
            HB_MEM_USAGE_CPU_WRITE_OFTEN |
            HB_MEM_USAGE_CACHED |
            HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
        std::memset(&buffer_, 0, sizeof(buffer_));
        const int ret = hb_mem_alloc_graph_buf(
            width, height, MEM_PIX_FMT_NV12, flags,
            align64(width), height, &buffer_);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mem_alloc_graph_buf(%ux%u) failed: %d\n",
                         width, height, ret);
            return ret;
        }
        allocated_ = true;
        return 0;
    }

    void reset()
    {
        if (allocated_) {
            const int ret = hb_mem_free_buf(buffer_.fd[0]);
            if (ret != 0) {
                std::fprintf(stderr, "hb_mem_free_buf failed: %d\n", ret);
            }
            allocated_ = false;
            std::memset(&buffer_, 0, sizeof(buffer_));
        }
    }

    hb_mem_graphic_buf_t &get()
    {
        return buffer_;
    }

private:
    hb_mem_graphic_buf_t buffer_{};
    bool allocated_ = false;
};

int flush_nv12(const hb_mem_graphic_buf_t &buffer)
{
    int ret = hb_mem_flush_buf_with_vaddr(
        reinterpret_cast<std::uint64_t>(buffer.virt_addr[0]),
        buffer.size[0]);
    if (ret == 0) {
        ret = hb_mem_flush_buf_with_vaddr(
            reinterpret_cast<std::uint64_t>(buffer.virt_addr[1]),
            buffer.size[1]);
    }
    if (ret != 0) {
        std::fprintf(stderr, "NV12 cache flush failed: %d\n", ret);
    }
    return ret;
}

int bgr_to_nv12(const cv::Mat &bgr, hb_mem_graphic_buf_t &output)
{
    if (bgr.empty() || bgr.type() != CV_8UC3 ||
        bgr.cols != output.width || bgr.rows != output.height ||
        output.format != MEM_PIX_FMT_NV12 || output.plane_cnt < 2 ||
        output.virt_addr[0] == nullptr || output.virt_addr[1] == nullptr) {
        std::fprintf(stderr, "Invalid BGR image or destination NV12 buffer.\n");
        return -1;
    }

    cv::Mat i420;
    try {
        cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    } catch (const cv::Exception &exception) {
        std::fprintf(stderr, "BGR to I420 conversion failed: %s\n",
                     exception.what());
        return -1;
    }
    if (!i420.isContinuous()) {
        i420 = i420.clone();
    }

    const std::uint32_t width = static_cast<std::uint32_t>(bgr.cols);
    const std::uint32_t height = static_cast<std::uint32_t>(bgr.rows);
    const std::size_t y_size =
        static_cast<std::size_t>(width) * height;
    const std::size_t chroma_plane_size = y_size / 4U;
    const auto *i420_data = i420.ptr<std::uint8_t>(0);
    const auto *source_y = i420_data;
    const auto *source_u = i420_data + y_size;
    const auto *source_v = source_u + chroma_plane_size;
    auto *destination_y =
        static_cast<std::uint8_t *>(output.virt_addr[0]);
    auto *destination_uv =
        static_cast<std::uint8_t *>(output.virt_addr[1]);
    const std::size_t destination_stride =
        static_cast<std::size_t>(output.stride);

    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination_y + row * destination_stride,
                    source_y + static_cast<std::size_t>(row) * width,
                    width);
    }
    for (std::uint32_t row = 0; row < height / 2U; ++row) {
        std::uint8_t *destination_row =
            destination_uv + row * destination_stride;
        const std::uint8_t *source_u_row =
            source_u + static_cast<std::size_t>(row) * (width / 2U);
        const std::uint8_t *source_v_row =
            source_v + static_cast<std::size_t>(row) * (width / 2U);
        for (std::uint32_t column = 0; column < width / 2U; ++column) {
            destination_row[column * 2U] = source_u_row[column];
            destination_row[column * 2U + 1U] = source_v_row[column];
        }
    }
    return flush_nv12(output);
}

int save_jpeg(const char *path, const cv::Mat &bgr)
{
    const std::vector<int> parameters = {
        cv::IMWRITE_JPEG_QUALITY, kJpegQuality,
    };
    try {
        if (!cv::imwrite(path, bgr, parameters)) {
            std::fprintf(stderr, "Failed to write JPEG '%s'.\n", path);
            return -1;
        }
    } catch (const cv::Exception &exception) {
        std::fprintf(stderr, "JPEG write failed: %s\n", exception.what());
        return -1;
    }
    return 0;
}

void print_usage(const char *program)
{
    std::fprintf(
        stderr,
        "Usage: %s [input.jpg output.jpg]\n"
        "  Load JPEG -> NV12, resize the NV12 image to one half with PYM,\n"
        "  overlay it at the top-right with STITCH, then write JPEG.\n"
        "  Defaults: %s -> %s\n",
        program, kDefaultInputPath, kDefaultOutputPath);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 &&
        (std::strcmp(argv[1], "-h") == 0 ||
         std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc != 1 && argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *input_path = argc == 3 ? argv[1] : kDefaultInputPath;
    const char *output_path = argc == 3 ? argv[2] : kDefaultOutputPath;

    cv::Mat input = cv::imread(input_path, cv::IMREAD_COLOR);
    if (input.empty()) {
        std::fprintf(stderr, "Cannot load input JPEG '%s'.\n", input_path);
        return EXIT_FAILURE;
    }

    // NV12 and the half-size overlay both need even dimensions, therefore the
    // source is cropped by at most three pixels to a multiple of four.
    const int usable_width = input.cols & ~3;
    const int usable_height = input.rows & ~3;
    if (usable_width < 64 || usable_height < 64 ||
        usable_width > 4096 || usable_height > 4096) {
        std::fprintf(stderr,
                     "Input dimensions must become [64,4096] after "
                     "four-pixel alignment; got %dx%d.\n",
                     input.cols, input.rows);
        return EXIT_FAILURE;
    }
    if (usable_width != input.cols || usable_height != input.rows) {
        input = input(cv::Rect(0, 0, usable_width, usable_height)).clone();
        std::printf("Input cropped to NV12-compatible size: %dx%d\n",
                    usable_width, usable_height);
    }

    const std::uint32_t width =
        static_cast<std::uint32_t>(usable_width);
    const std::uint32_t height =
        static_cast<std::uint32_t>(usable_height);
    const std::uint32_t overlay_width = width / 2U;
    const std::uint32_t overlay_height = height / 2U;
    const std::uint32_t margin_x =
        width >= overlay_width + kOverlayMargin ? kOverlayMargin : 0U;
    const std::uint32_t margin_y =
        height >= overlay_height + kOverlayMargin ? kOverlayMargin : 0U;
    const std::uint32_t overlay_x =
        (width - overlay_width - margin_x) & ~1U;
    const std::uint32_t overlay_y = margin_y & ~1U;

    horddt_resize::config resize_config;
    resize_config.input_width = width;
    resize_config.input_height = height;
    resize_config.output_width = overlay_width;
    resize_config.output_height = overlay_height;
    resize_config.timeout_ms = kTimeoutMs;
    resize_config.enable_file_io = false;
    resize_config.verbose = true;

    horddt_pip::config pip_config;
    pip_config.background_width = width;
    pip_config.background_height = height;
    pip_config.overlay_width = overlay_width;
    pip_config.overlay_height = overlay_height;
    pip_config.overlay_x = overlay_x;
    pip_config.overlay_y = overlay_y;
    pip_config.background_stride = align64(width);
    pip_config.overlay_stride = align64(overlay_width);
    pip_config.output_stride = align64(width);
    pip_config.timeout_ms = kTimeoutMs;
    pip_config.verbose = true;

    horddt_resize resizer(resize_config);
    horddt_pip compositor(pip_config);
    horddt_color color_converter;
    if (!resizer.is_initialized()) {
        std::fprintf(stderr, "horddt_resize initialization failed: %d\n",
                     resizer.initialization_status());
        return EXIT_FAILURE;
    }
    if (!compositor.is_initialized()) {
        std::fprintf(stderr, "horddt_pip initialization failed: %d\n",
                     compositor.initialization_status());
        return EXIT_FAILURE;
    }
    if (!color_converter.is_initialized()) {
        std::fprintf(stderr, "horddt_color initialization failed: %d\n",
                     color_converter.initialization_status());
        return EXIT_FAILURE;
    }

    graphic_buffer original_nv12;
    graphic_buffer resized_nv12;
    graphic_buffer composed_nv12;
    int ret = original_nv12.allocate(width, height);
    if (ret == 0) {
        ret = resized_nv12.allocate(overlay_width, overlay_height);
    }
    if (ret == 0) {
        ret = composed_nv12.allocate(width, height);
    }
    if (ret == 0) {
        ret = bgr_to_nv12(input, original_nv12.get());
    }
    if (ret == 0) {
        ret = resizer.resize(original_nv12.get(), resized_nv12.get());
    }
    if (ret == 0) {
        ret = compositor.compose(original_nv12.get(),
                                 resized_nv12.get(),
                                 composed_nv12.get());
    }

    cv::Mat output_bgr;
    if (ret == 0) {
        ret = color_converter.convert(
            composed_nv12.get(), output_bgr,
            horddt_color::output_format::bgr);
    }
    if (ret == 0) {
        ret = save_jpeg(output_path, output_bgr);
    }
    if (ret != 0) {
        std::fprintf(stderr, "PIP sample failed: %d\n", ret);
        return EXIT_FAILURE;
    }

    std::printf(
        "PIP complete: %s (%ux%u), overlay=%ux%u at (%u,%u)\n",
        output_path, width, height,
        overlay_width, overlay_height, overlay_x, overlay_y);
    return EXIT_SUCCESS;
}
