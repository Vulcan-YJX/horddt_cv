/***************************************************************************
 * Hardware JPEG/H.264 codec class based on multimedia_samples/sample_codec
 * and multimedia_samples/sample_pipeline/common/vp_codec.
 ***************************************************************************/

#include "horddt_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

extern "C" {
#include "hb_media_codec.h"
#include "hb_media_error.h"
}

namespace {

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

int validate_nv12_buffer(const hb_mem_graphic_buf_t &buffer,
                         std::uint32_t width,
                         std::uint32_t height)
{
    if (buffer.format != MEM_PIX_FMT_NV12) {
        std::fprintf(stderr, "Codec input format must be NV12, got %d.\n",
                     buffer.format);
        return -1;
    }
    if (buffer.width != static_cast<std::int32_t>(width) ||
        buffer.height != static_cast<std::int32_t>(height)) {
        std::fprintf(stderr,
                     "Codec input size is %dx%d, expected %ux%u.\n",
                     buffer.width, buffer.height, width, height);
        return -1;
    }
    if (buffer.plane_cnt < 2 || buffer.virt_addr[0] == nullptr ||
        buffer.virt_addr[1] == nullptr) {
        std::fprintf(stderr, "Codec input has invalid NV12 planes.\n");
        return -1;
    }
    if (buffer.stride < buffer.width || buffer.vstride < buffer.height) {
        std::fprintf(stderr,
                     "Codec input has invalid stride=%d, vstride=%d.\n",
                     buffer.stride, buffer.vstride);
        return -1;
    }
    return 0;
}

int copy_nv12(const std::uint8_t *source_y,
              const std::uint8_t *source_uv,
              std::uint32_t source_stride,
              std::uint8_t *destination_y,
              std::uint8_t *destination_uv,
              std::uint32_t destination_stride,
              std::uint32_t width,
              std::uint32_t height)
{
    if (source_y == nullptr || source_uv == nullptr ||
        destination_y == nullptr || destination_uv == nullptr ||
        source_stride < width || destination_stride < width) {
        return -1;
    }

    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination_y +
                        static_cast<std::size_t>(row) * destination_stride,
                    source_y + static_cast<std::size_t>(row) * source_stride,
                    static_cast<std::size_t>(width));
    }
    for (std::uint32_t row = 0; row < height / 2U; ++row) {
        std::memcpy(destination_uv +
                        static_cast<std::size_t>(row) * destination_stride,
                    source_uv + static_cast<std::size_t>(row) * source_stride,
                    static_cast<std::size_t>(width));
    }
    return 0;
}

}  // namespace

struct horddt_codec::impl {
    struct codec_state {
        media_codec_context_t context{};
        bool initialized = false;
        bool started = false;
    };

    config cfg{};
    codec_state jpeg_encoder{};
    codec_state h264_encoder{};
    codec_state jpeg_decoder{};
    int initialization_result = -1;
    bool initialized = false;
    std::mutex mutex{};

    int validate_config() const
    {
        if (cfg.width == 0U || cfg.height == 0U ||
            cfg.width > static_cast<std::uint32_t>(
                            std::numeric_limits<std::int32_t>::max()) ||
            cfg.height > static_cast<std::uint32_t>(
                             std::numeric_limits<std::int32_t>::max())) {
            std::fprintf(stderr, "Invalid codec image size %ux%u.\n",
                         cfg.width, cfg.height);
            return -1;
        }
        if ((cfg.width & 1U) != 0U || (cfg.height & 1U) != 0U) {
            std::fprintf(stderr, "NV12 codec dimensions must be even.\n");
            return -1;
        }
        if (cfg.frame_rate == 0U || cfg.bit_rate == 0U) {
            std::fprintf(stderr,
                         "H.264 frame_rate and bit_rate must not be zero.\n");
            return -1;
        }
        if (cfg.jpeg_quality < 1 || cfg.jpeg_quality > 100) {
            std::fprintf(stderr, "JPEG quality must be in [1, 100].\n");
            return -1;
        }
        if (cfg.timeout_ms <= 0) {
            std::fprintf(stderr, "Codec timeout must be positive.\n");
            return -1;
        }
        if (!cfg.enable_jpeg_encoder && !cfg.enable_h264_encoder &&
            !cfg.enable_jpeg_decoder) {
            std::fprintf(stderr, "At least one codec context must be enabled.\n");
            return -1;
        }
        if (cfg.frame_buffer_count == 0U ||
            cfg.bitstream_buffer_count == 0U ||
            cfg.frame_buffer_count >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            cfg.bitstream_buffer_count >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            std::fprintf(stderr, "Codec buffer counts must be positive.\n");
            return -1;
        }
        const std::uint64_t frame_size =
            static_cast<std::uint64_t>(cfg.width) * cfg.height * 3U / 2U;
        if (frame_size > std::numeric_limits<std::uint32_t>::max() - 4095U) {
            std::fprintf(stderr, "Codec image is too large.\n");
            return -1;
        }
        return 0;
    }

