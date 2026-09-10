#include "horddt_codec.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 1920U;
constexpr std::uint32_t kHeight = 1080U;
constexpr std::uint32_t kFrameRate = 30U;
constexpr std::uint32_t kBitRate = 8192U;
constexpr int kJpegQuality = 90;
constexpr int kTimeoutMs = 2000;

constexpr const char *kDefaultNv12InputPath = "data/input_1920x1080.nv12";
constexpr const char *kDefaultJpegInputPath = "data/input_1920x1080.jpg";
constexpr const char *kJpegOutputName = "codec_output_1920x1080.jpg";
constexpr const char *kNv12H264OutputName = "nv12_output_1920x1080.h264";
constexpr const char *kJpegH264OutputName = "jpeg_output_1920x1080.h264";

std::uint32_t align64(std::uint32_t value)
{
    return (value + 63U) & ~63U;
}

int allocate_nv12_buffer(hb_mem_graphic_buf_t &buffer)
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
    const int ret = hb_mem_alloc_graph_buf(
        kWidth, kHeight, MEM_PIX_FMT_NV12, flags,
        align64(kWidth), kHeight, &buffer);
    if (ret != 0) {
        std::fprintf(stderr, "hb_mem_alloc_graph_buf failed: %d\n", ret);
    }
    return ret;
}

int load_nv12(const char *path, hb_mem_graphic_buf_t &buffer)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot open '%s': %s\n",
                     path, std::strerror(errno));
        return -1;
    }

    auto *y = static_cast<std::uint8_t *>(buffer.virt_addr[0]);
    auto *uv = static_cast<std::uint8_t *>(buffer.virt_addr[1]);
    const std::size_t stride = static_cast<std::size_t>(buffer.stride);
    int result = 0;
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        if (std::fread(y + static_cast<std::size_t>(row) * stride,
                       1, kWidth, file) != kWidth) {
            result = -1;
            break;
        }
    }
    for (std::uint32_t row = 0; row < kHeight / 2U && result == 0; ++row) {
        if (std::fread(uv + static_cast<std::size_t>(row) * stride,
                       1, kWidth, file) != kWidth) {
            result = -1;
        }
    }

    const int close_ret = std::fclose(file);
    if (result != 0 || close_ret != 0) {
        std::fprintf(stderr,
                     "Failed to read one %ux%u NV12 frame from '%s'.\n",
                     kWidth, kHeight, path);
        return -1;
    }
    return 0;
}

int load_file(const char *path, std::vector<std::uint8_t> &data)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot open '%s': %s\n",
                     path, std::strerror(errno));
        return -1;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return -1;
    }
    const long file_size = std::ftell(file);
    if (file_size <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return -1;
    }

    try {
        data.resize(static_cast<std::size_t>(file_size));
    } catch (...) {
        std::fclose(file);
        return -1;
    }
    const bool read_failed =
        std::fread(data.data(), 1, data.size(), file) != data.size();
    const int close_ret = std::fclose(file);
    if (read_failed || close_ret != 0) {
        std::fprintf(stderr, "Failed to read '%s'.\n", path);
        data.clear();
        return -1;
    }
    return 0;
}

int save_file(const char *path, const std::vector<std::uint8_t> &data)
{
    if (data.empty()) {
        std::fprintf(stderr, "Refusing to write empty output '%s'.\n", path);
        return -1;
    }
    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot create '%s': %s\n",
                     path, std::strerror(errno));
        return -1;
    }
    const bool write_failed =
        std::fwrite(data.data(), 1, data.size(), file) != data.size();
    const int close_ret = std::fclose(file);
    if (write_failed || close_ret != 0) {
        std::fprintf(stderr, "Failed to write '%s'.\n", path);
        return -1;
    }
    std::printf("Wrote %s (%zu bytes).\n", path, data.size());
    return 0;
}

