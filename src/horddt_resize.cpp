/***************************************************************************
 * NV12 image resize class based on multimedia_samples/sample_pym.
 ***************************************************************************/

#include "horddt_resize.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/time.h>

extern "C" {
#include "hb_mem_mgr.h"
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

std::uint32_t floor_align2(std::uint32_t value)
{
    return value & ~1U;
}

int select_pym_channel(std::uint32_t input_width,
                       std::uint32_t input_height,
                       std::uint32_t output_width,
                       std::uint32_t output_height)
{
    for (int channel = 0; channel < static_cast<int>(MAX_DS_NUM); ++channel) {
        const std::uint32_t ratio = 1U << static_cast<std::uint32_t>(channel);
        const std::uint32_t layer_width =
            floor_align2(input_width / ratio);
        const std::uint32_t layer_height =
            floor_align2(input_height / ratio);

        if (layer_width < kPymMinWidth || layer_height < kPymMinHeight) {
            break;
        }

        if (output_width <= layer_width && output_height <= layer_height &&
            static_cast<std::uint64_t>(output_width) * 2U >= layer_width &&
            static_cast<std::uint64_t>(output_height) * 2U >= layer_height) {
            return channel;
        }
    }

    return -1;
}

int read_plane(FILE *file, std::uint8_t *destination, std::uint32_t rows,
               std::uint32_t row_bytes, std::uint32_t stride)
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

int write_nv12_image(const std::string &path,
                     const hb_mem_graphic_buf_t &image)
{
    const std::int32_t width = image.width;
    const std::int32_t height = image.height;
    const std::int32_t stride = image.stride;

    if (image.plane_cnt < 2 || image.virt_addr[0] == nullptr ||
        image.virt_addr[1] == nullptr || width <= 0 || height <= 0 ||
        stride < width) {
        std::fprintf(stderr, "PYM returned an invalid NV12 buffer.\n");
        return -1;
    }

    FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "Cannot open output file '%s': %s\n",
                     path.c_str(), std::strerror(errno));
        return -1;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(width);
    for (std::int32_t row = 0; row < height; ++row) {
        const auto *row_address =
            static_cast<const std::uint8_t *>(image.virt_addr[0]) +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
        if (std::fwrite(row_address, 1, row_bytes, file) != row_bytes) {
            std::fprintf(stderr, "Failed to write output file '%s': %s\n",
                         path.c_str(), std::strerror(errno));
            std::fclose(file);
            return -1;
        }
    }

    for (std::int32_t row = 0; row < height / 2; ++row) {
        const auto *row_address =
            static_cast<const std::uint8_t *>(image.virt_addr[1]) +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
        if (std::fwrite(row_address, 1, row_bytes, file) != row_bytes) {
            std::fprintf(stderr, "Failed to write output file '%s': %s\n",
                         path.c_str(), std::strerror(errno));
            std::fclose(file);
            return -1;
        }
    }

    if (std::fclose(file) != 0) {
        std::fprintf(stderr, "Failed to close output file '%s': %s\n",
                     path.c_str(), std::strerror(errno));
        return -1;
    }
    return 0;
}

}  // namespace

struct horddt_resize::impl {
    config cfg{};
    int pym_channel = -1;
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

    int validate_config()
    {
        if (cfg.input_width == 0 || cfg.input_height == 0 ||
            cfg.output_width == 0 || cfg.output_height == 0) {
            std::fprintf(stderr, "Image width and height must not be zero.\n");
            return -1;
        }

        if ((cfg.input_width & 1U) != 0 || (cfg.input_height & 1U) != 0 ||
            (cfg.output_width & 1U) != 0 ||
            (cfg.output_height & 1U) != 0) {
            std::fprintf(stderr,
                         "NV12 input and output dimensions must be even.\n");
            return -1;
        }

        if (cfg.input_width > kPymMaxWidth ||
            cfg.input_height > kPymMaxHeight) {
            std::fprintf(stderr, "PYM input must not exceed %ux%u.\n",
                         kPymMaxWidth, kPymMaxHeight);
            return -1;
        }

        if (cfg.output_width < kPymMinWidth ||
            cfg.output_height < kPymMinHeight) {
            std::fprintf(stderr, "PYM output must be at least %ux%u.\n",
                         kPymMinWidth, kPymMinHeight);
            return -1;
        }

        if (cfg.output_width > cfg.input_width ||
            cfg.output_height > cfg.input_height) {
            std::fprintf(stderr,
                         "PYM does not support upscaling (%ux%u -> %ux%u).\n",
                         cfg.input_width, cfg.input_height,
                         cfg.output_width, cfg.output_height);
            return -1;
        }

        pym_channel = select_pym_channel(
            cfg.input_width, cfg.input_height,
            cfg.output_width, cfg.output_height);
        if (pym_channel < 0) {
            std::fprintf(
                stderr,
                "Unsupported PYM scale ratio: %ux%u -> %ux%u.\n"
                "Each output dimension must be between 1/2 and 1 of the "
                "same SRC/BL layer.\n",
                cfg.input_width, cfg.input_height,
                cfg.output_width, cfg.output_height);
            return -1;
        }

        if (cfg.timeout_ms <= 0) {
            std::fprintf(stderr, "Timeout must be greater than zero.\n");
            return -1;
        }
        return 0;
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

        std::memset(&input_image, 0, sizeof(input_image));
        const std::uint32_t stride = align16(cfg.input_width);
        const int ret = hb_mem_alloc_graph_buf(
            cfg.input_width, cfg.input_height, MEM_PIX_FMT_NV12, flags,
            stride, cfg.input_height, &input_image.buffer);
        if (ret != 0) {
            std::fprintf(stderr, "hb_mem_alloc_graph_buf failed: %d\n", ret);
            return ret;
        }
        input_allocated = true;

        if (input_image.buffer.stride <= 0 ||
            input_image.buffer.virt_addr[0] == nullptr ||
            input_image.buffer.virt_addr[1] == nullptr) {
            std::fprintf(stderr, "hbmem returned an invalid input buffer.\n");
            return -1;
        }
        return 0;
    }

