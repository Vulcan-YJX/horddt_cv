#ifndef HORDDT_CV_HORDDT_RESIZE_HPP_
#define HORDDT_CV_HORDDT_RESIZE_HPP_

#include <cstdint>
#include <memory>
#include <string>

extern "C" {
#include "hb_mem_mgr.h"
}

class horddt_resize {
public:
    struct config {
        std::uint32_t input_width = 0;
        std::uint32_t input_height = 0;
        std::uint32_t output_width = 0;
        std::uint32_t output_height = 0;
        int timeout_ms = 2000;
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
