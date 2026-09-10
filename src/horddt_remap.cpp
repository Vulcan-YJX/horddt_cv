/***************************************************************************
 * NV12 distortion correction class based on:
 *   multimedia_samples/sample_pipeline/common/vp_pipeline.c
 *   multimedia_samples/sample_gdc/3-gdc_static_valid/gdc_static_valid.c
 ***************************************************************************/

#include "horddt_remap.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/time.h>
#include <vector>

extern "C" {
#include "gdc_cfg.h"
#include "gdc_bin_cfg.h"
#include "hbn_vpf_data_info.h"
#include "hbn_vpf_interface.h"
}

namespace {

constexpr int kAutoAllocId = -1;
constexpr int kChannelId = 0;
constexpr int kOutputBufferCount = 3;
constexpr std::uint32_t kGdcMagicNumber = 0x12345678U;

std::uint32_t align16(std::uint32_t value)
{
    return (value + 15U) & ~15U;
}

int validate_plane_size(const hb_mem_graphic_buf_t &buffer,
                        std::uint32_t rows,
                        int plane,
                        const char *name)
{
    const std::uint64_t required =
        static_cast<std::uint64_t>(buffer.stride) * rows;
    if (buffer.size[plane] < required) {
        std::fprintf(stderr,
                     "%s plane %d is too small: size=%llu, required=%llu.\n",
                     name, plane,
                     static_cast<unsigned long long>(buffer.size[plane]),
                     static_cast<unsigned long long>(required));
        return -1;
    }
    return 0;
}

int validate_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                         std::uint32_t expected_width,
                         std::uint32_t expected_height,
                         const char *name)
{
    if (buffer.format != MEM_PIX_FMT_NV12) {
        std::fprintf(stderr, "%s must use MEM_PIX_FMT_NV12 (actual=%d).\n",
                     name, buffer.format);
        return -1;
    }
    if (buffer.width <= 0 || buffer.height <= 0 ||
        static_cast<std::uint32_t>(buffer.width) != expected_width ||
        static_cast<std::uint32_t>(buffer.height) != expected_height) {
        std::fprintf(stderr,
                     "%s dimensions must be %ux%u (actual=%dx%d).\n",
                     name, expected_width, expected_height,
                     buffer.width, buffer.height);
        return -1;
    }
    if (buffer.plane_cnt < 2 || buffer.virt_addr[0] == nullptr ||
        buffer.virt_addr[1] == nullptr) {
        std::fprintf(stderr,
                     "%s must have two mapped NV12 planes.\n", name);
        return -1;
    }
    if (buffer.stride < buffer.width || buffer.vstride < buffer.height) {
        std::fprintf(stderr,
                     "%s has invalid stride=%d or vstride=%d for %dx%d.\n",
                     name, buffer.stride, buffer.vstride,
                     buffer.width, buffer.height);
        return -1;
    }

    if (validate_plane_size(buffer, expected_height, 0, name) != 0 ||
        validate_plane_size(buffer, expected_height / 2U, 1, name) != 0) {
        return -1;
    }
    return 0;
}

int flush_nv12_buffer(const hb_mem_graphic_buf_t &buffer, const char *name)
{
    const std::uint64_t y_bytes =
        static_cast<std::uint64_t>(buffer.stride) *
        static_cast<std::uint64_t>(buffer.height);
    const std::uint64_t uv_bytes =
        static_cast<std::uint64_t>(buffer.stride) *
        static_cast<std::uint64_t>(buffer.height / 2);

    int ret = hb_mem_flush_buf_with_vaddr(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
            buffer.virt_addr[0])),
        y_bytes);
    if (ret != 0) {
        std::fprintf(stderr, "Failed to flush %s Y plane: %d\n", name, ret);
        return ret;
    }

    ret = hb_mem_flush_buf_with_vaddr(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
            buffer.virt_addr[1])),
        uv_bytes);
    if (ret != 0) {
        std::fprintf(stderr, "Failed to flush %s UV plane: %d\n", name, ret);
    }
    return ret;
}

