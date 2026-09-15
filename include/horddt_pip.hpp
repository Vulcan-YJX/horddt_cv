#ifndef HORDDT_CV_HORDDT_PIP_HPP_
#define HORDDT_CV_HORDDT_PIP_HPP_

#include <cstdint>
#include <functional>
#include <memory>

extern "C" {
#include "hb_mem_mgr.h"
}

// Hardware NV12 picture-in-picture compositor based on the D-Robotics
// STITCH vnode.  STITCH performs composition only; resize the overlay before
// passing it to this class (for example with horddt_resize).
class horddt_pip {
public:
    using output_callback =
        std::function<int(const hb_mem_graphic_buf_t &output_buffer)>;

    struct config {
        std::uint32_t background_width = 0;
        std::uint32_t background_height = 0;
        std::uint32_t overlay_width = 0;
        std::uint32_t overlay_height = 0;

        // Top-left position of the overlay in the background/output image.
        // NV12 requires all dimensions and coordinates to be even.
        std::uint32_t overlay_x = 0;
        std::uint32_t overlay_y = 0;

        // STITCH channel strides.  Zero selects 64-byte aligned width, which
        // matches hb_mem_alloc_graph_buf() usage in the samples.
        std::uint32_t background_stride = 0;
        std::uint32_t overlay_stride = 0;
        std::uint32_t output_stride = 0;

        int timeout_ms = 2000;
        std::uint32_t output_buffer_count = 3;

        // Set false when an upstream DMA engine has already synchronized the
        // input for the device.  Keep true for CPU-written/file input.
        bool sync_inputs_for_device = true;
        bool sync_borrowed_output_for_cpu = true;
        bool verbose = false;
    };

    explicit horddt_pip(const config &cfg);
    ~horddt_pip();

    horddt_pip(const horddt_pip &) = delete;
    horddt_pip &operator=(const horddt_pip &) = delete;

    // Composes an opaque overlay over the background and copies the STITCH
    // result into a caller-owned NV12 hbmem buffer.
    int compose(const hb_mem_graphic_buf_t &background,
                const hb_mem_graphic_buf_t &overlay,
                hb_mem_graphic_buf_t &output);

    // Zero-copy result path.  The STITCH-owned output buffer is valid only
    // during callback execution and must not be retained or freed.
    int compose_borrowed(const hb_mem_graphic_buf_t &background,
                         const hb_mem_graphic_buf_t &overlay,
                         const output_callback &callback);

    void close();

    bool is_initialized() const noexcept;
    int initialization_status() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

#endif  // HORDDT_CV_HORDDT_PIP_HPP_
