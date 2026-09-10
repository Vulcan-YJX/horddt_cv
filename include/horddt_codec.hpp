#ifndef HORDDT_CV_HORDDT_CODEC_HPP_
#define HORDDT_CV_HORDDT_CODEC_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include "hb_mem_mgr.h"
}

// Hardware JPEG/H.264 codec based on multimedia_samples/sample_codec and
// sample_pipeline/common/vp_codec. The class owns and reuses one JPEG encoder,
// one H.264 encoder, and one JPEG decoder.
class horddt_codec {
public:
    struct config {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t frame_rate = 30;
        std::uint32_t bit_rate = 8192;
        int jpeg_quality = 90;
        int timeout_ms = 2000;
        bool verbose = false;
    };

    // Saves the parameters and initializes all hardware codec contexts.
    explicit horddt_codec(const config &cfg);
    ~horddt_codec();

    horddt_codec(const horddt_codec &) = delete;
    horddt_codec &operator=(const horddt_codec &) = delete;

    // Encodes one caller-owned NV12 graphic buffer to a complete JPEG image.
    int nv12_to_jpeg(const hb_mem_graphic_buf_t &input_buffer,
                     std::vector<std::uint8_t> &jpeg_data);

    // Encodes one caller-owned NV12 graphic buffer to one Annex-B H.264 frame.
    int nv12_to_h264(const hb_mem_graphic_buf_t &input_buffer,
                     std::vector<std::uint8_t> &h264_data);

    // Decodes one complete JPEG image to NV12 in hardware, then encodes that
    // NV12 frame to one Annex-B H.264 frame in hardware.
    int jpeg_to_h264(const std::uint8_t *jpeg_data,
                     std::size_t jpeg_size,
                     std::vector<std::uint8_t> &h264_data);

    int jpeg_to_h264(const std::vector<std::uint8_t> &jpeg_data,
                     std::vector<std::uint8_t> &h264_data);

    // Pauses and releases all codec contexts. Safe to call more than once.
    void close();

    bool is_initialized() const noexcept;
    int initialization_status() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

#endif  // HORDDT_CV_HORDDT_CODEC_HPP_
