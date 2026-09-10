#ifndef HORDDT_CV_HORDDT_COLOR_HPP_
#define HORDDT_CV_HORDDT_COLOR_HPP_

#include <memory>

#include <opencv2/core.hpp>

extern "C" {
#include "hb_mem_mgr.h"
}

// CPU color converter implemented with libyuv. OpenCV only owns the resulting
// CV_8UC3 image and is not used for color-space conversion.
class horddt_color {
public:
    enum class output_format {
        bgr,
        rgb,
    };

    // libyuv has no runtime context, so construction completes immediately.
    horddt_color();
    ~horddt_color();

    horddt_color(const horddt_color &) = delete;
    horddt_color &operator=(const horddt_color &) = delete;

    bool is_initialized() const noexcept;
    int initialization_status() const noexcept;

    // Converts a caller-owned NV12/NV21 hbmem buffer with libyuv. Input planes
    // need CPU-accessible virtual addresses but do not need to be physically
    // contiguous. The output is an owning CV_8UC3 BGR or RGB cv::Mat.
    int convert(const hb_mem_graphic_buf_t &input_buffer,
                cv::Mat &output_image,
                output_format format = output_format::bgr) const;

private:
    struct impl;
    std::unique_ptr<impl> implementation_;
};

#endif  // HORDDT_CV_HORDDT_COLOR_HPP_