std::string join_path(const std::string &directory, const char *name)
{
    if (directory.empty() || directory == ".") {
        return name;
    }
    if (directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

void print_usage(const char *program)
{
    std::fprintf(stderr,
                 "Usage: %s [input.nv12 input.jpg output_dir]\n"
                 "  Run NV12->JPEG, NV12->H.264 and JPEG->H.264 conversions.\n"
                 "  Defaults: %s + %s -> current directory\n",
                 program, kDefaultNv12InputPath, kDefaultJpegInputPath);
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
    if (argc != 1 && argc != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *nv12_input_path =
        argc == 4 ? argv[1] : kDefaultNv12InputPath;
    const char *jpeg_input_path =
        argc == 4 ? argv[2] : kDefaultJpegInputPath;
    const std::string output_dir = argc == 4 ? argv[3] : ".";
    const std::string jpeg_output_path =
        join_path(output_dir, kJpegOutputName);
    const std::string nv12_h264_output_path =
        join_path(output_dir, kNv12H264OutputName);
    const std::string jpeg_h264_output_path =
        join_path(output_dir, kJpegH264OutputName);
    horddt_codec::config cfg;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.frame_rate = kFrameRate;
    cfg.bit_rate = kBitRate;
    cfg.jpeg_quality = kJpegQuality;
    cfg.timeout_ms = kTimeoutMs;
    cfg.verbose = true;

    horddt_codec codec(cfg);
    if (!codec.is_initialized()) {
        std::fprintf(stderr, "horddt_codec initialization failed: %d\n",
                     codec.initialization_status());
        return EXIT_FAILURE;
    }

    int ret = hb_mem_module_open();
    if (ret != 0) {
        std::fprintf(stderr, "hb_mem_module_open failed: %d\n", ret);
        return EXIT_FAILURE;
    }

    hb_mem_graphic_buf_t nv12_buffer{};
    nv12_buffer.fd[0] = -1;
    ret = allocate_nv12_buffer(nv12_buffer);
    if (ret == 0) {
        ret = load_nv12(nv12_input_path, nv12_buffer);
    }

    std::vector<std::uint8_t> jpeg_output;
    std::vector<std::uint8_t> nv12_h264_output;
    std::vector<std::uint8_t> jpeg_input;
    std::vector<std::uint8_t> jpeg_h264_output;

    if (ret == 0) {
        ret = codec.nv12_to_jpeg(nv12_buffer, jpeg_output);
    }
    if (ret == 0) {
        ret = save_file(jpeg_output_path.c_str(), jpeg_output);
    }
    if (ret == 0) {
        ret = codec.nv12_to_h264(nv12_buffer, nv12_h264_output);
    }
    if (ret == 0) {
        ret = save_file(nv12_h264_output_path.c_str(), nv12_h264_output);
    }

    // Finish the first H.264 sequence. A fresh codec object below makes the
    // JPEG-derived H.264 output start with its own SPS/PPS and IDR frame, so
    // both sample .h264 files can be decoded independently.
    codec.close();

    if (ret == 0) {
        ret = load_file(jpeg_input_path, jpeg_input);
    }
    if (ret == 0) {
        horddt_codec jpeg_codec(cfg);
        if (!jpeg_codec.is_initialized()) {
            ret = jpeg_codec.initialization_status();
            std::fprintf(stderr,
                         "JPEG-to-H264 codec initialization failed: %d\n",
                         ret);
        } else {
            ret = jpeg_codec.jpeg_to_h264(jpeg_input, jpeg_h264_output);
            if (ret == 0) {
                ret = save_file(jpeg_h264_output_path.c_str(), jpeg_h264_output);
            }
        }
        jpeg_codec.close();
    }

    if (nv12_buffer.fd[0] >= 0) {
        const int free_ret = hb_mem_free_buf(nv12_buffer.fd[0]);
        if (ret == 0 && free_ret != 0) {
            ret = free_ret;
        }
    }
    const int mem_close_ret = hb_mem_module_close();
    if (ret == 0 && mem_close_ret != 0) {
        ret = mem_close_ret;
    }

    if (ret != 0) {
        std::fprintf(stderr, "Codec sample failed: %d\n", ret);
        return EXIT_FAILURE;
    }

    std::printf("All hardware codec conversions completed.\n");
    return EXIT_SUCCESS;
}
