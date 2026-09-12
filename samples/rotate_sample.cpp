#include "horddt_rotate.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::uint32_t kInputWidth = 1920U;
constexpr std::uint32_t kInputHeight = 1080U;
constexpr int kTimeoutMs = 2000;

constexpr const char *kDefaultInputPath = "data/input_1920x1080.nv12";
constexpr const char *kDefaultOutputPath =
    "rotate_output_1920x1080_180.nv12";

std::uint32_t align16(std::uint32_t value)
{
    return (value + 15U) & ~15U;
}

bool parse_angle(const char *text, horddt_rotate::angle &rotation)
{
    if (std::strcmp(text, "90") == 0) {
        rotation = horddt_rotate::angle::rotate_90;
        return true;
    }
    if (std::strcmp(text, "180") == 0) {
        rotation = horddt_rotate::angle::rotate_180;
        return true;
    }
    if (std::strcmp(text, "270") == 0) {
        rotation = horddt_rotate::angle::rotate_270;
        return true;
    }
    return false;
}

unsigned int angle_degrees(horddt_rotate::angle rotation)
{
    return static_cast<unsigned int>(rotation);
}

bool swaps_dimensions(horddt_rotate::angle rotation)
{
    return rotation == horddt_rotate::angle::rotate_90 ||
           rotation == horddt_rotate::angle::rotate_270;
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
        read_plane(file, static_cast<std::uint8_t *>(buffer.virt_addr[0]),
                   height, width, stride) != 0 ||
                read_plane(file, static_cast<std::uint8_t *>(buffer.virt_addr[1]),
                           height / 2U, width, stride) != 0
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
        write_plane(file,
                    static_cast<const std::uint8_t *>(buffer.virt_addr[0]),
                    height, width, stride) != 0 ||
                write_plane(file,
                            static_cast<const std::uint8_t *>(
                                buffer.virt_addr[1]),
                            height / 2U, width, stride) != 0
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
                 "Usage: %s [input.nv12 output.nv12 angle]\n"
                 "  Rotate one 1920x1080 NV12 frame by 90, 180, or 270 "
                 "degrees.\n"
                 "  Defaults: %s -> %s (180 degrees)\n",
                 program, kDefaultInputPath, kDefaultOutputPath);
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
    const char *output_path = argc == 4 ? argv[2] : kDefaultOutputPath;
    horddt_rotate::angle rotation = horddt_rotate::angle::rotate_180;
    if (argc == 4 && !parse_angle(argv[3], rotation)) {
        std::fprintf(stderr,
                     "Unsupported rotation angle '%s'; use 90, 180, or 270.\n",
                     argv[3]);
        return 1;
    }

    const bool swap = swaps_dimensions(rotation);
    const std::uint32_t output_width =
        swap ? kInputHeight : kInputWidth;
    const std::uint32_t output_height =
        swap ? kInputWidth : kInputHeight;
    const std::uint32_t input_stride = align16(kInputWidth);
    const std::uint32_t output_stride = align16(output_width);

    horddt_rotate::config cfg;
    cfg.input_width = kInputWidth;
    cfg.input_height = kInputHeight;
    cfg.rotation = rotation;
    cfg.input_stride = input_stride;
    cfg.output_stride = output_stride;
    cfg.timeout_ms = kTimeoutMs;
    cfg.hw_id = 0;
    cfg.verbose = true;

    horddt_rotate rotator(cfg);
    if (!rotator.is_initialized()) {
        std::fprintf(stderr, "Failed to initialize horddt_rotate: %d\n",
                     rotator.initialization_status());
        return 1;
    }

    hb_mem_graphic_buf_t input_buffer{};
    hb_mem_graphic_buf_t output_buffer{};
    input_buffer.fd[0] = -1;
    output_buffer.fd[0] = -1;

    int ret = allocate_nv12_buffer(kInputWidth, kInputHeight,
                                   input_stride, input_buffer);
    if (ret == 0) {
        ret = allocate_nv12_buffer(output_width, output_height,
                                   output_stride, output_buffer);
    }
    if (ret == 0) {
        ret = load_nv12(input_path, input_buffer);
    }
    if (ret == 0) {
        ret = rotator.rotate(input_buffer, output_buffer);
    }
    if (ret == 0) {
        ret = save_nv12(output_path, output_buffer);
    }

    free_graphic_buffer(output_buffer);
    free_graphic_buffer(input_buffer);

    if (ret != 0) {
        std::fprintf(stderr, "GDC rotate sample failed: %d\n", ret);
        return 1;
    }

    std::printf("GDC rotate completed: %s -> %s (%u degrees, %ux%u)\n",
                input_path, output_path, angle_degrees(rotation),
                output_width, output_height);
    return 0;
}
