/***************************************************************************
 * NV12 hardware crop based on multimedia_samples/sample_pym.
 *
 * PYM exposes multiple DS ROI output channels.  All enabled channels below
 * select the original SRC layer, but use a different ROI rectangle.  A frame
 * is therefore submitted once and all crops are returned in one output group.
 ***************************************************************************/

#include "horddt_crop.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/time.h>

extern "C" {
#include "hbn_pym_cfg.h"
#include "hbn_vpf_data_info.h"
#include "hbn_vpf_interface.h"
}

namespace {

constexpr std::uint32_t kPymMinWidth = 32U;
constexpr std::uint32_t kPymMinHeight = 32U;
constexpr std::uint32_t kPymMaxWidth = 4096U;
constexpr std::uint32_t kPymMaxHeight = 4096U;
constexpr int kAutoAllocId = -1;
constexpr std::uint32_t kPymMagicNumber = 0x12345678U;
constexpr int kPymBlMaxLayerEnable = 5;
constexpr int kPymPixelNumBeforeSol = 2;
constexpr int kPymSuffixHb = 100;
constexpr int kPymPrefixHb = 2;
constexpr int kPymSuffixVb = 10;
constexpr int kPymPrefixVb = 0;

std::uint32_t align16(std::uint32_t value)
{
    return (value + 15U) & ~15U;
}

int read_plane(FILE *file, std::uint8_t *destination, std::uint32_t rows,
               std::uint32_t row_bytes, std::uint32_t stride)
{
    for (std::uint32_t row = 0; row < rows; ++row) {
        if (std::fread(destination + static_cast<std::size_t>(row) * stride,
                       1, row_bytes, file) != row_bytes) {
            return -1;
        }
    }
    return 0;
}

int write_nv12_image(const std::string &path,
                     const hb_mem_graphic_buf_t &image)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot open output file '%s': %s\n",
                     path.c_str(), std::strerror(errno));
        return -1;
    }

    const std::size_t width = static_cast<std::size_t>(image.width);
    const std::size_t stride = static_cast<std::size_t>(image.stride);
    bool failed = false;
    for (std::int32_t row = 0; row < image.height && !failed; ++row) {
        const auto *address =
            static_cast<const std::uint8_t *>(image.virt_addr[0]) +
            static_cast<std::size_t>(row) * stride;
        failed = std::fwrite(address, 1, width, file) != width;
    }
    for (std::int32_t row = 0; row < image.height / 2 && !failed; ++row) {
        const auto *address =
            static_cast<const std::uint8_t *>(image.virt_addr[1]) +
            static_cast<std::size_t>(row) * stride;
        failed = std::fwrite(address, 1, width, file) != width;
    }

    const int close_result = std::fclose(file);
    if (failed || close_result != 0) {
        std::fprintf(stderr, "Failed to write NV12 file '%s': %s\n",
                     path.c_str(), std::strerror(errno));
        return -1;
    }
    return 0;
}

}  // namespace

struct horddt_crop::impl {
    config cfg{};
    int initialization_result = -1;

    hbn_vnode_handle_t vnode{};
    hbn_vflow_handle_t vflow{};
    hbn_vnode_image_t input_image{};

    bool initialized = false;
    bool mem_opened = false;
    bool input_allocated = false;
    bool vnode_opened = false;
    bool vflow_created = false;
    bool vflow_started = false;

    int validate_config() const
    {
        if (cfg.input_width == 0U || cfg.input_height == 0U ||
            cfg.input_width > kPymMaxWidth ||
            cfg.input_height > kPymMaxHeight) {
            std::fprintf(stderr, "PYM input size must be in 1x1..%ux%u.\n",
                         kPymMaxWidth, kPymMaxHeight);
            return -1;
        }
        if ((cfg.input_width & 1U) != 0U ||
            (cfg.input_height & 1U) != 0U) {
            std::fprintf(stderr, "NV12 input dimensions must be even.\n");
            return -1;
        }
        if (cfg.regions.empty() ||
            cfg.regions.size() > static_cast<std::size_t>(MAX_DS_NUM)) {
            std::fprintf(stderr, "Crop region count must be in [1, %d].\n",
                         static_cast<int>(MAX_DS_NUM));
            return -1;
        }

        for (std::size_t index = 0; index < cfg.regions.size(); ++index) {
            const rect &region = cfg.regions[index];
            const bool odd = ((region.x | region.y | region.width |
                               region.height) & 1U) != 0U;
            const bool too_small = region.width < kPymMinWidth ||
                                   region.height < kPymMinHeight;
            const bool outside = region.x >= cfg.input_width ||
                                 region.y >= cfg.input_height ||
                                 region.width > cfg.input_width - region.x ||
                                 region.height > cfg.input_height - region.y;
            if (odd || too_small || outside) {
                std::fprintf(
                    stderr,
                    "Invalid NV12 crop[%zu]: x=%u y=%u width=%u height=%u. "
                    "Coordinates/sizes must be even, output at least %ux%u, "
                    "and the rectangle must stay inside %ux%u.\n",
                    index, region.x, region.y, region.width, region.height,
                    kPymMinWidth, kPymMinHeight,
                    cfg.input_width, cfg.input_height);
                return -1;
            }
        }

        if (cfg.timeout_ms <= 0 || cfg.output_buffer_count == 0U ||
            cfg.feedback_buffer_count == 0U ||
            cfg.output_buffer_count >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            cfg.feedback_buffer_count >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            std::fprintf(stderr, "Invalid PYM timeout or buffer count.\n");
            return -1;
        }
        return 0;
    }

