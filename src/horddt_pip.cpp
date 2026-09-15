/***************************************************************************
 * Hardware NV12 picture-in-picture compositor based on the STITCH vnode.
 ***************************************************************************/

#include "horddt_pip.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/time.h>

extern "C" {
#include "hb_mem_mgr.h"
#include "hbn_vpf_data_info.h"
#include "hbn_vpf_interface.h"

// The official sample_gdc_stitch uses hbn_sth_cfg.h. Keep the alternate
// names for SDK packages that install the same public structs under a more
// descriptive compatibility header.
#if defined(__has_include)
#if __has_include("hbn_sth_cfg.h")
#include "hbn_sth_cfg.h"
#elif __has_include("hbn_stitch_cfg.h")
#include "hbn_stitch_cfg.h"
#elif __has_include("stitch_cfg.h")
#include "stitch_cfg.h"
#else
#error "D-Robotics STITCH configuration header was not found"
#endif
#else
#include "hbn_sth_cfg.h"
#endif
}

namespace {

constexpr int kAutoAllocId = -1;
constexpr std::uint32_t kStitchHardwareId = 0U;
constexpr std::uint32_t kBackgroundChannel = 0U;
constexpr std::uint32_t kOverlayChannel = 1U;
constexpr std::uint32_t kOutputChannel = 0U;
constexpr std::uint32_t kImageCount = 2U;
constexpr std::uint32_t kMaximumStitchRoiDimension = 2000U;
constexpr std::size_t kMaximumPipRois =
    static_cast<std::size_t>(MAX_STH_ROI_NUMS);

// Values are defined by the SDK STITCH blending_mode/direct enums.  Numeric
// constants also keep this code compatible with SDK headers that expose only
// the structure definitions (the official configuration examples use 3/0).
constexpr std::uint32_t kBlendingModeSourceCopy = 3U;
constexpr std::uint32_t kBlendingDirectionLeftTop = 0U;

std::uint32_t align64(std::uint32_t value)
{
    return (value + 63U) & ~63U;
}

bool is_even(std::uint32_t value)
{
    return (value & 1U) == 0U;
}

int validate_config(horddt_pip::config &cfg)
{
    if (cfg.background_width == 0U || cfg.background_height == 0U ||
        cfg.overlay_width == 0U || cfg.overlay_height == 0U) {
        std::fprintf(stderr, "horddt_pip dimensions must not be zero.\n");
        return -1;
    }
    if (!is_even(cfg.background_width) ||
        !is_even(cfg.background_height) ||
        !is_even(cfg.overlay_width) || !is_even(cfg.overlay_height) ||
        !is_even(cfg.overlay_x) || !is_even(cfg.overlay_y)) {
        std::fprintf(stderr,
                     "horddt_pip NV12 dimensions and overlay coordinates "
                     "must be even.\n");
        return -1;
    }
    if (cfg.overlay_width > cfg.background_width ||
        cfg.overlay_height > cfg.background_height) {
        std::fprintf(stderr,
                     "horddt_pip overlay must not exceed the background.\n");
        return -1;
    }
    if (cfg.background_width > 4096U || cfg.background_height > 4096U ||
        cfg.overlay_width > kMaximumStitchRoiDimension ||
        cfg.overlay_height > kMaximumStitchRoiDimension) {
        std::fprintf(stderr,
                     "horddt_pip exceeds STITCH limits: images/output are "
                     "at most 4096x4096 and each selected ROI is at most "
                     "2000x2000.\n");
        return -1;
    }
    if (cfg.overlay_x > cfg.background_width - cfg.overlay_width ||
        cfg.overlay_y > cfg.background_height - cfg.overlay_height) {
        std::fprintf(stderr,
                     "horddt_pip overlay ROI (%u,%u %ux%u) is outside "
                     "the %ux%u output.\n",
                     cfg.overlay_x, cfg.overlay_y,
                     cfg.overlay_width, cfg.overlay_height,
                     cfg.background_width, cfg.background_height);
        return -1;
    }
    if (cfg.timeout_ms <= 0 || cfg.output_buffer_count == 0U) {
        std::fprintf(stderr,
                     "horddt_pip timeout and output buffer count must be "
                     "positive.\n");
        return -1;
    }

    if (cfg.background_stride == 0U) {
        cfg.background_stride = align64(cfg.background_width);
    }
    if (cfg.overlay_stride == 0U) {
        cfg.overlay_stride = align64(cfg.overlay_width);
    }
    if (cfg.output_stride == 0U) {
        cfg.output_stride = align64(cfg.background_width);
    }

    if (cfg.background_stride < cfg.background_width ||
        cfg.overlay_stride < cfg.overlay_width ||
        cfg.output_stride < cfg.background_width ||
        (cfg.background_stride & 15U) != 0U ||
        (cfg.overlay_stride & 15U) != 0U ||
        (cfg.output_stride & 15U) != 0U) {
        std::fprintf(stderr,
                     "horddt_pip strides must cover image width and be "
                     "16-byte aligned.\n");
        return -1;
    }
    return 0;
}

int validate_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                         std::uint32_t width,
                         std::uint32_t height,
                         std::uint32_t stride,
                         const char *name)
{
    if (buffer.format != MEM_PIX_FMT_NV12 || buffer.plane_cnt < 2 ||
        buffer.width != static_cast<int>(width) ||
        buffer.height != static_cast<int>(height) ||
        buffer.stride != static_cast<int>(stride) ||
        buffer.vstride < static_cast<int>(height) ||
        buffer.virt_addr[0] == nullptr || buffer.virt_addr[1] == nullptr) {
        std::fprintf(
            stderr,
            "%s must be NV12 %ux%u stride=%u; got format=%d, planes=%d, "
            "size=%dx%d, stride=%d, vstride=%d.\n",
            name, width, height, stride, buffer.format, buffer.plane_cnt,
            buffer.width, buffer.height, buffer.stride, buffer.vstride);
        return -1;
    }

    const std::uint64_t y_required =
        static_cast<std::uint64_t>(height - 1U) * stride + width;
    const std::uint64_t uv_required =
        static_cast<std::uint64_t>(height / 2U - 1U) * stride + width;
    if (buffer.size[0] < y_required || buffer.size[1] < uv_required) {
        std::fprintf(stderr,
                     "%s plane storage is too small for its configured "
                     "stride.\n",
                     name);
        return -1;
    }
    return 0;
}

