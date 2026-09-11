#ifndef HORDDT_CV_HORDDT_RESIZE_HPP_
#define HORDDT_CV_HORDDT_RESIZE_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include "hb_mem_mgr.h"
}

class horddt_resize {
public:
    using output_callback =
        std::function<int(const hb_mem_graphic_buf_t &output_buffer)>;
    struct config {
        std::uint32_t input_width = 0;
        std::uint32_t input_height = 0;
        std::uint32_t output_width = 0;
        std::uint32_t output_height = 0;
        int timeout_ms = 2000;

        // Disable the private file-input buffer in live buffer-only pipelines.
        bool enable_file_io = true;

        // Two buffers are enough for this synchronous borrowed-output API.
        // Increase these values only when integrating the vnode in a deeper
        // asynchronous pipeline.
        std::uint32_t output_buffer_count = 2;
        std::uint32_t feedback_buffer_count = 2;

        // By default only the pyramid layers required to reach the selected
        // output channel are enabled. PYM DS scaling is (1/2, 1] per layer;
        // an exact 1/2 request is served by the next BL layer at scale 1.0.
        // Set this only when other layers are consumed externally.
        bool enable_extra_layers = false;

        // Set false when the input was just produced by a DMA engine and no
        // CPU writer touched it. Borrowed output synchronization is needed
        // only when the callback reads the image on CPU.
        bool sync_input_for_device = true;
        bool sync_borrowed_output_for_cpu = true;
        bool verbose = false;
    };

    // Stores the resize parameters and initializes hbmem/PYM immediately.
    explicit horddt_resize(const config &cfg);
    ~horddt_resize();

    horddt_resize(const horddt_resize &) = delete;
    horddt_resize &operator=(const horddt_resize &) = delete;

    // Resizes the first NV12 frame in input_path and writes a compact NV12 file.
    // This overload is kept for command-line and file-based use.
    int resize(const std::string &input_path, const std::string &output_path);

    // Resizes an NV12 hbmem buffer into another caller-owned hbmem buffer.
    // Both buffers are borrowed and must remain valid until this call returns.
    int resize(const hb_mem_graphic_buf_t &input_buffer,
               hb_mem_graphic_buf_t &output_buffer);

    // Zero-copy output path. The PYM-owned buffer is valid only while callback
    // is running and must not be retained or freed by the caller. When
    // sync_borrowed_output_for_cpu is true, its cache is invalidated before
    // callback so CPU consumers can read it immediately.
    int resize_borrowed(const hb_mem_graphic_buf_t &input_buffer,
                        const output_callback &callback);

    // Stops the PYM pipeline and releases resources owned by this class.
    // Caller-owned input/output buffers are never released by this class.
    void close();

    bool is_initialized() const noexcept;
    int initialization_status() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

#endif  // HORDDT_CV_HORDDT_RESIZE_HPP_