    int start_pym()
    {
        pym_cfg_t pym_cfg{};
        hbn_buf_alloc_attr_t alloc_attr{};

        pym_cfg.hw_id = 0;
        pym_cfg.pym_mode = PYM_M2M_MODE;
        pym_cfg.output_buf_num = 3;
        pym_cfg.fb_buf_num = 2;
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

        for (int channel = 0; channel < static_cast<int>(MAX_DS_NUM);
             ++channel) {
            const std::uint32_t ratio =
                1U << static_cast<std::uint32_t>(channel);
            const std::uint32_t layer_width =
                floor_align2(cfg.input_width / ratio);
            const std::uint32_t layer_height =
                floor_align2(cfg.input_height / ratio);

            if (layer_width < kPymMinWidth ||
                layer_height < kPymMinHeight) {
                continue;
            }

            std::uint32_t target_width = layer_width;
            std::uint32_t target_height = layer_height;
            if (channel == pym_channel) {
                target_width = cfg.output_width;
                target_height = cfg.output_height;
            }

            pym_cfg.chn_ctrl.ds_roi_sel[channel] =
                (channel == 0) ? PYM_SRC_SEL : PYM_BL_SEL;
            pym_cfg.chn_ctrl.ds_roi_layer[channel] =
                (channel == 0) ? 0 : channel - 1;
            pym_cfg.chn_ctrl.ds_roi_en |= static_cast<std::uint8_t>(
                1U << static_cast<std::uint32_t>(channel));

            roi_box_t &roi = pym_cfg.chn_ctrl.ds_roi_info[channel];
            roi.start_left = 0;
            roi.start_top = 0;
            roi.region_width = layer_width;
            roi.region_height = layer_height;
            roi.out_width = target_width;
            roi.out_height = target_height;
            roi.wstride_y = align16(target_width);
            roi.wstride_uv = align16(target_width);
            roi.vstride = target_height;

            if (cfg.verbose) {
                std::printf(
                    "  DS[%d]: layer=1/%u %ux%u -> %ux%u, stride=%u\n",
                    channel, ratio, layer_width, layer_height,
                    target_width, target_height, roi.wstride_y);
            }
        }

        int ret = hbn_vnode_open(HB_PYM, pym_cfg.hw_id, kAutoAllocId,
                                 &vnode);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_open(HB_PYM) failed: %d\n", ret);
            return ret;
        }
        vnode_opened = true;

