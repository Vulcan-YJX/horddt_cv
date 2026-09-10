#include "horddt_remap.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::uint32_t kInputWidth = 1920U;
constexpr std::uint32_t kInputHeight = 1080U;
constexpr std::uint32_t kOutputWidth = 1920U;
constexpr std::uint32_t kOutputHeight = 1080U;
constexpr int kTimeoutMs = 2000;

constexpr const char *kDefaultInputPath = "data/input_1920x1080.nv12";
constexpr const char *kDefaultGdcBinPath = "samples/gdc_1920x1080.bin";
constexpr const char *kOutputPath = "remap_output_1920x1080.nv12";

std::uint32_t align16(std::uint32_t value)
{
    return (value + 15U) & ~15U;
}

int allocate_nv12_buffer(std::uint32_t width,
                         std::uint32_t height,
                         std::uint32_t stride,
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
    buffer.fd[0] = -1;
    buffer.fd[1] = -1;
    buffer.fd[2] = -1;
    const int ret = hb_mem_alloc_graph_buf(width, height, MEM_PIX_FMT_NV12,
                                           flags, stride, height, &buffer);
    if (ret != 0) {
        std::fprintf(stderr,
                     "hb_mem_alloc_graph_buf(%ux%u, stride=%u) failed: %d\n",
                     width, height, stride, ret);
        buffer.fd[0] = -1;
    }
    return ret;
}

int read_plane(FILE *file,
               std::uint8_t *destination,
               std::uint32_t rows,
               std::uint32_t row_bytes,
               std::uint32_t stride)
{
    for (std::uint32_t row = 0; row < rows; ++row) {
        if (std::fread(destination + static_cast<std::size_t>(row) * stride,
                       1, static_cast<std::size_t>(row_bytes), file) !=
            static_cast<std::size_t>(row_bytes)) {
            return -1;
        }
    }
    return 0;
}

int write_plane(FILE *file,
                const std::uint8_t *source,
                std::uint32_t rows,
                std::uint32_t row_bytes,
                std::uint32_t stride)
{
    for (std::uint32_t row = 0; row < rows; ++row) {
        if (std::fwrite(source + static_cast<std::size_t>(row) * stride,
                        1, static_cast<std::size_t>(row_bytes), file) !=
            static_cast<std::size_t>(row_bytes)) {
            return -1;
        }
    }
    return 0;
}

int load_nv12(const char *path, hb_mem_graphic_buf_t &buffer)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot open input '%s': %s\n",
                     path, std::strerror(errno));
        return -1;
    }

    const auto width = static_cast<std::uint32_t>(buffer.width);
    const auto height = static_cast<std::uint32_t>(buffer.height);
    const auto stride = static_cast<std::uint32_t>(buffer.stride);
    const int ret =
        read_plane(file, buffer.virt_addr[0], height, width, stride) != 0 ||
                read_plane(file, buffer.virt_addr[1], height / 2U,
                           width, stride) != 0
            ? -1
            : 0;
    const int close_ret = std::fclose(file);
    if (ret != 0 || close_ret != 0) {
        std::fprintf(stderr,
                     "Failed to read one %ux%u NV12 frame from '%s'.\n",
                     width, height, path);
        return -1;
    }
    return 0;
}

int save_nv12(const char *path, const hb_mem_graphic_buf_t &buffer)
{
    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot open output '%s': %s\n",
                     path, std::strerror(errno));
        return -1;
    }

    const auto width = static_cast<std::uint32_t>(buffer.width);
    const auto height = static_cast<std::uint32_t>(buffer.height);
    const auto stride = static_cast<std::uint32_t>(buffer.stride);
    const int ret =
        write_plane(file, buffer.virt_addr[0], height, width, stride) != 0 ||
                write_plane(file, buffer.virt_addr[1], height / 2U,
                            width, stride) != 0
            ? -1
            : 0;
    const int close_ret = std::fclose(file);
    if (ret != 0 || close_ret != 0) {
        std::fprintf(stderr, "Failed to write NV12 output '%s'.\n", path);
        return -1;
    }
    return 0;
}

void free_graphic_buffer(hb_mem_graphic_buf_t &buffer)
{
    if (buffer.fd[0] >= 0) {
        const int ret = hb_mem_free_buf(buffer.fd[0]);
        if (ret != 0) {
            std::fprintf(stderr, "hb_mem_free_buf failed: %d\n", ret);
        }
        buffer.fd[0] = -1;
    }
}

void print_usage(const char *program)
{
    std::fprintf(stderr,
                 "Usage: %s [input.nv12 gdc.bin output.nv12]\n"
                 "  Apply GDC remapping to one 1920x1080 NV12 frame.\n"
                 "  Defaults: %s + %s -> %s\n",
                 program, kDefaultInputPath, kDefaultGdcBinPath,
                 kOutputPath);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 &&
        (std::strcmp(argv[1], "-h") == 0 ||
         std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc != 1 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *input_path = argc == 4 ? argv[1] : kDefaultInputPath;
    const char *gdc_bin_path = argc == 4 ? argv[2] : kDefaultGdcBinPath;
    const char *output_path = argc == 4 ? argv[3] : kOutputPath;
    const std::uint32_t input_stride = align16(kInputWidth);
    const std::uint32_t output_stride = align16(kOutputWidth);

    horddt_remap::config cfg;
    cfg.input_width = kInputWidth;
    cfg.input_height = kInputHeight;
    cfg.output_width = kOutputWidth;
    cfg.output_height = kOutputHeight;
    cfg.input_stride = input_stride;
    cfg.output_stride = output_stride;
    cfg.gdc_bin_path = gdc_bin_path;
    cfg.timeout_ms = kTimeoutMs;
    cfg.hw_id = 0;
    cfg.verbose = true;

    horddt_remap remapper(cfg);
    if (!remapper.is_initialized()) {
        std::fprintf(stderr,
                     "Failed to initialize horddt_remap: %d\n"
                     "Check that '%s' matches the image resolution and "
                     "camera calibration.\n",
                     remapper.initialization_status(), gdc_bin_path);
        return 1;
    }

    hb_mem_graphic_buf_t input_buffer{};
    hb_mem_graphic_buf_t output_buffer{};
    input_buffer.fd[0] = -1;
    output_buffer.fd[0] = -1;

    int ret = allocate_nv12_buffer(kInputWidth, kInputHeight,
                                   input_stride, input_buffer);
    if (ret == 0) {
        ret = allocate_nv12_buffer(kOutputWidth, kOutputHeight,
                                   output_stride, output_buffer);
    }
    if (ret == 0) {
        ret = load_nv12(input_path, input_buffer);
    }
    if (ret == 0) {
        ret = remapper.remap(input_buffer, output_buffer);
    }
    if (ret == 0) {
        ret = save_nv12(output_path, output_buffer);
    }

    free_graphic_buffer(output_buffer);
    free_graphic_buffer(input_buffer);

    if (ret != 0) {
        std::fprintf(stderr, "GDC remap sample failed: %d\n", ret);
        return 1;
    }

    std::printf("GDC remap completed: %s -> %s\n",
                input_path, output_path);
    return 0;
}