    std::uint32_t frame_size() const
    {
        return static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(cfg.width) * cfg.height * 3U / 2U);
    }

    int configure_encoder(codec_state &state, media_codec_id_t codec_id)
    {
        std::memset(&state.context, 0, sizeof(state.context));
        state.context.encoder = true;
        state.context.codec_id = codec_id;

        mc_video_codec_enc_params_t &params =
            state.context.video_enc_params;
        params.width = static_cast<std::int32_t>(cfg.width);
        params.height = static_cast<std::int32_t>(cfg.height);
        params.pix_fmt = MC_PIXEL_FORMAT_NV12;
        params.bitstream_buf_size = align_up(frame_size(), 1024U);
        params.frame_buf_count =
            static_cast<int>(cfg.frame_buffer_count);
        params.external_frame_buf = false;
        params.bitstream_buf_count =
            static_cast<int>(cfg.bitstream_buffer_count);
        params.gop_params.gop_preset_idx = 1;
        params.rot_degree = MC_CCW_0;
        params.mir_direction = MC_DIRECTION_NONE;
        params.frame_cropping_flag = false;
        params.enable_user_pts = 1;

        if (codec_id == MEDIA_CODEC_ID_H264) {
            params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
            int ret = hb_mm_mc_get_rate_control_config(
                &state.context, &params.rc_params);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hb_mm_mc_get_rate_control_config(H264) "
                             "failed: %d\n",
                             ret);
                return ret;
            }

            params.rc_params.h264_cbr_params.intra_period = 30;
            params.rc_params.h264_cbr_params.intra_qp = 30;
            params.rc_params.h264_cbr_params.bit_rate = cfg.bit_rate;
            params.rc_params.h264_cbr_params.frame_rate = cfg.frame_rate;
            params.rc_params.h264_cbr_params.initial_rc_qp = 20;
            params.rc_params.h264_cbr_params.vbv_buffer_size = 20;
            params.rc_params.h264_cbr_params.mb_level_rc_enalbe = 1;
            params.rc_params.h264_cbr_params.min_qp_I = 8;
            params.rc_params.h264_cbr_params.max_qp_I = 50;
            params.rc_params.h264_cbr_params.min_qp_P = 8;
            params.rc_params.h264_cbr_params.max_qp_P = 50;
            params.rc_params.h264_cbr_params.min_qp_B = 8;
            params.rc_params.h264_cbr_params.max_qp_B = 50;
            params.rc_params.h264_cbr_params.hvs_qp_enable = 1;
            params.rc_params.h264_cbr_params.hvs_qp_scale = 2;
            params.rc_params.h264_cbr_params.max_delta_qp = 10;
            params.rc_params.h264_cbr_params.qp_map_enable = 0;
        } else if (codec_id == MEDIA_CODEC_ID_JPEG) {
            params.jpeg_enc_config.quality_factor = cfg.jpeg_quality;
            params.mjpeg_enc_config.restart_interval =
                static_cast<std::int32_t>(cfg.width / 16U);
            params.bitstream_buf_size = align_up(frame_size(), 4096U);
        } else {
            return -1;
        }

        return initialize_and_start(state);
    }

    int configure_jpeg_decoder(codec_state &state)
    {
        std::memset(&state.context, 0, sizeof(state.context));
        state.context.encoder = false;
        state.context.codec_id = MEDIA_CODEC_ID_JPEG;

        mc_video_codec_dec_params_t &params =
            state.context.video_dec_params;
        params.feed_mode = MC_FEEDING_MODE_FRAME_SIZE;
        params.pix_fmt = MC_PIXEL_FORMAT_NV12;
        params.bitstream_buf_size = align_up(frame_size(), 1024U);
        params.bitstream_buf_count =
            static_cast<int>(cfg.bitstream_buffer_count);
        params.frame_buf_count =
            static_cast<int>(cfg.frame_buffer_count);
        params.jpeg_dec_config.frame_crop_enable = 0;
        params.jpeg_dec_config.rot_degree = MC_CCW_0;
        params.jpeg_dec_config.mir_direction = MC_DIRECTION_NONE;
        params.mjpeg_dec_config.frame_crop_enable = false;

        return initialize_and_start(state);
    }

    int initialize_and_start(codec_state &state)
    {
        int ret = hb_mm_mc_initialize(&state.context);
        if (ret != 0) {
            std::fprintf(stderr, "hb_mm_mc_initialize(codec=%d) failed: %d\n",
                         state.context.codec_id, ret);
            return ret;
        }
        state.initialized = true;

        ret = hb_mm_mc_configure(&state.context);
        if (ret != 0) {
            std::fprintf(stderr, "hb_mm_mc_configure(codec=%d) failed: %d\n",
                         state.context.codec_id, ret);
            release_state(state);
            return ret;
        }

        mc_av_codec_startup_params_t startup_params{};
        ret = hb_mm_mc_start(&state.context, &startup_params);
        if (ret != 0) {
            std::fprintf(stderr, "hb_mm_mc_start(codec=%d) failed: %d\n",
                         state.context.codec_id, ret);
            release_state(state);
            return ret;
        }
        state.started = true;

        if (cfg.verbose) {
            std::printf("%s codec=%d instance=%d initialized.\n",
                        state.context.encoder ? "Encoder" : "Decoder",
                        state.context.codec_id,
                        state.context.instance_index);
        }
        return 0;
    }

    void release_state(codec_state &state)
    {
        if (state.started) {
            const int ret = hb_mm_mc_pause(&state.context);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hb_mm_mc_pause(codec=%d) failed: %d\n",
                             state.context.codec_id, ret);
            }
            state.started = false;
        }
        if (state.initialized) {
            const int ret = hb_mm_mc_release(&state.context);
            if (ret != 0) {
                std::fprintf(stderr,
                             "hb_mm_mc_release(codec=%d) failed: %d\n",
                             state.context.codec_id, ret);
            }
            state.initialized = false;
        }
    }

    int initialize()
    {
        int ret = validate_config();
        if (ret != 0) {
            return ret;
        }

        if (cfg.enable_jpeg_encoder) {
            ret = configure_encoder(jpeg_encoder, MEDIA_CODEC_ID_JPEG);
            if (ret != 0) {
                close_unlocked();
                return ret;
            }
        }
        if (cfg.enable_h264_encoder) {
            ret = configure_encoder(h264_encoder, MEDIA_CODEC_ID_H264);
            if (ret != 0) {
                close_unlocked();
                return ret;
            }
        }
        if (cfg.enable_jpeg_decoder) {
            ret = configure_jpeg_decoder(jpeg_decoder);
            if (ret != 0) {
                close_unlocked();
                return ret;
            }
        }

        initialized = true;
        return 0;
    }

    void close_unlocked()
    {
        initialized = false;
        release_state(jpeg_decoder);
        release_state(h264_encoder);
        release_state(jpeg_encoder);
    }

    int prepare_encoder_input(codec_state &encoder,
                              media_codec_buffer_t &input,
                              const std::uint8_t *source_y,
                              const std::uint8_t *source_uv,
                              std::uint32_t source_stride)
    {
        std::memset(&input, 0, sizeof(input));
        input.type = MC_VIDEO_FRAME_BUFFER;
        int ret = hb_mm_mc_dequeue_input_buffer(
            &encoder.context, &input, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_dequeue_input_buffer(codec=%d) "
                         "failed: %d\n",
                         encoder.context.codec_id, ret);
            return ret;
        }

        input.type = MC_VIDEO_FRAME_BUFFER;
        input.vframe_buf.width = static_cast<std::int32_t>(cfg.width);
        input.vframe_buf.height = static_cast<std::int32_t>(cfg.height);
        input.vframe_buf.pix_fmt = MC_PIXEL_FORMAT_NV12;
        input.vframe_buf.size = frame_size();

        const std::uint32_t destination_stride =
            input.vframe_buf.stride >= static_cast<std::int32_t>(cfg.width)
                ? static_cast<std::uint32_t>(input.vframe_buf.stride)
                : cfg.width;
        ret = copy_nv12(
            source_y, source_uv, source_stride,
            input.vframe_buf.vir_ptr[0], input.vframe_buf.vir_ptr[1],
            destination_stride, cfg.width, cfg.height);
        if (ret != 0) {
            std::fprintf(stderr, "Invalid codec encoder input buffer.\n");
            // multimedia_samples returns a dequeued input buffer by queuing it.
            input.vframe_buf.size = 0;
            (void)hb_mm_mc_queue_input_buffer(
                &encoder.context, &input, cfg.timeout_ms);
            return ret;
        }
        return 0;
    }

    int collect_encoded_output_borrowed(
        codec_state &encoder,
        const horddt_codec::encoded_callback &callback)
    {
        media_codec_buffer_t output{};
        media_codec_output_buffer_info_t info{};
        int ret = hb_mm_mc_dequeue_output_buffer(
            &encoder.context, &output, &info, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_dequeue_output_buffer(codec=%d) "
                         "failed: %d\n",
                         encoder.context.codec_id, ret);
            return ret;
        }

        int result = 0;
        if (output.vstream_buf.vir_ptr == nullptr ||
            output.vstream_buf.size == 0U) {
            std::fprintf(stderr, "Codec returned an empty encoded stream.\n");
            result = -1;
        } else if (!callback) {
            std::fprintf(stderr, "Encoded output callback must not be empty.\n");
            result = -1;
        } else {
            try {
                result = callback(
                    static_cast<const std::uint8_t *>(
                        output.vstream_buf.vir_ptr),
                    static_cast<std::size_t>(output.vstream_buf.size));
            } catch (...) {
                // The SDK-owned output must be queued even when user code
                // fails, otherwise the encoder eventually runs out of buffers.
                std::fprintf(stderr,
                             "Encoded output callback threw an exception.\n");
                result = -1;
            }
        }

        ret = hb_mm_mc_queue_output_buffer(
            &encoder.context, &output, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_queue_output_buffer(codec=%d) failed: %d\n",
                         encoder.context.codec_id, ret);
            if (result == 0) {
                result = ret;
            }
        }
        return result;
    }

    int collect_encoded_output(codec_state &encoder,
                               std::vector<std::uint8_t> &encoded_data)
    {
        return collect_encoded_output_borrowed(
            encoder,
            [&encoded_data](const std::uint8_t *data, std::size_t size) {
                try {
                    encoded_data.assign(data, data + size);
                } catch (const std::bad_alloc &) {
                    std::fprintf(stderr,
                                 "Cannot allocate encoded output vector.\n");
                    return -1;
                }
                return 0;
            });
    }

    int encode_nv12_borrowed(
        codec_state &encoder,
        const std::uint8_t *source_y,
        const std::uint8_t *source_uv,
        std::uint32_t source_stride,
        const horddt_codec::encoded_callback &callback)
    {
        if (!encoder.initialized || !encoder.started) {
            std::fprintf(stderr, "Requested codec encoder is disabled.\n");
            return -1;
        }
        media_codec_buffer_t input{};
        int ret = prepare_encoder_input(
            encoder, input, source_y, source_uv, source_stride);
        if (ret != 0) {
            return ret;
        }
        ret = hb_mm_mc_queue_input_buffer(
            &encoder.context, &input, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_queue_input_buffer(codec=%d) failed: %d\n",
                         encoder.context.codec_id, ret);
            return ret;
        }
        return collect_encoded_output_borrowed(encoder, callback);
    }

    int encode_nv12(codec_state &encoder,
                    const std::uint8_t *source_y,
                    const std::uint8_t *source_uv,
                    std::uint32_t source_stride,
                    std::vector<std::uint8_t> &encoded_data)
    {
        encoded_data.clear();
        if (!encoder.initialized || !encoder.started) {
            std::fprintf(stderr, "Requested codec encoder is disabled.\n");
            return -1;
        }
        media_codec_buffer_t input{};
        int ret = prepare_encoder_input(
            encoder, input, source_y, source_uv, source_stride);
        if (ret != 0) {
            return ret;
        }

        ret = hb_mm_mc_queue_input_buffer(
            &encoder.context, &input, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_queue_input_buffer(codec=%d) failed: %d\n",
                         encoder.context.codec_id, ret);
            return ret;
        }
        return collect_encoded_output(encoder, encoded_data);
    }

    int encode_graphic_buffer(codec_state &encoder,
                              const hb_mem_graphic_buf_t &input_buffer,
                              std::vector<std::uint8_t> &encoded_data)
    {
        const int ret = validate_nv12_buffer(
            input_buffer, cfg.width, cfg.height);
        if (ret != 0) {
            return ret;
        }

        return encode_nv12(
            encoder,
            static_cast<const std::uint8_t *>(input_buffer.virt_addr[0]),
            static_cast<const std::uint8_t *>(input_buffer.virt_addr[1]),
            static_cast<std::uint32_t>(input_buffer.stride), encoded_data);
    }

    int encode_graphic_buffer_borrowed(
        codec_state &encoder,
        const hb_mem_graphic_buf_t &input_buffer,
        const horddt_codec::encoded_callback &callback)
    {
        const int ret = validate_nv12_buffer(
            input_buffer, cfg.width, cfg.height);
        if (ret != 0) {
            return ret;
        }
        return encode_nv12_borrowed(
            encoder,
            static_cast<const std::uint8_t *>(input_buffer.virt_addr[0]),
            static_cast<const std::uint8_t *>(input_buffer.virt_addr[1]),
            static_cast<std::uint32_t>(input_buffer.stride), callback);
    }

    int decode_jpeg_and_encode_h264(const std::uint8_t *jpeg_data,
                                    std::size_t jpeg_size,
                                    std::vector<std::uint8_t> &h264_data)
    {
        h264_data.clear();
        if (!jpeg_decoder.initialized || !h264_encoder.initialized) {
            std::fprintf(stderr,
                         "JPEG decoder or H.264 encoder is disabled.\n");
            return -1;
        }
        if (jpeg_data == nullptr || jpeg_size == 0U ||
            jpeg_size > std::numeric_limits<std::uint32_t>::max()) {
            std::fprintf(stderr, "Invalid JPEG input data.\n");
            return -1;
        }

        media_codec_buffer_t decoder_input{};
        decoder_input.type = MC_VIDEO_STREAM_BUFFER;
        int ret = hb_mm_mc_dequeue_input_buffer(
            &jpeg_decoder.context, &decoder_input, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_dequeue_input_buffer(JPEG decoder) "
                         "failed: %d\n",
                         ret);
            return ret;
        }

        if (decoder_input.vstream_buf.vir_ptr == nullptr ||
            jpeg_size > decoder_input.vstream_buf.size) {
            std::fprintf(
                stderr,
                "JPEG size %zu exceeds decoder input capacity %zu.\n",
                jpeg_size,
                static_cast<std::size_t>(decoder_input.vstream_buf.size));
            decoder_input.type = MC_VIDEO_STREAM_BUFFER;
            decoder_input.vstream_buf.size = 0U;
            decoder_input.vstream_buf.stream_end = 0;
            (void)hb_mm_mc_queue_input_buffer(
                &jpeg_decoder.context, &decoder_input, cfg.timeout_ms);
            return -1;
        }

        std::memcpy(decoder_input.vstream_buf.vir_ptr,
                    jpeg_data, jpeg_size);
        decoder_input.type = MC_VIDEO_STREAM_BUFFER;
        decoder_input.vstream_buf.size =
            static_cast<std::uint32_t>(jpeg_size);
        decoder_input.vstream_buf.stream_end = 0;
        ret = hb_mm_mc_queue_input_buffer(
            &jpeg_decoder.context, &decoder_input, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_queue_input_buffer(JPEG decoder) "
                         "failed: %d\n",
                         ret);
            return ret;
        }

        media_codec_buffer_t decoded{};
        media_codec_output_buffer_info_t decode_info{};
        ret = hb_mm_mc_dequeue_output_buffer(
            &jpeg_decoder.context, &decoded, &decode_info, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_dequeue_output_buffer(JPEG decoder) "
                         "failed: %d\n",
                         ret);
            return ret;
        }

        int result = 0;
        if (decoded.type != MC_VIDEO_FRAME_BUFFER ||
            decode_info.video_frame_info.decode_result == 0 ||
            decoded.vframe_buf.size == 0U ||
            decoded.vframe_buf.vir_ptr[0] == nullptr ||
            decoded.vframe_buf.vir_ptr[1] == nullptr) {
            std::fprintf(stderr, "JPEG decoder returned an invalid frame.\n");
            result = -1;
        } else if (decoded.vframe_buf.width <
                       static_cast<std::int32_t>(cfg.width) ||
                   decoded.vframe_buf.height <
                       static_cast<std::int32_t>(cfg.height)) {
            std::fprintf(stderr,
                         "Decoded JPEG is %dx%d, expected at least %ux%u.\n",
                         decoded.vframe_buf.width,
                         decoded.vframe_buf.height,
                         cfg.width, cfg.height);
            result = -1;
        } else {
            const std::uint32_t decoded_stride =
                decoded.vframe_buf.stride >=
                        static_cast<std::int32_t>(cfg.width)
                    ? static_cast<std::uint32_t>(decoded.vframe_buf.stride)
                    : static_cast<std::uint32_t>(decoded.vframe_buf.width);
            result = encode_nv12(
                h264_encoder,
                decoded.vframe_buf.vir_ptr[0],
                decoded.vframe_buf.vir_ptr[1],
                decoded_stride, h264_data);
        }

        ret = hb_mm_mc_queue_output_buffer(
            &jpeg_decoder.context, &decoded, cfg.timeout_ms);
        if (ret != 0) {
            std::fprintf(stderr,
                         "hb_mm_mc_queue_output_buffer(JPEG decoder) "
                         "failed: %d\n",
                         ret);
            if (result == 0) {
                result = ret;
            }
        }
        return result;
    }
};