        ret = hbn_vnode_set_attr(vnode, &pym_cfg);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_set_attr failed: %d\n", ret);
            return ret;
        }
        ret = hbn_vnode_set_ichn_attr(vnode, 0, &pym_cfg);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_set_ichn_attr failed: %d\n", ret);
            return ret;
        }
        ret = hbn_vnode_set_ochn_attr(vnode, 0, &pym_cfg);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_set_ochn_attr failed: %d\n", ret);
            return ret;
        }

        alloc_attr.buffers_num = pym_cfg.output_buf_num;
        alloc_attr.is_contig = 1;
        alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
                           HB_MEM_USAGE_CPU_WRITE_OFTEN |
                           HB_MEM_USAGE_CACHED;
        ret = hbn_vnode_set_ochn_buf_attr(vnode, 0, &alloc_attr);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_set_ochn_buf_attr failed: %d\n", ret);
            return ret;
        }

        ret = hbn_vflow_create(&vflow);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vflow_create failed: %d\n", ret);
            return ret;
        }
        vflow_created = true;

        ret = hbn_vflow_add_vnode(vflow, vnode);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vflow_add_vnode failed: %d\n", ret);
            return ret;
        }

        ret = hbn_vflow_start(vflow);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vflow_start failed: %d\n", ret);
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
            if (ret != 0) {
                std::fprintf(stderr, "hb_mem_module_open failed: %d\n", ret);
            } else {
                mem_opened = true;
            }
        }
        if (ret == 0) {
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
            std::printf(
                "horddt_resize initialized: %ux%u -> %ux%u, PYM DS[%d]\n",
                cfg.input_width, cfg.input_height,
                cfg.output_width, cfg.output_height, pym_channel);
        }
        return 0;
    }

    int read_input_file(const std::string &input_path)
    {
        FILE *file = std::fopen(input_path.c_str(), "rb");
        if (file == nullptr) {
            std::fprintf(stderr, "Cannot open input file '%s': %s\n",
                         input_path.c_str(), std::strerror(errno));
            return -1;
        }

        const auto stride =
            static_cast<std::uint32_t>(input_image.buffer.stride);
        const bool read_failed =
            read_plane(file,
                       static_cast<std::uint8_t *>(
                           input_image.buffer.virt_addr[0]),
                       cfg.input_height, cfg.input_width, stride) != 0 ||
            read_plane(file,
                       static_cast<std::uint8_t *>(
                           input_image.buffer.virt_addr[1]),
                       cfg.input_height / 2U, cfg.input_width, stride) != 0;
        std::fclose(file);

        if (read_failed) {
            const auto expected =
                static_cast<unsigned long long>(cfg.input_width) *
                cfg.input_height * 3ULL / 2ULL;
            std::fprintf(stderr,
                         "Input file is smaller than one %ux%u NV12 frame "
                         "(%llu bytes).\n",
                         cfg.input_width, cfg.input_height, expected);
            return -1;
        }
        return 0;
    }

    int validate_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                             std::uint32_t expected_width,
                             std::uint32_t expected_height,
                             const char *name) const
    {
        if (buffer.format != MEM_PIX_FMT_NV12) {
            std::fprintf(stderr,
                         "%s format must be MEM_PIX_FMT_NV12 (actual=%d).\n",
                         name, buffer.format);
            return -1;
        }
        if (buffer.width != static_cast<std::int32_t>(expected_width) ||
            buffer.height != static_cast<std::int32_t>(expected_height)) {
            std::fprintf(stderr,
                         "%s size is %dx%d, expected %ux%u.\n",
                         name, buffer.width, buffer.height,
                         expected_width, expected_height);
            return -1;
        }
        if (buffer.plane_cnt < 2) {
            std::fprintf(stderr,
                         "%s must contain at least 2 NV12 planes "
                         "(actual=%d).\n",
                         name, buffer.plane_cnt);
            return -1;
        }
        if (buffer.stride < buffer.width || buffer.vstride < buffer.height) {
            std::fprintf(stderr,
                         "Invalid %s stride: stride=%d, vstride=%d, "
                         "size=%dx%d.\n",
                         name, buffer.stride, buffer.vstride,
                         buffer.width, buffer.height);
            return -1;
        }
        if (buffer.virt_addr[0] == nullptr ||
            buffer.virt_addr[1] == nullptr) {
            std::fprintf(stderr,
                         "%s must have mapped Y and UV virtual addresses.\n",
                         name);
            return -1;
        }
        return 0;
    }

    int flush_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                          const char *name) const
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
            std::fprintf(stderr, "Failed to flush %s Y plane: %d\n",
                         name, ret);
            return ret;
        }

        ret = hb_mem_flush_buf_with_vaddr(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                buffer.virt_addr[1])),
            uv_bytes);
        if (ret != 0) {
            std::fprintf(stderr, "Failed to flush %s UV plane: %d\n",
                         name, ret);
            return ret;
        }
        return 0;
    }

    int copy_nv12_buffer(const hb_mem_graphic_buf_t &source,
                         hb_mem_graphic_buf_t &destination) const
    {
        const std::size_t row_bytes =
            static_cast<std::size_t>(cfg.output_width);
        const std::size_t source_stride =
            static_cast<std::size_t>(source.stride);
        const std::size_t destination_stride =
            static_cast<std::size_t>(destination.stride);

        const auto *source_y =
            static_cast<const std::uint8_t *>(source.virt_addr[0]);
        auto *destination_y =
            static_cast<std::uint8_t *>(destination.virt_addr[0]);
        for (std::uint32_t row = 0; row < cfg.output_height; ++row) {
            std::memcpy(destination_y +
                            static_cast<std::size_t>(row) * destination_stride,
                        source_y +
                            static_cast<std::size_t>(row) * source_stride,
                        row_bytes);
        }

        const auto *source_uv =
            static_cast<const std::uint8_t *>(source.virt_addr[1]);
        auto *destination_uv =
            static_cast<std::uint8_t *>(destination.virt_addr[1]);
        for (std::uint32_t row = 0; row < cfg.output_height / 2U; ++row) {
            std::memcpy(destination_uv +
                            static_cast<std::size_t>(row) * destination_stride,
                        source_uv +
                            static_cast<std::size_t>(row) * source_stride,
                        row_bytes);
        }

        return flush_nv12_buffer(destination, "output buffer");
    }

    int run_pym(const hb_mem_graphic_buf_t &input_buffer,
                hb_mem_graphic_buf_t *destination_buffer,
                const std::string *destination_path)
    {
        if (!initialized) {
            std::fprintf(stderr, "horddt_resize is not initialized.\n");
            return initialization_result != 0 ? initialization_result : -1;
        }

        int ret = validate_nv12_buffer(input_buffer,
                                       cfg.input_width,
                                       cfg.input_height,
                                       "Input buffer");
        if (ret != 0) {
            return ret;
        }
        if (destination_buffer != nullptr) {
            ret = validate_nv12_buffer(*destination_buffer,
                                       cfg.output_width,
                                       cfg.output_height,
                                       "Output buffer");
            if (ret != 0) {
                return ret;
            }
        } else if (destination_path == nullptr || destination_path->empty()) {
            std::fprintf(stderr, "Output destination is not valid.\n");
            return -1;
        }

        ret = flush_nv12_buffer(input_buffer, "input buffer");
        if (ret != 0) {
            return ret;
        }

        hbn_vnode_image_t frame{};
        frame.buffer = input_buffer;
        gettimeofday(&frame.info.tv, nullptr);

        ret = hbn_vnode_sendframe(vnode, 0, &frame);
        if (ret != 0) {
            std::fprintf(stderr, "hbn_vnode_sendframe failed: %d\n", ret);
            return ret;
        }

        hbn_vnode_image_group_t output_group{};
        ret = hbn_vnode_getframe_group(vnode, 0, cfg.timeout_ms,
                                       &output_group);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_getframe_group timed out or failed: %d\n",
                         ret);
            return ret;
        }

        hb_mem_graphic_buf_t &pym_output =
            output_group.buf_group.graph_group[pym_channel];
        if (cfg.verbose) {
            std::printf(
                "PYM output: %dx%d, stride=%d, vstride=%d, planes=%d\n",
                pym_output.width, pym_output.height, pym_output.stride,
                pym_output.vstride, pym_output.plane_cnt);
        }

        ret = validate_nv12_buffer(pym_output,
                                   cfg.output_width,
                                   cfg.output_height,
                                   "PYM output buffer");
        if (ret == 0) {
            if (destination_buffer != nullptr) {
                ret = copy_nv12_buffer(pym_output, *destination_buffer);
            } else {
                ret = write_nv12_image(*destination_path, pym_output);
            }
        }

        const int release_ret =
            hbn_vnode_releaseframe_group(vnode, 0, &output_group);
        if (release_ret != 0) {
            std::fprintf(stderr,
                         "hbn_vnode_releaseframe_group failed: %d\n",
                         release_ret);
            if (ret == 0) {
                ret = release_ret;
            }
        }
        return ret;
    }

    int process_buffer(const hb_mem_graphic_buf_t &input_buffer,
                       hb_mem_graphic_buf_t &output_buffer)
    {
        return run_pym(input_buffer, &output_buffer, nullptr);
    }

    int process_file(const std::string &input_path,
                     const std::string &output_path)
    {
        if (!initialized) {
            std::fprintf(stderr, "horddt_resize is not initialized.\n");
            return initialization_result != 0 ? initialization_result : -1;
        }
        if (input_path.empty() || output_path.empty()) {
            std::fprintf(stderr, "Input and output paths must not be empty.\n");
            return -1;
        }

        const int ret = read_input_file(input_path);
        if (ret != 0) {
            return ret;
        }
        return run_pym(input_image.buffer, nullptr, &output_path);
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
        pym_channel = -1;
    }
};

horddt_resize::horddt_resize(const config &cfg) : impl_(new impl())
{
    impl_->initialize(cfg);
}

horddt_resize::~horddt_resize()
{
    close();
}

int horddt_resize::resize(const std::string &input_path,
                          const std::string &output_path)
{
    return impl_->process_file(input_path, output_path);
}

int horddt_resize::resize(const hb_mem_graphic_buf_t &input_buffer,
                          hb_mem_graphic_buf_t &output_buffer)
{
    return impl_->process_buffer(input_buffer, output_buffer);
}

void horddt_resize::close()
{
    if (impl_) {
        impl_->release();
    }
}

bool horddt_resize::is_initialized() const noexcept
{
    return impl_ && impl_->initialized;
}

int horddt_resize::initialization_status() const noexcept
{
    return impl_ ? impl_->initialization_result : -1;
}
