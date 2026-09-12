/***************************************************************************
 * NV12 right-angle rotation based on:
 *   multimedia_samples/sample_gdc/5-gdc_equisolid/gdc_equisolid.c
 *   multimedia_samples/sample_gdc/6-gdc_transformation/
 *       gdc_transformation.c and gdc_res/Affine.json
 *
 * The sample's Affine transformation is generated in memory with
 * hbn_gen_gdc_cfg(), then executed by a feedback-mode GDC vnode.
 ***************************************************************************/

#include "horddt_rotate.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/time.h>

extern "C" {
#include "gdc_cfg.h"
#include "gdc_bin_cfg.h"
#include "hb_gdc_data_info.h"
#include "hbn_vpf_data_info.h"
#include "hbn_vpf_interface.h"
}

namespace {

constexpr int kAutoAllocId = -1;
constexpr int kChannelId = 0;
constexpr std::uint32_t kGdcMagicNumber = 0x12345678U;

std::uint32_t align16(std::uint32_t value)
{
    return (value + 15U) & ~15U;
}

bool swaps_dimensions(horddt_rotate::angle rotation)
{
    return rotation == horddt_rotate::angle::rotate_90 ||
           rotation == horddt_rotate::angle::rotate_270;
}

bool valid_rotation(horddt_rotate::angle rotation)
{
    return rotation == horddt_rotate::angle::rotate_90 ||
           rotation == horddt_rotate::angle::rotate_180 ||
           rotation == horddt_rotate::angle::rotate_270;
}

unsigned int rotation_degrees(horddt_rotate::angle rotation)
{
    return static_cast<unsigned int>(rotation);
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
        std::fprintf(stderr, "%s must have two mapped NV12 planes.\n", name);
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

struct horddt_rotate::impl {
    config cfg{};
    std::uint32_t rotated_width = 0;
    std::uint32_t rotated_height = 0;
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
        if (cfg.input_width == 0U || cfg.input_height == 0U) {
            std::fprintf(stderr, "GDC rotation dimensions must be non-zero.\n");
            return -1;
        }
        if ((cfg.input_width & 1U) != 0U ||
            (cfg.input_height & 1U) != 0U) {
            std::fprintf(stderr, "NV12 rotation dimensions must be even.\n");
            return -1;
        }
        if (!valid_rotation(cfg.rotation)) {
            std::fprintf(stderr,
                         "GDC rotation must be 90, 180, or 270 degrees.\n");
            return -1;
        }
        if (cfg.input_width >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            cfg.input_height >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            std::fprintf(stderr,
                         "GDC rotation dimensions exceed the SDK range.\n");
            return -1;
        }
        if (cfg.timeout_ms <= 0 || cfg.hw_id < 0) {
            std::fprintf(stderr,
                         "GDC timeout must be positive and hw_id non-negative.\n");
            return -1;
        }
        if (cfg.output_buffer_count == 0U ||
            cfg.output_buffer_count >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            std::fprintf(stderr,
                         "GDC output buffer count must be positive.\n");
            return -1;
        }

        rotated_width = swaps_dimensions(cfg.rotation)
                            ? cfg.input_height
                            : cfg.input_width;
        rotated_height = swaps_dimensions(cfg.rotation)
                             ? cfg.input_width
                             : cfg.input_height;
        configured_input_stride =
            cfg.input_stride == 0U ? align16(cfg.input_width)
                                   : cfg.input_stride;
        configured_output_stride =
            cfg.output_stride == 0U ? align16(rotated_width)
                                    : cfg.output_stride;
        if (configured_input_stride < cfg.input_width ||
            configured_output_stride < rotated_width ||
            (configured_input_stride & 15U) != 0U ||
            (configured_output_stride & 15U) != 0U) {
            std::fprintf(stderr,
                         "GDC strides must cover the image width and be "
                         "16-byte aligned (input=%u, output=%u).\n",
                         configured_input_stride, configured_output_stride);
            return -1;
        }
        return 0;
    }

    int generate_gdc_binary()
    {
        param_t parameters{};
        parameters.in.w = cfg.input_width;
        parameters.in.h = cfg.input_height;
        parameters.out.w = rotated_width;
        parameters.out.h = rotated_height;
        // Match the Affine.json transformation sample. These lens fields are
        // required by the generator even though AFFINE does not dewarp a lens.
        parameters.fov = 160;
        parameters.diameter = cfg.input_height;
        parameters.x_offset = 0;
        parameters.y_offset = 0;
        parameters.format = FMT_SEMIPLANAR_420;

        window_t window{};
        window.strength = 1.0;
        window.strengthY = 1.0;
        window.angle = static_cast<double>(rotation_degrees(cfg.rotation));
        window.elevation = 0;
        window.azimuth = 0;
        window.keep_ratio = 1;
        window.FOV_h = 90;
        window.FOV_w = 90;
        window.cylindricity_y = 0;
        window.cylindricity_x = 0;
        window.trapezoid_left_angle = 90;
        window.trapezoid_right_angle = 90;
        window.pan = 0;
        window.tilt = 0;
        window.zoom = 1.0;
        window.out_r.x = 0;
        window.out_r.y = 0;
        window.out_r.w = rotated_width;
        window.out_r.h = rotated_height;
        window.input_roi_r.x = 0;
        window.input_roi_r.y = 0;
        window.input_roi_r.w = cfg.input_width;
        window.input_roi_r.h = cfg.input_height;
        window.transform = AFFINE;
        window.custom.full_tile_calc = 0;

        void *generated_config = nullptr;
        std::uint64_t config_size = 0;
        int ret = hbn_gen_gdc_cfg(&parameters, &window, 1U,
                                  &generated_config, &config_size);
        if (ret != 0 || generated_config == nullptr || config_size == 0U) {
            std::fprintf(stderr,
                         "hbn_gen_gdc_cfg(AFFINE %u) failed: %d\n",
                         rotation_degrees(cfg.rotation), ret);
            if (generated_config != nullptr) {
                (void)hbn_free_gdc_cfg(
                    static_cast<std::uint32_t *>(generated_config));
            }
            return ret != 0 ? ret : -1;
        }

        constexpr std::int64_t flags =
            HB_MEM_USAGE_MAP_INITIALIZED |
            HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
            HB_MEM_USAGE_CPU_READ_OFTEN |
            HB_MEM_USAGE_CPU_WRITE_OFTEN |
            HB_MEM_USAGE_CACHED;
        ret = hb_mem_alloc_com_buf(config_size, flags, &gdc_bin);
        if (ret == 0) {
            bin_allocated = true;
            if (gdc_bin.virt_addr == nullptr) {
                ret = -1;
            }
        }
        if (ret == 0) {
            std::memcpy(gdc_bin.virt_addr, generated_config,
                        static_cast<std::size_t>(config_size));
            ret = hb_mem_flush_buf(gdc_bin.fd, 0U, config_size);
        }
        (void)hbn_free_gdc_cfg(
            static_cast<std::uint32_t *>(generated_config));

        if (ret != 0) {
            std::fprintf(stderr,
                         "Failed to allocate/flush generated GDC config: %d\n",
                         ret);
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

        gdc_settings_t settings{};
        settings.gdc_config.config_addr = gdc_bin.phys_addr;
        settings.gdc_config.config_size = gdc_bin.size;
        settings.gdc_config.input_width = cfg.input_width;
        settings.gdc_config.input_height = cfg.input_height;
        settings.gdc_config.input_stride = configured_input_stride;
        settings.gdc_config.output_width = rotated_width;
        settings.gdc_config.output_height = rotated_height;
        settings.gdc_config.output_stride = configured_output_stride;
        settings.gdc_config.div_width = 0;
        settings.gdc_config.div_height = 0;
        settings.gdc_config.total_planes = 2;
        settings.binary_ion_id = gdc_bin.share_id;
        settings.binary_offset = gdc_bin.offset;
        settings.magicNumber = kGdcMagicNumber;

        ret = hbn_vnode_set_attr(vnode, &settings);
        if (ret == 0) {
            ret = hbn_vnode_set_ichn_attr(vnode, kChannelId, &settings);
        }
        if (ret == 0) {
            ret = hbn_vnode_set_ochn_attr(vnode, kChannelId, &settings);
        }
        if (ret != 0) {
            std::fprintf(stderr,
                         "Failed to configure GDC rotation vnode: %d\n", ret);
            return ret;
        }

        hbn_buf_alloc_attr_t alloc_attr{};
        alloc_attr.buffers_num =
            static_cast<int>(cfg.output_buffer_count);
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

        // Standalone buffer-to-buffer processing uses feedback mode, matching
        // sample_gdc's VNODE_WORK_MODE_FEEDBACK path.
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
            ret = generate_gdc_binary();
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
                "horddt_rotate initialized: %ux%u(stride=%u) --%u deg--> "
                "%ux%u(stride=%u)\n",
                cfg.input_width, cfg.input_height, configured_input_stride,
                rotation_degrees(cfg.rotation), rotated_width, rotated_height,
                configured_output_stride);
        }
        return 0;
    }

