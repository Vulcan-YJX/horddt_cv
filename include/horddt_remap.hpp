#ifndef HORDDT_CV_HORDDT_REMAP_HPP_
#define HORDDT_CV_HORDDT_REMAP_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include "hb_mem_mgr.h"
}

// NV12 geometric distortion correction/remapping based on the D-Robotics GDC
// feedback-mode vnode. The GDC binary must match the configured image size and
// the lens/camera calibration used to generate it.
class horddt_remap {
public:
    using output_callback =
        std::function<int(const hb_mem_graphic_buf_t &output_buffer)>;
    struct config {
        std::uint32_t input_width = 0;
        std::uint32_t input_height = 0;
        std::uint32_t output_width = 0;
        std::uint32_t output_height = 0;

        // Zero selects align16(width), matching multimedia_samples/sample_gdc.
        std::uint32_t input_stride = 0;
        std::uint32_t output_stride = 0;

        std::string gdc_bin_path;
        int timeout_ms = 2000;
        int hw_id = 0;
        std::uint32_t output_buffer_count = 2;
        bool sync_input_for_device = true;
        bool sync_borrowed_output_for_cpu = true;
        bool verbose = false;
    };

    // Saves the parameters and initializes hbmem/GDC immediately.
    explicit horddt_remap(const config &cfg);
    ~horddt_remap();

    horddt_remap(const horddt_remap &) = delete;
    horddt_remap &operator=(const horddt_remap &) = delete;

    // Applies the GDC mapping to a caller-owned NV12 input buffer and copies
    // the result to a caller-owned NV12 output buffer. Neither buffer is freed
    // by this class.
    int remap(const hb_mem_graphic_buf_t &input_buffer,
              hb_mem_graphic_buf_t &output_buffer);

    // Zero-copy output path. The GDC-owned buffer is valid only for the
    // callback duration. When sync_borrowed_output_for_cpu is true, its cache
    // is invalidated before callback for CPU use.
    int remap_borrowed(const hb_mem_graphic_buf_t &input_buffer,
                       const output_callback &callback);

    // Stops the GDC vnode and releases resources owned by this class.
    void close();

    bool is_initialized() const noexcept;
    int initialization_status() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

#endif  // HORDDT_CV_HORDDT_REMAP_HPP_