horddt_codec::horddt_codec(const config &cfg)
    : impl_(new impl)
{
    impl_->cfg = cfg;
    impl_->initialization_result = impl_->initialize();
}

horddt_codec::~horddt_codec()
{
    close();
}

int horddt_codec::nv12_to_jpeg(
    const hb_mem_graphic_buf_t &input_buffer,
    std::vector<std::uint8_t> &jpeg_data)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        jpeg_data.clear();
        return impl_->initialization_result;
    }
    return impl_->encode_graphic_buffer(
        impl_->jpeg_encoder, input_buffer, jpeg_data);
}

int horddt_codec::nv12_to_jpeg_borrowed(
    const hb_mem_graphic_buf_t &input_buffer,
    const encoded_callback &callback)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return impl_->initialization_result;
    }
    return impl_->encode_graphic_buffer_borrowed(
        impl_->jpeg_encoder, input_buffer, callback);
}

int horddt_codec::nv12_to_h264(
    const hb_mem_graphic_buf_t &input_buffer,
    std::vector<std::uint8_t> &h264_data)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        h264_data.clear();
        return impl_->initialization_result;
    }
    return impl_->encode_graphic_buffer(
        impl_->h264_encoder, input_buffer, h264_data);
}

int horddt_codec::nv12_to_h264_borrowed(
    const hb_mem_graphic_buf_t &input_buffer,
    const encoded_callback &callback)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return impl_->initialization_result;
    }
    return impl_->encode_graphic_buffer_borrowed(
        impl_->h264_encoder, input_buffer, callback);
}

int horddt_codec::jpeg_to_h264(
    const std::uint8_t *jpeg_data,
    std::size_t jpeg_size,
    std::vector<std::uint8_t> &h264_data)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        h264_data.clear();
        return impl_->initialization_result;
    }
    return impl_->decode_jpeg_and_encode_h264(
        jpeg_data, jpeg_size, h264_data);
}

int horddt_codec::jpeg_to_h264(
    const std::vector<std::uint8_t> &jpeg_data,
    std::vector<std::uint8_t> &h264_data)
{
    return jpeg_to_h264(jpeg_data.data(), jpeg_data.size(), h264_data);
}

void horddt_codec::close()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->close_unlocked();
}

bool horddt_codec::is_initialized() const noexcept
{
    return impl_->initialized;
}

int horddt_codec::initialization_status() const noexcept
{
    return impl_->initialization_result;
}