    int copy_output(const hb_mem_graphic_buf_t &source,
                    hb_mem_graphic_buf_t &destination) const
    {
        int ret = invalidate_nv12_buffer(source, "GDC rotation output");
        if (ret != 0) {
            return ret;
        }

        const auto *source_y =
            static_cast<const std::uint8_t *>(source.virt_addr[0]);
        const auto *source_uv =
            static_cast<const std::uint8_t *>(source.virt_addr[1]);
        auto *destination_y =
            static_cast<std::uint8_t *>(destination.virt_addr[0]);
        auto *destination_uv =
            static_cast<std::uint8_t *>(destination.virt_addr[1]);

        for (std::uint32_t row = 0; row < rotated_height; ++row) {
            std::memcpy(destination_y +
                            static_cast<std::size_t>(row) * destination.stride,
                        source_y +
                            static_cast<std::size_t>(row) * source.stride,
                        rotated_width);
        }
        for (std::uint32_t row = 0; row < rotated_height / 2U; ++row) {
            std::memcpy(destination_uv +
                            static_cast<std::size_t>(row) * destination.stride,
                        source_uv +
                            static_cast<std::size_t>(row) * source.stride,
                        rotated_width);
        }
        return 0;
    }

    int process(const hb_mem_graphic_buf_t &input_buffer,
                hb_mem_graphic_buf_t *output_buffer,
                const output_callback *callback)
    {
        if (!initialized) {
            std::fprintf(stderr, "horddt_rotate is not initialized.\n");
            return initialization_result != 0 ? initialization_result : -1;
        }

        int ret = validate_nv12_buffer(input_buffer, cfg.input_width,
                                       cfg.input_height,
                                       "GDC rotation input buffer");
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

        if (output_buffer != nullptr) {
            ret = validate_nv12_buffer(*output_buffer, rotated_width,
                                       rotated_height,
                                       "Rotation destination buffer");
            if (ret != 0) {
                return ret;
            }
        } else if (callback == nullptr || !(*callback)) {
            std::fprintf(stderr,
                         "GDC rotation output callback must not be empty.\n");
            return -1;
        }

        if (cfg.sync_input_for_device) {
            ret = flush_nv12_buffer(input_buffer, "GDC rotation input");
            if (ret != 0) {
                return ret;
            }
        }

        hbn_vnode_image_t input_frame{};
        input_frame.buffer = input_buffer;
        gettimeofday(&input_frame.info.tv, nullptr);

        ret = hbn_vnode_sendframe(vnode, kChannelId, &input_frame);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_sendframe(GDC rotation) failed: %d\n",
                         ret);
            return ret;
        }

        hbn_vnode_image_t gdc_output{};
        ret = hbn_vnode_getframe(vnode, kChannelId, cfg.timeout_ms,
                                 &gdc_output);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_getframe(GDC rotation) failed: %d\n",
                         ret);
            return ret;
        }

        if (cfg.verbose) {
            std::printf(
                "GDC rotation output: %dx%d, stride=%d, vstride=%d, "
                "planes=%d\n",
                gdc_output.buffer.width, gdc_output.buffer.height,
                gdc_output.buffer.stride, gdc_output.buffer.vstride,
                gdc_output.buffer.plane_cnt);
        }

        ret = validate_nv12_buffer(gdc_output.buffer, rotated_width,
                                   rotated_height,
                                   "GDC rotation output buffer");
        if (ret == 0) {
            if (output_buffer != nullptr) {
                ret = copy_output(gdc_output.buffer, *output_buffer);
            } else {
                if (cfg.sync_borrowed_output_for_cpu) {
                    ret = invalidate_nv12_buffer(
                        gdc_output.buffer, "GDC rotation output");
                }
                if (ret == 0) {
                    try {
                        ret = (*callback)(gdc_output.buffer);
                    } catch (...) {
                        std::fprintf(stderr,
                                     "GDC rotation callback threw an "
                                     "exception.\n");
                        ret = -1;
                    }
                }
            }
        }

        const int release_ret =
            hbn_vnode_releaseframe(vnode, kChannelId, &gdc_output);
        if (release_ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_releaseframe(GDC rotation) failed: %d\n",
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
                             "hbn_vnode_stop(GDC rotation) failed during "
                             "cleanup: %d\n",
                             ret);
            }
            vnode_started = false;
        }
        if (vnode_opened) {
            const int ret = hbn_vnode_close(vnode);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hbn_vnode_close(GDC rotation) failed during "
                             "cleanup: %d\n",
                             ret);
            }
            vnode_opened = false;
        }
        if (bin_allocated) {
            const int ret = hb_mem_free_buf(gdc_bin.fd);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hb_mem_free_buf(GDC rotation binary) failed: "
                             "%d\n",
                             ret);
            }
            bin_allocated = false;
        }
        if (mem_opened) {
            const int ret = hb_mem_module_close();
            if (ret != 0) {
                std::fprintf(stderr,
                             "hb_mem_module_close failed during GDC rotation "
                             "cleanup: %d\n",
                             ret);
            }
            mem_opened = false;
        }
    }
};

