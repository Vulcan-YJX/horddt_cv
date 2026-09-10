#include "horddt_color.hpp"
#include "horddt_resize.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kInputWidth = 1920U;
constexpr std::uint32_t kInputHeight = 1080U;
constexpr std::uint32_t kOutputWidth = 640U;
constexpr std::uint32_t kOutputHeight = 360U;
constexpr int kTimeoutMs = 2000;
constexpr int kJpegQuality = 95;

constexpr const char *kDefaultInputPath = "data/input_1920x1080.nv12";
constexpr const char *kOutputPath = "output_640x360.jpg";

std::uint32_t align64(std::uint32_t value)
{
    return (value + 63U) & ~63U;
}

int allocate_nv12_buffer(std::uint32_t width,
                         std::uint32_t height,
                         hb_mem_graphic_buf_t &buffer)
{
    constexpr std::int64_t flags =
        HB_MEM_USAGE_MAP_INITIALIZED |
        HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
        HB_MEM_USAGE_CPU_READ_OFTEN |
        HB_MEM_USAGE_CPU_WRITE_OFTEN |
        HB_MEM_USAGE_CACHED |
        HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

    std::memset(&buffer, 0, sizeof(buffer));
    const int ret = hb_mem_alloc_graph_buf(
        width, height, MEM_PIX_FMT_NV12, flags,
        align64(width), height, &buffer);
    if (ret != 0) {
        std::fprintf(stderr,
                     "hb_mem_alloc_graph_buf(%ux%u) failed: %d\n",
                     width, height, ret);
    }
    return ret;
}

int load_nv12_file(const char *path, hb_mem_graphic_buf_t &buffer)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot open input file '%s': %s\n",
                     path, std::strerror(errno));
        return -1;
    }

    const auto width = static_cast<std::uint32_t>(buffer.width);
    const auto height = static_cast<std::uint32_t>(buffer.height);
    const auto stride = static_cast<std::uint32_t>(buffer.stride);
    auto *y_plane = static_cast<std::uint8_t *>(buffer.virt_addr[0]);
    auto *uv_plane = static_cast<std::uint8_t *>(buffer.virt_addr[1]);

    bool failed = false;
    for (std::uint32_t row = 0; row < height && !failed; ++row) {
        failed = std::fread(
                     y_plane + static_cast<std::size_t>(row) * stride,
                     1, width, file) != width;
    }
    for (std::uint32_t row = 0; row < height / 2U && !failed; ++row) {
        failed = std::fread(
                     uv_plane + static_cast<std::size_t>(row) * stride,
                     1, width, file) != width;
    }

    const int close_ret = std::fclose(file);
    if (failed || close_ret != 0) {
        std::fprintf(stderr,
                     "Failed to read one %ux%u NV12 frame from '%s'.\n",
                     width, height, path);
        return -1;
    }
    return 0;
}

int save_jpeg(const char *path, const cv::Mat &image)
{
    if (image.empty() || image.type() != CV_8UC3) {
        std::fprintf(stderr, "Invalid BGR image for JPEG output.\n");
        return -1;
    }

    const std::vector<int> parameters = {
        cv::IMWRITE_JPEG_QUALITY, kJpegQuality,
    };
    try {
        if (!cv::imwrite(path, image, parameters)) {
            std::fprintf(stderr, "Failed to write JPEG file '%s'.\n", path);
            return -1;
        }
    } catch (const cv::Exception &exception) {
        std::fprintf(stderr, "JPEG encoding failed: %s\n", exception.what());
        return -1;
    }
    return 0;
}

void print_usage(const char *program)
{
    std::fprintf(stderr,
                 "Usage: %s [input.nv12 output.jpg]\n"
                 "  Resize one 1920x1080 NV12 frame to 640x360 JPEG.\n"
                 "  Defaults: %s -> %s\n",
                 program, kDefaultInputPath, kOutputPath);
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
    const char *output_path = argc == 3 ? argv[2] : kOutputPath;
    horddt_resize::config cfg;
    cfg.input_width = kInputWidth;
    cfg.input_height = kInputHeight;
    cfg.output_width = kOutputWidth;
    cfg.output_height = kOutputHeight;
    cfg.timeout_ms = kTimeoutMs;
    cfg.verbose = true;

    horddt_resize resizer(cfg);
    horddt_color color_converter;
    if (!resizer.is_initialized()) {
        std::fprintf(stderr, "horddt_resize initialization failed: %d\n",
                     resizer.initialization_status());
        return EXIT_FAILURE;
    }
    if (!color_converter.is_initialized()) {
        std::fprintf(stderr, "horddt_color initialization failed: %d\n",
                     color_converter.initialization_status());
        return EXIT_FAILURE;
    }

    hb_mem_graphic_buf_t input_buffer{};
    hb_mem_graphic_buf_t output_buffer{};
    bool input_allocated = false;
    bool output_allocated = false;

    int ret = allocate_nv12_buffer(kInputWidth, kInputHeight, input_buffer);
    if (ret == 0) {
        input_allocated = true;
        ret = allocate_nv12_buffer(kOutputWidth, kOutputHeight, output_buffer);
    }
    if (ret == 0) {
        output_allocated = true;
        ret = load_nv12_file(input_path, input_buffer);
    }
    if (ret == 0) {
        ret = resizer.resize(input_buffer, output_buffer);
    }
    cv::Mat bgr_image;
    if (ret == 0) {
        ret = color_converter.convert(
            output_buffer, bgr_image, horddt_color::output_format::bgr);
    }
    if (ret == 0) {
        ret = save_jpeg(output_path, bgr_image);
    }

    if (output_allocated) {
        const int free_ret = hb_mem_free_buf(output_buffer.fd[0]);
        if (ret == 0 && free_ret != 0) {
            ret = free_ret;
        }
    }
    if (input_allocated) {
        const int free_ret = hb_mem_free_buf(input_buffer.fd[0]);
        if (ret == 0 && free_ret != 0) {
            ret = free_ret;
        }
    }

    if (ret != 0) {
        return EXIT_FAILURE;
    }

    std::printf("Resize complete: %s (%ux%u JPEG)\n",
                output_path, kOutputWidth, kOutputHeight);
    return EXIT_SUCCESS;
}