int sync_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                     bool flush,
                     const char *name)
{
    const auto y_address =
        reinterpret_cast<std::uint64_t>(buffer.virt_addr[0]);
    const auto uv_address =
        reinterpret_cast<std::uint64_t>(buffer.virt_addr[1]);
    const int y_ret = flush
                          ? hb_mem_flush_buf_with_vaddr(y_address,
                                                       buffer.size[0])
                          : hb_mem_invalidate_buf_with_vaddr(y_address,
                                                            buffer.size[0]);
    if (y_ret != 0) {
        std::fprintf(stderr, "%s Y-plane cache sync failed: %d\n",
                     name, y_ret);
        return y_ret;
    }
    const int uv_ret = flush
                           ? hb_mem_flush_buf_with_vaddr(uv_address,
                                                        buffer.size[1])
                           : hb_mem_invalidate_buf_with_vaddr(uv_address,
                                                             buffer.size[1]);
    if (uv_ret != 0) {
        std::fprintf(stderr, "%s UV-plane cache sync failed: %d\n",
                     name, uv_ret);
    }
    return uv_ret;
}

int copy_nv12_buffer(const hb_mem_graphic_buf_t &source,
                     hb_mem_graphic_buf_t &destination,
                     std::uint32_t width,
                     std::uint32_t height)
{
    const std::size_t row_bytes = static_cast<std::size_t>(width);
    const std::size_t source_stride =
        static_cast<std::size_t>(source.stride);
    const std::size_t destination_stride =
        static_cast<std::size_t>(destination.stride);

    const auto *source_y =
        static_cast<const std::uint8_t *>(source.virt_addr[0]);
    auto *destination_y =
        static_cast<std::uint8_t *>(destination.virt_addr[0]);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination_y + row * destination_stride,
                    source_y + row * source_stride,
                    row_bytes);
    }

    const auto *source_uv =
        static_cast<const std::uint8_t *>(source.virt_addr[1]);
    auto *destination_uv =
        static_cast<std::uint8_t *>(destination.virt_addr[1]);
    for (std::uint32_t row = 0; row < height / 2U; ++row) {
        std::memcpy(destination_uv + row * destination_stride,
                    source_uv + row * source_stride,
                    row_bytes);
    }
    return sync_nv12_buffer(destination, true, "PIP output buffer");
}