horddt_rotate::horddt_rotate(const config &cfg) : impl_(new impl())
{
    impl_->initialize(cfg);
}

horddt_rotate::~horddt_rotate()
{
    close();
}

int horddt_rotate::rotate(const hb_mem_graphic_buf_t &input_buffer,
                          hb_mem_graphic_buf_t &output_buffer)
{
    return impl_ != nullptr
               ? impl_->process(input_buffer, &output_buffer, nullptr)
               : -1;
}

int horddt_rotate::rotate_borrowed(
    const hb_mem_graphic_buf_t &input_buffer,
    const output_callback &callback)
{
    return impl_ != nullptr
               ? impl_->process(input_buffer, nullptr, &callback)
               : -1;
}

std::uint32_t horddt_rotate::output_width() const noexcept
{
    return impl_ != nullptr ? impl_->rotated_width : 0U;
}

std::uint32_t horddt_rotate::output_height() const noexcept
{
    return impl_ != nullptr ? impl_->rotated_height : 0U;
}

void horddt_rotate::close()
{
    if (impl_ != nullptr) {
        impl_->release();
    }
}

bool horddt_rotate::is_initialized() const noexcept
{
    return impl_ != nullptr && impl_->initialized;
}

int horddt_rotate::initialization_status() const noexcept
{
    return impl_ != nullptr ? impl_->initialization_result : -1;
}