int invalidate_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                           const char *name)
{
    const std::uint64_t y_bytes =
        static_cast<std::uint64_t>(buffer.stride) *
        static_cast<std::uint64_t>(buffer.height);
    const std::uint64_t uv_bytes =
        static_cast<std::uint64_t>(buffer.stride) *
        static_cast<std::uint64_t>(buffer.height / 2);

    int ret = hb_mem_invalidate_buf_with_vaddr(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
            buffer.virt_addr[0])),
        y_bytes);
    if (ret != 0) {
        std::fprintf(stderr, "Failed to invalidate %s Y plane: %d\n",
                     name, ret);
        return ret;
    }

    ret = hb_mem_invalidate_buf_with_vaddr(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
            buffer.virt_addr[1])),
        uv_bytes);
    if (ret != 0) {
        std::fprintf(stderr, "Failed to invalidate %s UV plane: %d\n",
                     name, ret);
    }
    return ret;
}

}  // namespace

struct horddt_remap::impl {
    config cfg{};
    std::uint32_t configured_input_stride = 0;
    std::uint32_t configured_output_stride = 0;
    int initialization_result = -1;

    hbn_vnode_handle_t vnode{};
    hb_mem_common_buf_t gdc_bin{};

    bool initialized = false;
    bool mem_opened = false;
    bool bin_allocated = false;
    bool vnode_opened = false;
    bool vnode_started = false;

    int validate_config()
    {
        if (cfg.input_width == 0U || cfg.input_height == 0U ||
            cfg.output_width == 0U || cfg.output_height == 0U) {
            std::fprintf(stderr, "GDC input/output dimensions must be non-zero.\n");
            return -1;
        }
        if ((cfg.input_width & 1U) != 0U ||
            (cfg.input_height & 1U) != 0U ||
            (cfg.output_width & 1U) != 0U ||
            (cfg.output_height & 1U) != 0U) {
            std::fprintf(stderr,
                         "NV12 input/output dimensions must be even.\n");
            return -1;
        }
        if (cfg.input_width >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            cfg.input_height >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            cfg.output_width >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            cfg.output_height >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            std::fprintf(stderr, "GDC dimensions exceed the SDK integer range.\n");
            return -1;
        }
        if (cfg.gdc_bin_path.empty()) {
            std::fprintf(stderr, "GDC binary path must not be empty.\n");
            return -1;
        }
        if (cfg.timeout_ms <= 0) {
            std::fprintf(stderr, "GDC timeout must be greater than zero.\n");
            return -1;
        }
        if (cfg.hw_id < 0) {
            std::fprintf(stderr, "GDC hardware id must not be negative.\n");
            return -1;
        }

        configured_input_stride =
            cfg.input_stride == 0U ? align16(cfg.input_width) : cfg.input_stride;
        configured_output_stride = cfg.output_stride == 0U
                                       ? align16(cfg.output_width)
                                       : cfg.output_stride;
        if (configured_input_stride < cfg.input_width ||
            configured_output_stride < cfg.output_width ||
            (configured_input_stride & 15U) != 0U ||
            (configured_output_stride & 15U) != 0U) {
            std::fprintf(stderr,
                         "GDC strides must be at least the image width and "
                         "16-byte aligned (input=%u, output=%u).\n",
                         configured_input_stride, configured_output_stride);
            return -1;
        }
        return 0;
    }