struct pip_roi {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t source_index = kBackgroundChannel;
    std::uint32_t source_x = 0U;
    std::uint32_t source_y = 0U;
};

int build_non_overlapping_rois(
    const horddt_pip::config &cfg,
    std::array<pip_roi, kMaximumPipRois> &rois,
    std::size_t &count)
{
    count = 0U;
    const std::uint32_t overlay_right = cfg.overlay_x + cfg.overlay_width;
    const std::uint32_t overlay_bottom = cfg.overlay_y + cfg.overlay_height;

    const auto add_roi =
        [&rois, &count](std::uint32_t x, std::uint32_t y,
                        std::uint32_t width, std::uint32_t height,
                        std::uint32_t source_index,
                        std::uint32_t source_x,
                        std::uint32_t source_y) -> int {
            if (width == 0U || height == 0U) {
                return 0;
            }
            if (count >= rois.size()) {
                return -1;
            }
            pip_roi &roi = rois[count++];
            roi.x = x;
            roi.y = y;
            roi.width = width;
            roi.height = height;
            roi.source_index = source_index;
            roi.source_x = source_x;
            roi.source_y = source_y;
            return 0;
        };

    const auto add_background =
        [&add_roi](std::uint32_t x, std::uint32_t y,
                   std::uint32_t width, std::uint32_t height) -> int {
            // STITCH allows 4096x4096 input/output images but limits every
            // selected source ROI to 2000x2000. Split large background strips
            // into adjacent tiles while preserving source/output coordinates.
            for (std::uint32_t tile_y = 0U; tile_y < height;
                 tile_y += kMaximumStitchRoiDimension) {
                const std::uint32_t tile_height =
                    height - tile_y < kMaximumStitchRoiDimension
                        ? height - tile_y
                        : kMaximumStitchRoiDimension;
                for (std::uint32_t tile_x = 0U; tile_x < width;
                     tile_x += kMaximumStitchRoiDimension) {
                    const std::uint32_t tile_width =
                        width - tile_x < kMaximumStitchRoiDimension
                            ? width - tile_x
                            : kMaximumStitchRoiDimension;
                    const int ret = add_roi(
                        x + tile_x, y + tile_y, tile_width, tile_height,
                        kBackgroundChannel, x + tile_x, y + tile_y);
                    if (ret != 0) {
                        return ret;
                    }
                }
            }
            return 0;
        };

    // Partition the background around the PIP rectangle.  The official
    // STITCH documentation reserves Src Copy for non-overlapping ROIs, so
    // this avoids relying on undocumented overwrite ordering.
    int ret = add_background(0U, 0U,
                             cfg.background_width, cfg.overlay_y);
    if (ret == 0) {
        ret = add_background(0U, overlay_bottom, cfg.background_width,
                             cfg.background_height - overlay_bottom);
    }
    if (ret == 0) {
        ret = add_background(0U, cfg.overlay_y,
                             cfg.overlay_x, cfg.overlay_height);
    }
    if (ret == 0) {
        ret = add_background(overlay_right, cfg.overlay_y,
                             cfg.background_width - overlay_right,
                             cfg.overlay_height);
    }
    if (ret == 0) {
        ret = add_roi(cfg.overlay_x, cfg.overlay_y,
                      cfg.overlay_width, cfg.overlay_height,
                      kOverlayChannel, 0U, 0U);
    }
    if (ret != 0) {
        std::fprintf(stderr,
                     "PIP layout requires more than %zu STITCH ROIs.\n",
                     kMaximumPipRois);
    }
    return ret;
}