    int validate_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                             std::uint32_t width,
                             std::uint32_t height,
                             const char *name) const
    {
        if (buffer.format != MEM_PIX_FMT_NV12 ||
            buffer.width != static_cast<std::int32_t>(width) ||
            buffer.height != static_cast<std::int32_t>(height) ||
            buffer.plane_cnt < 2 || buffer.stride < buffer.width ||
            buffer.vstride < buffer.height ||
            buffer.virt_addr[0] == nullptr ||
            buffer.virt_addr[1] == nullptr) {
            std::fprintf(stderr,
                         "Invalid %s: format=%d size=%dx%d stride=%d "
                         "vstride=%d planes=%d; expected NV12 %ux%u.\n",
                         name, buffer.format, buffer.width, buffer.height,
                         buffer.stride, buffer.vstride, buffer.plane_cnt,
                         width, height);
            return -1;
        }
        return 0;
    }

    int sync_buffer(const hb_mem_graphic_buf_t &buffer,
                    bool flush,
                    const char *name) const
    {
        const std::uint64_t y_bytes =
            static_cast<std::uint64_t>(buffer.stride) * buffer.height;
        const std::uint64_t uv_bytes = y_bytes / 2U;
        const auto y_address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(buffer.virt_addr[0]));
        const auto uv_address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(buffer.virt_addr[1]));

        int ret = flush
                      ? hb_mem_flush_buf_with_vaddr(y_address, y_bytes)
                      : hb_mem_invalidate_buf_with_vaddr(y_address, y_bytes);
        if (ret == 0) {
            ret = flush
                      ? hb_mem_flush_buf_with_vaddr(uv_address, uv_bytes)
                      : hb_mem_invalidate_buf_with_vaddr(uv_address, uv_bytes);
        }
        if (ret != 0) {
            std::fprintf(stderr, "Failed to %s %s cache: %d\n",
                         flush ? "flush" : "invalidate", name, ret);
        }
        return ret;
    }

    int allocate_input_buffer()
    {
        constexpr std::int64_t flags =
            HB_MEM_USAGE_MAP_INITIALIZED |
            HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
            HB_MEM_USAGE_CPU_READ_OFTEN |
            HB_MEM_USAGE_CPU_WRITE_OFTEN |
            HB_MEM_USAGE_CACHED |
            HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
        const int ret = hb_mem_alloc_graph_buf(
            cfg.input_width, cfg.input_height, MEM_PIX_FMT_NV12, flags,
            align16(cfg.input_width), cfg.input_height, &input_image.buffer);
        if (ret != 0) {
            std::fprintf(stderr, "hb_mem_alloc_graph_buf failed: %d\n", ret);
            return ret;
        }
        input_allocated = true;
        return validate_nv12_buffer(input_image.buffer, cfg.input_width,
                                    cfg.input_height, "file input buffer");
    }

    int start_pym()
    {
        pym_cfg_t pym_cfg{};
        pym_cfg.hw_id = 0;
        pym_cfg.pym_mode = PYM_M2M_MODE;
        pym_cfg.output_buf_num = static_cast<int>(cfg.output_buffer_count);
        pym_cfg.fb_buf_num = static_cast<int>(cfg.feedback_buffer_count);
        pym_cfg.layer_num_trans_next = 0;
        pym_cfg.layer_num_share_prev = -1;
        pym_cfg.out_buf_noinvalid = 1;
        pym_cfg.out_buf_noncached = 0;
        pym_cfg.in_buf_noclean = 1;
        pym_cfg.in_buf_noncached = 0;
        pym_cfg.magicNumber = kPymMagicNumber;

        pym_cfg.chn_ctrl.pixel_num_before_sol = kPymPixelNumBeforeSol;
        pym_cfg.chn_ctrl.suffix_hb_val = kPymSuffixHb;
        pym_cfg.chn_ctrl.prefix_hb_val = kPymPrefixHb;
        pym_cfg.chn_ctrl.suffix_vb_val = kPymSuffixVb;
        pym_cfg.chn_ctrl.prefix_vb_val = kPymPrefixVb;
        pym_cfg.chn_ctrl.bl_max_layer_en = kPymBlMaxLayerEnable;
        pym_cfg.chn_ctrl.src_in_width = cfg.input_width;
        pym_cfg.chn_ctrl.src_in_height = cfg.input_height;
        pym_cfg.chn_ctrl.src_in_stride_y = align16(cfg.input_width);
        pym_cfg.chn_ctrl.src_in_stride_uv = align16(cfg.input_width);
        pym_cfg.chn_ctrl.ds_roi_en = 0;

        for (std::size_t index = 0; index < cfg.regions.size(); ++index) {
            const rect &region = cfg.regions[index];
            const int channel = static_cast<int>(index);

            // Unlike a normal image pyramid, every output selects SRC. This is
            // what lets multiple independent crops be generated in one pass.
            pym_cfg.chn_ctrl.ds_roi_sel[channel] = PYM_SRC_SEL;
            pym_cfg.chn_ctrl.ds_roi_layer[channel] = 0;
            pym_cfg.chn_ctrl.ds_roi_en |= static_cast<std::uint8_t>(
                1U << static_cast<std::uint32_t>(channel));

            roi_box_t &roi = pym_cfg.chn_ctrl.ds_roi_info[channel];
            roi.start_left = region.x;
            roi.start_top = region.y;
            roi.region_width = region.width;
            roi.region_height = region.height;
            roi.out_width = region.width;
            roi.out_height = region.height;
            roi.wstride_y = align16(region.width);
            roi.wstride_uv = align16(region.width);
            roi.vstride = region.height;

            if (cfg.verbose) {
                std::printf("  crop[%zu] DS[%d]: (%u,%u) %ux%u, stride=%u\n",
                            index, channel, region.x, region.y,
                            region.width, region.height, roi.wstride_y);
            }
        }

        int ret = hbn_vnode_open(HB_PYM, pym_cfg.hw_id, kAutoAllocId, &vnode);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_open(HB_PYM) failed: %d\n", ret);
            return ret;
        }
        vnode_opened = true;

        ret = hbn_vnode_set_attr(vnode, &pym_cfg);
        if (ret == 0) {
            ret = hbn_vnode_set_ichn_attr(vnode, 0, &pym_cfg);
        }
        if (ret == 0) {
            ret = hbn_vnode_set_ochn_attr(vnode, 0, &pym_cfg);
        }
        if (ret != 0) {
            std::fprintf(stderr, "Failed to configure PYM crop vnode: %d\n",
                         ret);
            return ret;
        }

        hbn_buf_alloc_attr_t alloc_attr{};
        alloc_attr.buffers_num = pym_cfg.output_buf_num;
        alloc_attr.is_contig = 1;
        alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
                           HB_MEM_USAGE_CPU_WRITE_OFTEN |
                           HB_MEM_USAGE_CACHED;
        ret = hbn_vnode_set_ochn_buf_attr(vnode, 0, &alloc_attr);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_ochn_buf_attr(PYM) failed: %d\n",
                         ret);
            return ret;
        }

        ret = hbn_vflow_create(&vflow);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vflow_create failed: %d\n", ret);
            return ret;
        }
        vflow_created = true;
        ret = hbn_vflow_add_vnode(vflow, vnode);
        if (ret == 0) {
            ret = hbn_vflow_start(vflow);
        }
        if (ret != 0) {
            std::fprintf(stderr, "Failed to start PYM crop vflow: %d\n", ret);
            return ret;
        }
        vflow_started = true;
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
        if (ret == 0 && cfg.enable_file_io) {
            ret = allocate_input_buffer();
        }
        if (ret == 0) {
            ret = start_pym();
        }
        if (ret != 0) {
            release();
            initialization_result = ret;
            return ret;
        }
        initialized = true;
        initialization_result = 0;
        if (cfg.verbose) {
            std::printf("horddt_crop initialized: input=%ux%u, regions=%zu\n",
                        cfg.input_width, cfg.input_height,
                        cfg.regions.size());
        }
        return 0;
    }

    int read_input_file(const std::string &path)
    {
        FILE *file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            std::fprintf(stderr, "Cannot open input file '%s': %s\n",
                         path.c_str(), std::strerror(errno));
            return -1;
        }
        const auto stride =
            static_cast<std::uint32_t>(input_image.buffer.stride);
        const bool failed =
            read_plane(file,
                       static_cast<std::uint8_t *>(
                           input_image.buffer.virt_addr[0]),
                       cfg.input_height, cfg.input_width, stride) != 0 ||
            read_plane(file,
                       static_cast<std::uint8_t *>(
                           input_image.buffer.virt_addr[1]),
                       cfg.input_height / 2U, cfg.input_width, stride) != 0;
        const int close_result = std::fclose(file);
        if (failed || close_result != 0) {
            std::fprintf(stderr,
                         "Failed to read one %ux%u NV12 frame from '%s'.\n",
                         cfg.input_width, cfg.input_height, path.c_str());
            return -1;
        }
        return 0;
    }

    int copy_output(const hb_mem_graphic_buf_t &source,
                    hb_mem_graphic_buf_t &destination,
                    const rect &region) const
    {
        const std::size_t row_bytes = region.width;
        const std::size_t source_stride = source.stride;
        const std::size_t destination_stride = destination.stride;
        const auto *source_y =
            static_cast<const std::uint8_t *>(source.virt_addr[0]);
        const auto *source_uv =
            static_cast<const std::uint8_t *>(source.virt_addr[1]);
        auto *destination_y =
            static_cast<std::uint8_t *>(destination.virt_addr[0]);
        auto *destination_uv =
            static_cast<std::uint8_t *>(destination.virt_addr[1]);

        for (std::uint32_t row = 0; row < region.height; ++row) {
            std::memcpy(destination_y + row * destination_stride,
                        source_y + row * source_stride, row_bytes);
        }
        for (std::uint32_t row = 0; row < region.height / 2U; ++row) {
            std::memcpy(destination_uv + row * destination_stride,
                        source_uv + row * source_stride, row_bytes);
        }
        return sync_buffer(destination, true, "crop destination");
    }

    int run_pym(const hb_mem_graphic_buf_t &input_buffer,
                std::vector<hb_mem_graphic_buf_t> *destinations,
                const output_callback *callback,
                const std::vector<std::string> *paths)
    {
        if (!initialized) {
            std::fprintf(stderr, "horddt_crop is not initialized.\n");
            return initialization_result != 0 ? initialization_result : -1;
        }
        int ret = validate_nv12_buffer(input_buffer, cfg.input_width,
                                       cfg.input_height, "crop input buffer");
        if (ret != 0) {
            return ret;
        }
        if (cfg.sync_input_for_device) {
            ret = sync_buffer(input_buffer, true, "crop input buffer");
            if (ret != 0) {
                return ret;
            }
        }

        hbn_vnode_image_t frame{};
        frame.buffer = input_buffer;
        gettimeofday(&frame.info.tv, nullptr);
        ret = hbn_vnode_sendframe(vnode, 0, &frame);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_sendframe(PYM) failed: %d\n", ret);
            return ret;
        }

        hbn_vnode_image_group_t output_group{};
        ret = hbn_vnode_getframe_group(vnode, 0, cfg.timeout_ms,
                                       &output_group);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_getframe_group(PYM) failed: %d\n", ret);
            return ret;
        }

        std::vector<hb_mem_graphic_buf_t> borrowed_outputs;
        borrowed_outputs.reserve(cfg.regions.size());
        for (std::size_t index = 0;
             index < cfg.regions.size() && ret == 0; ++index) {
            hb_mem_graphic_buf_t &output =
                output_group.buf_group.graph_group[index];
            const rect &region = cfg.regions[index];
            ret = validate_nv12_buffer(output, region.width, region.height,
                                       "PYM crop output");
            const bool cpu_reads_output =
                destinations != nullptr || paths != nullptr ||
                (callback != nullptr && cfg.sync_borrowed_output_for_cpu);
            if (ret == 0 && cpu_reads_output) {
                ret = sync_buffer(output, false, "PYM crop output");
            }
            if (ret == 0) {
                borrowed_outputs.push_back(output);
            }
        }

        if (ret == 0 && destinations != nullptr) {
            for (std::size_t index = 0;
                 index < cfg.regions.size() && ret == 0; ++index) {
                ret = copy_output(borrowed_outputs[index],
                                  (*destinations)[index], cfg.regions[index]);
            }
        } else if (ret == 0 && callback != nullptr) {
            try {
                ret = (*callback)(borrowed_outputs);
            } catch (...) {
                std::fprintf(stderr,
                             "PYM crop output callback threw an exception.\n");
                ret = -1;
            }
        } else if (ret == 0 && paths != nullptr) {
            for (std::size_t index = 0;
                 index < cfg.regions.size() && ret == 0; ++index) {
                ret = write_nv12_image((*paths)[index],
                                       borrowed_outputs[index]);
            }
        }

        const int release_ret =
            hbn_vnode_releaseframe_group(vnode, 0, &output_group);
        if (release_ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_releaseframe_group(PYM) failed: %d\n",
                         release_ret);
            if (ret == 0) {
                ret = release_ret;
            }
        }
        return ret;
    }

    int process_file(const std::string &input_path,
                     const std::vector<std::string> &output_paths)
    {
        if (!initialized) {
            std::fprintf(stderr, "horddt_crop is not initialized.\n");
            return initialization_result != 0 ? initialization_result : -1;
        }
        if (!cfg.enable_file_io || !input_allocated) {
            std::fprintf(stderr, "File crop is disabled.\n");
            return -1;
        }
        if (output_paths.size() != cfg.regions.size()) {
            std::fprintf(stderr,
                         "Output path count must equal crop region count.\n");
            return -1;
        }
        int ret = read_input_file(input_path);
        if (ret == 0) {
            ret = run_pym(input_image.buffer, nullptr, nullptr, &output_paths);
        }
        return ret;
    }

    int process_buffers(const hb_mem_graphic_buf_t &input_buffer,
                        std::vector<hb_mem_graphic_buf_t> &outputs)
    {
        if (outputs.size() != cfg.regions.size()) {
            std::fprintf(stderr,
                         "Output buffer count must equal crop region count.\n");
            return -1;
        }
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            const rect &region = cfg.regions[index];
            const int ret = validate_nv12_buffer(outputs[index], region.width,
                                                 region.height,
                                                 "crop destination buffer");
            if (ret != 0) {
                return ret;
            }
        }
        return run_pym(input_buffer, &outputs, nullptr, nullptr);
    }

    int process_borrowed(const hb_mem_graphic_buf_t &input_buffer,
                         const output_callback &callback)
    {
        if (!callback) {
            std::fprintf(stderr, "Crop callback must not be empty.\n");
            return -1;
        }
        return run_pym(input_buffer, nullptr, &callback, nullptr);
    }

    void release()
    {
        initialized = false;
        if (vflow_started) {
            const int ret = hbn_vflow_stop(vflow);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hbn_vflow_stop failed during cleanup: %d\n",
                             ret);
            }
            vflow_started = false;
        }
        if (vflow_created) {
            hbn_vflow_destroy(vflow);
            vflow_created = false;
        }
        if (vnode_opened) {
            hbn_vnode_close(vnode);
            vnode_opened = false;
        }
        if (input_allocated) {
            hb_mem_free_buf(input_image.buffer.fd[0]);
            input_allocated = false;
        }
        if (mem_opened) {
            hb_mem_module_close();
            mem_opened = false;
        }
    }
};

horddt_crop::horddt_crop(const config &cfg) : impl_(new impl())
{
    impl_->initialize(cfg);
}

horddt_crop::~horddt_crop()
{
    close();
}

int horddt_crop::crop(const std::string &input_path,
                      const std::vector<std::string> &output_paths)
{
    return impl_ ? impl_->process_file(input_path, output_paths) : -1;
}

int horddt_crop::crop(const hb_mem_graphic_buf_t &input_buffer,
                      std::vector<hb_mem_graphic_buf_t> &outputs)
{
    return impl_ ? impl_->process_buffers(input_buffer, outputs) : -1;
}

int horddt_crop::crop_borrowed(const hb_mem_graphic_buf_t &input_buffer,
                               const output_callback &callback)
{
    return impl_ ? impl_->process_borrowed(input_buffer, callback) : -1;
}

void horddt_crop::close()
{
    if (impl_) {
        impl_->release();
    }
}

bool horddt_crop::is_initialized() const noexcept
{
    return impl_ && impl_->initialized;
}

int horddt_crop::initialization_status() const noexcept
{
    return impl_ ? impl_->initialization_result : -1;
}