    int load_gdc_binary()
    {
        FILE *file = std::fopen(cfg.gdc_bin_path.c_str(), "rb");
        if (file == nullptr) {
            std::fprintf(stderr, "Cannot open GDC binary '%s': %s\n",
                         cfg.gdc_bin_path.c_str(), std::strerror(errno));
            return -1;
        }

        int ret = 0;
        if (std::fseek(file, 0, SEEK_END) != 0) {
            ret = -1;
        }
        const long file_size = ret == 0 ? std::ftell(file) : -1L;
        if (file_size <= 0L || std::fseek(file, 0, SEEK_SET) != 0) {
            ret = -1;
        }
        if (ret != 0) {
            std::fprintf(stderr, "Cannot determine GDC binary size: %s\n",
                         cfg.gdc_bin_path.c_str());
            std::fclose(file);
            return -1;
        }

        std::vector<std::uint8_t> data(static_cast<std::size_t>(file_size));
        if (std::fread(data.data(), 1, data.size(), file) != data.size()) {
            std::fprintf(stderr, "Failed to read GDC binary '%s'.\n",
                         cfg.gdc_bin_path.c_str());
            std::fclose(file);
            return -1;
        }
        if (std::fclose(file) != 0) {
            std::fprintf(stderr, "Failed to close GDC binary '%s'.\n",
                         cfg.gdc_bin_path.c_str());
            return -1;
        }

        constexpr std::int64_t flags =
            HB_MEM_USAGE_MAP_INITIALIZED |
            HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
            HB_MEM_USAGE_CPU_READ_OFTEN |
            HB_MEM_USAGE_CPU_WRITE_OFTEN |
            HB_MEM_USAGE_CACHED;
        std::memset(&gdc_bin, 0, sizeof(gdc_bin));
        ret = hb_mem_alloc_com_buf(static_cast<std::uint64_t>(data.size()),
                                   flags, &gdc_bin);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mem_alloc_com_buf for GDC binary failed: %d\n",
                         ret);
            return ret;
        }
        bin_allocated = true;
        if (gdc_bin.virt_addr == nullptr) {
            std::fprintf(stderr,
                         "hb_mem_alloc_com_buf returned a null virtual "
                         "address.\n");
            return -1;
        }

        std::memcpy(gdc_bin.virt_addr, data.data(), data.size());
        ret = hb_mem_flush_buf(gdc_bin.fd, 0U,
                               static_cast<std::uint64_t>(data.size()));
        if (ret != 0) {
            std::fprintf(stderr, "Failed to flush GDC binary: %d\n", ret);
        }
        return ret;
    }

    int start_gdc()
    {
        int ret = hbn_vnode_open(HB_GDC, cfg.hw_id, kAutoAllocId, &vnode);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_open(HB_GDC) failed: %d\n", ret);
            return ret;
        }
        vnode_opened = true;

        // Use the aggregate gdc_settings_t layout used by the GDC pipeline
        // samples. The SDK validates magicNumber and expects the same settings
        // object for node/input/output attributes.
        gdc_settings_t settings{};
        settings.gdc_config.config_addr = gdc_bin.phys_addr;
        settings.gdc_config.config_size = gdc_bin.size;
        settings.gdc_config.input_width = cfg.input_width;
        settings.gdc_config.input_height = cfg.input_height;
        settings.gdc_config.input_stride = configured_input_stride;
        settings.gdc_config.output_width = cfg.output_width;
        settings.gdc_config.output_height = cfg.output_height;
        settings.gdc_config.output_stride = configured_output_stride;
        settings.gdc_config.div_width = 0;
        settings.gdc_config.div_height = 0;
        settings.gdc_config.total_planes = 2;
        settings.binary_ion_id = gdc_bin.share_id;
        settings.binary_offset = gdc_bin.offset;
        settings.magicNumber = kGdcMagicNumber;

        ret = hbn_vnode_set_attr(vnode, &settings);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_attr(GDC) failed: %d "
                         "(bin_size=%llu, phys=0x%llx, share_id=%d, "
                         "offset=%llu, magic=0x%08x)\n",
                         ret,
                         static_cast<unsigned long long>(gdc_bin.size),
                         static_cast<unsigned long long>(gdc_bin.phys_addr),
                         gdc_bin.share_id,
                         static_cast<unsigned long long>(gdc_bin.offset),
                         kGdcMagicNumber);
            return ret;
        }

        ret = hbn_vnode_set_ichn_attr(vnode, kChannelId, &settings);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_ichn_attr(GDC) failed: %d\n", ret);
            return ret;
        }

        ret = hbn_vnode_set_ochn_attr(vnode, kChannelId, &settings);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_ochn_attr(GDC) failed: %d\n", ret);
            return ret;
        }

        hbn_buf_alloc_attr_t alloc_attr{};
        alloc_attr.buffers_num = kOutputBufferCount;
        alloc_attr.is_contig = 1;
        alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
                           HB_MEM_USAGE_CPU_WRITE_OFTEN |
                           HB_MEM_USAGE_CACHED;
        ret = hbn_vnode_set_ochn_buf_attr(vnode, kChannelId, &alloc_attr);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_ochn_buf_attr(GDC) failed: %d\n", ret);
            return ret;
        }

        // Feedback mode is the sample_gdc path for standalone buffer-to-buffer
        // operation; no vflow is needed for a single GDC node.
        ret = hbn_vnode_start(vnode);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_start(GDC) failed: %d\n", ret);
            return ret;
        }
        vnode_started = true;
        return 0;
    }

    int initialize(const config &new_cfg)
    {
        cfg = new_cfg;
        int ret = validate_config();
        if (ret == 0) {
            ret = hb_mem_module_open();
            if (ret == 0) {
                mem_opened = true;
            } else {
                std::fprintf(stderr, "hb_mem_module_open failed: %d\n", ret);
            }
        }
        if (ret == 0) {
            ret = load_gdc_binary();
        }
        if (ret == 0) {
            ret = start_gdc();
        }
        if (ret != 0) {
            release();
            initialization_result = ret;
            return ret;
        }

        initialized = true;
        initialization_result = 0;
        if (cfg.verbose) {
            std::printf(
                "horddt_remap initialized: %ux%u(stride=%u) -> "
                "%ux%u(stride=%u), GDC bin=%s\n",
                cfg.input_width, cfg.input_height, configured_input_stride,
                cfg.output_width, cfg.output_height, configured_output_stride,
                cfg.gdc_bin_path.c_str());
        }
        return 0;
    }

    int copy_output(const hb_mem_graphic_buf_t &source,
                    hb_mem_graphic_buf_t &destination) const
    {
        int ret = invalidate_nv12_buffer(source, "GDC output buffer");
        if (ret != 0) {
            return ret;
        }

        const std::size_t row_bytes =
            static_cast<std::size_t>(cfg.output_width);
        const std::size_t source_stride =
            static_cast<std::size_t>(source.stride);
        const std::size_t destination_stride =
            static_cast<std::size_t>(destination.stride);

        const auto *source_y = source.virt_addr[0];
        auto *destination_y = destination.virt_addr[0];
        for (std::uint32_t row = 0; row < cfg.output_height; ++row) {
            std::memcpy(destination_y +
                            static_cast<std::size_t>(row) * destination_stride,
                        source_y +
                            static_cast<std::size_t>(row) * source_stride,
                        row_bytes);
        }

        const auto *source_uv = source.virt_addr[1];
        auto *destination_uv = destination.virt_addr[1];
        for (std::uint32_t row = 0; row < cfg.output_height / 2U; ++row) {
            std::memcpy(destination_uv +
                            static_cast<std::size_t>(row) * destination_stride,
                        source_uv +
                            static_cast<std::size_t>(row) * source_stride,
                        row_bytes);
        }
        return flush_nv12_buffer(destination, "remap output buffer");
    }

    int process(const hb_mem_graphic_buf_t &input_buffer,
                hb_mem_graphic_buf_t &output_buffer)
    {
        if (!initialized) {
            std::fprintf(stderr, "horddt_remap is not initialized.\n");
            return initialization_result != 0 ? initialization_result : -1;
        }

        int ret = validate_nv12_buffer(input_buffer, cfg.input_width,
                                       cfg.input_height, "GDC input buffer");
        if (ret != 0) {
            return ret;
        }
        if (static_cast<std::uint32_t>(input_buffer.stride) !=
            configured_input_stride) {
            std::fprintf(stderr,
                         "GDC input stride must equal configured stride %u "
                         "(actual=%d).\n",
                         configured_input_stride, input_buffer.stride);
            return -1;
        }

        ret = validate_nv12_buffer(output_buffer, cfg.output_width,
                                   cfg.output_height, "Remap output buffer");
        if (ret != 0) {
            return ret;
        }

        ret = flush_nv12_buffer(input_buffer, "GDC input buffer");
        if (ret != 0) {
            return ret;
        }

        hbn_vnode_image_t input_frame{};
        input_frame.buffer = input_buffer;
        gettimeofday(&input_frame.info.tv, nullptr);

        ret = hbn_vnode_sendframe(vnode, kChannelId, &input_frame);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_sendframe(GDC) failed: %d\n", ret);
            return ret;
        }

        hbn_vnode_image_t gdc_output{};
        ret = hbn_vnode_getframe(vnode, kChannelId, cfg.timeout_ms,
                                 &gdc_output);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_getframe(GDC) timed out or failed: %d\n",
                         ret);
            return ret;
        }

        if (cfg.verbose) {
            std::printf("GDC output: %dx%d, stride=%d, vstride=%d, planes=%d\n",
                        gdc_output.buffer.width, gdc_output.buffer.height,
                        gdc_output.buffer.stride, gdc_output.buffer.vstride,
                        gdc_output.buffer.plane_cnt);
        }

        ret = validate_nv12_buffer(gdc_output.buffer, cfg.output_width,
                                   cfg.output_height, "GDC output buffer");
        if (ret == 0) {
            ret = copy_output(gdc_output.buffer, output_buffer);
        }

        const int release_ret =
            hbn_vnode_releaseframe(vnode, kChannelId, &gdc_output);
        if (release_ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_releaseframe(GDC) failed: %d\n",
                         release_ret);
            if (ret == 0) {
                ret = release_ret;
            }
        }
        return ret;
    }

    void release()
    {
        initialized = false;
        if (vnode_started) {
            const int ret = hbn_vnode_stop(vnode);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hbn_vnode_stop(GDC) failed during cleanup: %d\n",
                             ret);
            }
            vnode_started = false;
        }
        if (vnode_opened) {
            const int ret = hbn_vnode_close(vnode);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hbn_vnode_close(GDC) failed during cleanup: %d\n",
                             ret);
            }
            vnode_opened = false;
        }
        if (bin_allocated) {
            const int ret = hb_mem_free_buf(gdc_bin.fd);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hb_mem_free_buf(GDC binary) failed: %d\n", ret);
            }
            bin_allocated = false;
        }
        if (mem_opened) {
            hb_mem_module_close();
            mem_opened = false;
        }
    }
};

horddt_remap::horddt_remap(const config &cfg) : impl_(new impl())
{
    impl_->initialize(cfg);
}

horddt_remap::~horddt_remap()
{
    close();
}

int horddt_remap::remap(const hb_mem_graphic_buf_t &input_buffer,
                        hb_mem_graphic_buf_t &output_buffer)
{
    return impl_ != nullptr ? impl_->process(input_buffer, output_buffer) : -1;
}

void horddt_remap::close()
{
    if (impl_ != nullptr) {
        impl_->release();
    }
}

bool horddt_remap::is_initialized() const noexcept
{
    return impl_ != nullptr && impl_->initialized;
}

int horddt_remap::initialization_status() const noexcept
{
    return impl_ != nullptr ? impl_->initialization_result : -1;
}