void set_input_roi(roi_info &roi,
                   std::uint32_t index,
                   std::uint32_t x,
                   std::uint32_t y)
{
    // sample_gdc_stitch configures only the source origin in each input
    // channel. The selected width/height come from the matching output ROI.
    roi.roi_index = index;
    roi.roi_x = x;
    roi.roi_y = y;
    roi.roi_w = 0U;
    roi.roi_h = 0U;
}

void set_output_roi(roi_info &roi,
                    std::uint32_t index,
                    std::uint32_t x,
                    std::uint32_t y,
                    std::uint32_t width,
                    std::uint32_t height)
{
    roi.roi_index = index;
    roi.roi_x = x;
    roi.roi_y = y;
    roi.roi_w = width;
    roi.roi_h = height;
}

void set_source_copy(blending_attr &blending,
                     std::uint32_t roi_index,
                     std::uint32_t source_index)
{
    blending.roi_index = roi_index;
    blending.blending_mode = kBlendingModeSourceCopy;
    blending.direct = kBlendingDirectionLeftTop;
    blending.uv_en = 1U;
    blending.src0_index = source_index;
    blending.src1_index = source_index;
    blending.margin = 0U;
    blending.margin_inv = 0U;
    for (std::size_t component = 0; component < 3U; ++component) {
        blending.gain_src0_yuv[component] = 256U;
        blending.gain_src1_yuv[component] = 256U;
    }
}

}  // namespace

struct horddt_pip::impl {
    config cfg{};
    hbn_vnode_handle_t vnode{};
    bool mem_opened = false;
    bool vnode_opened = false;
    bool vnode_started = false;
    bool initialized = false;
    int initialization_result = -1;
    std::mutex process_mutex;

    ~impl()
    {
        release();
    }

    int initialize(const config &requested_config)
    {
        cfg = requested_config;
        int ret = validate_config(cfg);
        if (ret != 0) {
            initialization_result = ret;
            return ret;
        }

        ret = hb_mem_module_open();
        if (ret != 0) {
            std::fprintf(stderr, "hb_mem_module_open failed: %d\n", ret);
            initialization_result = ret;
            return ret;
        }
        mem_opened = true;

        ret = hbn_vnode_open(HB_STITCH,
                             kStitchHardwareId,
                             kAutoAllocId,
                             &vnode);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_open(HB_STITCH) failed: %d\n", ret);
            initialization_result = ret;
            release();
            return ret;
        }
        vnode_opened = true;

        std::array<pip_roi, kMaximumPipRois> roi_plan{};
        std::size_t roi_count = 0U;
        ret = build_non_overlapping_rois(cfg, roi_plan, roi_count);
        if (ret != 0) {
            initialization_result = ret;
            release();
            return ret;
        }

        stitch_base_attr base_attr{};
        base_attr.mode = 0U;  // External-buffer feedback mode.
        base_attr.roi_nums = static_cast<std::uint32_t>(roi_count);
        base_attr.img_nums = kImageCount;
        for (std::size_t index = 0; index < roi_count; ++index) {
            set_source_copy(base_attr.blending[index],
                            static_cast<std::uint32_t>(index),
                            roi_plan[index].source_index);
        }

