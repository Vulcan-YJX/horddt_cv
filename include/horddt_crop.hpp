#ifndef HORDDT_CV_HORDDT_CROP_HPP_
#define HORDDT_CV_HORDDT_CROP_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "hb_mem_mgr.h"
}

// NV12 hardware crop operator implemented with PYM DS ROI channels.
// One input frame is submitted once and all configured regions are produced by
// the same PYM output group, so crop_borrowed() does not copy pixel data.
class horddt_crop {
public:
    struct rect {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct config {
        std::uint32_t input_width = 0;
        std::uint32_t input_height = 0;
        std::vector<rect> regions;
        int timeout_ms = 2000;

        // Allocate a private input buffer only for the file-to-file overload.
        // Disable this in a live hbmem pipeline.
        bool enable_file_io = true;

        std::uint32_t output_buffer_count = 2;
        std::uint32_t feedback_buffer_count = 2;

        // Set false when input is written by a DMA producer and its cache state
        // has already been handled by the producer/pipeline.
        bool sync_input_for_device = true;

        // Set false when borrowed outputs are passed directly to another DMA
        // engine. Keep true when the callback reads them on CPU.
        bool sync_borrowed_output_for_cpu = true;
        bool verbose = false;
    };

    // Descriptors in outputs point to PYM-owned buffers. Their pixels are not
    // copied and are valid only until callback returns.
    using output_callback =
        std::function<int(const std::vector<hb_mem_graphic_buf_t> &outputs)>;

    explicit horddt_crop(const config &cfg);
    ~horddt_crop();

    horddt_crop(const horddt_crop &) = delete;
    horddt_crop &operator=(const horddt_crop &) = delete;

    // File helper: reads one compact NV12 frame and writes one compact NV12
    // file for every configured region.
    int crop(const std::string &input_path,
             const std::vector<std::string> &output_paths);

    // Copies PYM results to caller-owned hbmem buffers. outputs.size() and each
    // output dimension must match config.regions.
    int crop(const hb_mem_graphic_buf_t &input_buffer,
             std::vector<hb_mem_graphic_buf_t> &outputs);

    // Preferred real-time path: no pixel copy is performed.
    int crop_borrowed(const hb_mem_graphic_buf_t &input_buffer,
                      const output_callback &callback);

    void close();
    bool is_initialized() const noexcept;
    int initialization_status() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

#endif  // HORDDT_CV_HORDDT_CROP_HPP_
