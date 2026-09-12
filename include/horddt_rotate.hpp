#ifndef HORDDT_CV_HORDDT_ROTATE_HPP_
#define HORDDT_CV_HORDDT_ROTATE_HPP_

#include <cstdint>
#include <functional>
#include <memory>

extern "C" {
#include "hb_mem_mgr.h"
}

// NV12 right-angle rotation implemented with the D-Robotics GDC affine
// transformation. The rotation configuration is generated at construction;
// callers do not need to provide a pre-generated GDC binary.
class horddt_rotate {
public:
    enum class angle : std::uint16_t {
        rotate_90 = 90,
        rotate_180 = 180,
        rotate_270 = 270,
    };

    using output_callback =
        std::function<int(const hb_mem_graphic_buf_t &output_buffer)>;

    struct config {
        std::uint32_t input_width = 0;
        std::uint32_t input_height = 0;
        angle rotation = angle::rotate_90;

        // Zero selects align16(width). For 90/270 degrees the output width is
        // input_height; for 180 degrees it is input_width.
        std::uint32_t input_stride = 0;
        std::uint32_t output_stride = 0;

        int timeout_ms = 2000;
        int hw_id = 0;
        std::uint32_t output_buffer_count = 2;

        // Set false if the input is produced by DMA and its cache state has
        // already been handled by the upstream component.
        bool sync_input_for_device = true;

        // Keep true when the borrowed output is read by the CPU. Set false
        // when it is passed directly to another DMA engine.
        bool sync_borrowed_output_for_cpu = true;
        bool verbose = false;
    };

    explicit horddt_rotate(const config &cfg);
    ~horddt_rotate();

    horddt_rotate(const horddt_rotate &) = delete;
    horddt_rotate &operator=(const horddt_rotate &) = delete;

    // Rotates a caller-owned NV12 input into a caller-owned NV12 output.
    // Neither buffer is released by this class.
    int rotate(const hb_mem_graphic_buf_t &input_buffer,
               hb_mem_graphic_buf_t &output_buffer);

    // Zero-copy output path. The GDC-owned output is valid only while callback
    // is running and must not be retained or freed by the caller.
    int rotate_borrowed(const hb_mem_graphic_buf_t &input_buffer,
                        const output_callback &callback);

    std::uint32_t output_width() const noexcept;
    std::uint32_t output_height() const noexcept;

    void close();
    bool is_initialized() const noexcept;
    int initialization_status() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

#endif  // HORDDT_CV_HORDDT_ROTATE_HPP_