        ret = hbn_vnode_set_attr(vnode, &base_attr);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_attr(STITCH) failed: %d\n", ret);
            initialization_result = ret;
            release();
            return ret;
        }

        stitch_ch_attr background_attr{};
        background_attr.width = cfg.background_width;
        background_attr.height = cfg.background_height;
        background_attr.strid[0] = cfg.background_stride;
        background_attr.strid[1] = cfg.background_stride;

        stitch_ch_attr overlay_attr{};
        overlay_attr.width = cfg.overlay_width;
        overlay_attr.height = cfg.overlay_height;
        overlay_attr.strid[0] = cfg.overlay_stride;
        overlay_attr.strid[1] = cfg.overlay_stride;
        for (std::size_t index = 0; index < roi_count; ++index) {
            const pip_roi &roi = roi_plan[index];
            stitch_ch_attr &source_attr =
                roi.source_index == kBackgroundChannel
                    ? background_attr
                    : overlay_attr;
            set_input_roi(source_attr.rois[index],
                          static_cast<std::uint32_t>(index),
                          roi.source_x, roi.source_y);
        }

        ret = hbn_vnode_set_ichn_attr(vnode,
                                      kBackgroundChannel,
                                      &background_attr);
        if (ret == 0) {
            ret = hbn_vnode_set_ichn_attr(vnode,
                                          kOverlayChannel,
                                          &overlay_attr);
        }
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_ichn_attr(STITCH) failed: %d\n",
                         ret);
            initialization_result = ret;
            release();
            return ret;
        }

        stitch_ch_attr output_attr{};
        output_attr.width = cfg.background_width;
        output_attr.height = cfg.background_height;
        output_attr.strid[0] = cfg.output_stride;
        output_attr.strid[1] = cfg.output_stride;
        for (std::size_t index = 0; index < roi_count; ++index) {
            const pip_roi &roi = roi_plan[index];
            set_output_roi(output_attr.rois[index],
                           static_cast<std::uint32_t>(index),
                           roi.x, roi.y, roi.width, roi.height);
        }

        ret = hbn_vnode_set_ochn_attr(vnode, kOutputChannel, &output_attr);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_ochn_attr(STITCH) failed: %d\n",
                         ret);
            initialization_result = ret;
            release();
            return ret;
        }

        hbn_buf_alloc_attr_t allocation_attr{};
        allocation_attr.buffers_num = cfg.output_buffer_count;
        allocation_attr.is_contig = 1;
        allocation_attr.flags =
            static_cast<std::int64_t>(HB_MEM_USAGE_CPU_READ_OFTEN) |
            static_cast<std::int64_t>(HB_MEM_USAGE_CPU_WRITE_OFTEN) |
            static_cast<std::int64_t>(HB_MEM_USAGE_MAP_INITIALIZED) |
            static_cast<std::int64_t>(HB_MEM_USAGE_CACHED);
        ret = hbn_vnode_set_ochn_buf_attr(vnode,
                                          kOutputChannel,
                                          &allocation_attr);
        if (ret != 0) {
            std::fprintf(
                stderr,
                "hbn_vnode_set_ochn_buf_attr(STITCH) failed: %d\n",
                ret);
            initialization_result = ret;
            release();
            return ret;
        }

        ret = hbn_vnode_start(vnode);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_start(STITCH) failed: %d\n",
                         ret);
            initialization_result = ret;
            release();
            return ret;
        }
        vnode_started = true;
        initialized = true;
        initialization_result = 0;

        if (cfg.verbose) {
            std::printf(
                "STITCH PIP ready: background=%ux%u, overlay=%ux%u at "
                "(%u,%u), output=%ux%u\n",
                cfg.background_width, cfg.background_height,
                cfg.overlay_width, cfg.overlay_height,
                cfg.overlay_x, cfg.overlay_y,
                cfg.background_width, cfg.background_height);
        }
        return 0;
    }

    int run(const hb_mem_graphic_buf_t &background,
            const hb_mem_graphic_buf_t &overlay,
            hb_mem_graphic_buf_t *destination,
            const output_callback *callback)
    {
        std::lock_guard<std::mutex> lock(process_mutex);
        if (!initialized) {
            std::fprintf(stderr, "horddt_pip is not initialized.\n");
            return initialization_result != 0 ? initialization_result : -1;
        }

        int ret = validate_nv12_buffer(background,
                                       cfg.background_width,
                                       cfg.background_height,
                                       cfg.background_stride,
                                       "PIP background");
        if (ret == 0) {
            ret = validate_nv12_buffer(overlay,
                                       cfg.overlay_width,
                                       cfg.overlay_height,
                                       cfg.overlay_stride,
                                       "PIP overlay");
        }
        if (ret == 0 && destination != nullptr) {
            ret = validate_nv12_buffer(*destination,
                                       cfg.background_width,
                                       cfg.background_height,
                                       cfg.output_stride,
                                       "PIP destination");
        }
        if (ret != 0) {
            return ret;
        }
        if (destination == nullptr &&
            (callback == nullptr || !(*callback))) {
            std::fprintf(stderr, "PIP output callback must not be empty.\n");
            return -1;
        }

        if (cfg.sync_inputs_for_device) {
            ret = sync_nv12_buffer(background, true, "PIP background");
            if (ret == 0) {
                ret = sync_nv12_buffer(overlay, true, "PIP overlay");
            }
            if (ret != 0) {
                return ret;
            }
        }

        timeval timestamp{};
        gettimeofday(&timestamp, nullptr);
        hbn_vnode_image_t background_frame{};
        background_frame.buffer = background;
        background_frame.info.tv = timestamp;
        hbn_vnode_image_t overlay_frame{};
        overlay_frame.buffer = overlay;
        overlay_frame.info.tv = timestamp;

        // Follow the official sample_gdc_stitch submission sequence: queue
        // channels N-1..1 asynchronously, then submit channel 0 synchronously
        // to trigger processing of the complete image set. Calling the
        // synchronous channel first may block before the overlay is queued.
        ret = hbn_vnode_sendframe_async(vnode,
                                        kOverlayChannel,
                                        &overlay_frame);
        if (ret != 0) {
            std::fprintf(
                stderr,
                "hbn_vnode_sendframe_async(STITCH overlay) failed: %d\n",
                ret);
            return ret;
        }
        ret = hbn_vnode_sendframe(vnode,
                                  kBackgroundChannel,
                                  &background_frame);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_sendframe(STITCH background) failed: "
                         "%d\n",
                         ret);
            return ret;
        }

        hbn_vnode_image_t output_frame{};
        ret = hbn_vnode_getframe(vnode,
                                 kOutputChannel,
                                 static_cast<std::uint32_t>(cfg.timeout_ms),
                                 &output_frame);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_getframe(STITCH) failed: %d\n", ret);
            return ret;
        }

        hb_mem_graphic_buf_t &stitch_output = output_frame.buffer;
        ret = validate_nv12_buffer(stitch_output,
                                   cfg.background_width,
                                   cfg.background_height,
                                   cfg.output_stride,
                                   "STITCH output");
        if (ret == 0 && destination != nullptr) {
            ret = sync_nv12_buffer(stitch_output,
                                   false,
                                   "STITCH output");
            if (ret == 0) {
                ret = copy_nv12_buffer(stitch_output,
                                       *destination,
                                       cfg.background_width,
                                       cfg.background_height);
            }
        } else if (ret == 0) {
            if (cfg.sync_borrowed_output_for_cpu) {
                ret = sync_nv12_buffer(stitch_output,
                                       false,
                                       "STITCH output");
            }
            if (ret == 0) {
                try {
                    ret = (*callback)(stitch_output);
                } catch (...) {
                    std::fprintf(stderr,
                                 "PIP output callback threw an exception.\n");
                    ret = -1;
                }
            }
        }

        const int release_ret = hbn_vnode_releaseframe(
            vnode, kOutputChannel, &output_frame);
        if (release_ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_releaseframe(STITCH) failed: %d\n",
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
                             "hbn_vnode_stop(STITCH) failed during cleanup: "
                             "%d\n",
                             ret);
            }
            vnode_started = false;
        }
        if (vnode_opened) {
            hbn_vnode_close(vnode);
            vnode_opened = false;
        }
        if (mem_opened) {
            hb_mem_module_close();
            mem_opened = false;
        }
    }
};

horddt_pip::horddt_pip(const config &cfg)
    : impl_(new impl())
{
    impl_->initialize(cfg);
}

horddt_pip::~horddt_pip()
{
    close();
}

int horddt_pip::compose(const hb_mem_graphic_buf_t &background,
                        const hb_mem_graphic_buf_t &overlay,
                        hb_mem_graphic_buf_t &output)
{
    return impl_ ? impl_->run(background, overlay, &output, nullptr) : -1;
}

int horddt_pip::compose_borrowed(
    const hb_mem_graphic_buf_t &background,
    const hb_mem_graphic_buf_t &overlay,
    const output_callback &callback)
{
    return impl_ ? impl_->run(background, overlay, nullptr, &callback) : -1;
}

void horddt_pip::close()
{
    if (impl_) {
        impl_->release();
    }
}

bool horddt_pip::is_initialized() const noexcept
{
    return impl_ && impl_->initialized;
}

int horddt_pip::initialization_status() const noexcept
{
    return impl_ ? impl_->initialization_result : -1;
}
