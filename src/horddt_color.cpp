/***************************************************************************
 * NV12/NV21 color conversion implemented with libyuv.
 ***************************************************************************/

#include "horddt_color.hpp"

#include <libyuv/convert_argb.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

int validate_input_buffer(const hb_mem_graphic_buf_t &buffer)
{
    if (buffer.plane_cnt < 2 ||
        (buffer.format != MEM_PIX_FMT_NV12 &&
         buffer.format != MEM_PIX_FMT_NV21)) {
        std::fprintf(stderr,
                     "Unexpected input buffer layout: plane_cnt=%d, "
                     "format=%d; expected NV12(%d) or NV21(%d).\n",
                     buffer.plane_cnt,
                     buffer.format,
                     MEM_PIX_FMT_NV12,
                     MEM_PIX_FMT_NV21);
        return -1;
    }

    if (buffer.width <= 0 || buffer.height <= 0 ||
        (buffer.width & 1) != 0 || (buffer.height & 1) != 0 ||
        buffer.stride < buffer.width || buffer.vstride < buffer.height ||
        buffer.virt_addr[0] == nullptr || buffer.virt_addr[1] == nullptr) {
        std::fprintf(stderr,
                     "Invalid NV12/NV21 buffer: size=%dx%d, stride=%d, "
                     "vstride=%d, y=%p, uv=%p.\n",
                     buffer.width,
                     buffer.height,
                     buffer.stride,
                     buffer.vstride,
                     buffer.virt_addr[0],
                     buffer.virt_addr[1]);
        return -1;
    }

    if (buffer.width > std::numeric_limits<int>::max() / 3) {
        std::fprintf(stderr, "Input width is too large for RGB24 output.\n");
        return -1;
    }

    const std::uint64_t y_required =
        static_cast<std::uint64_t>(buffer.height - 1) *
            static_cast<std::uint64_t>(buffer.stride) +
        static_cast<std::uint64_t>(buffer.width);
    const std::uint64_t uv_required =
        static_cast<std::uint64_t>(buffer.height / 2 - 1) *
            static_cast<std::uint64_t>(buffer.stride) +
        static_cast<std::uint64_t>(buffer.width);

    if (buffer.size[0] < y_required || buffer.size[1] < uv_required) {
        std::fprintf(stderr,
                     "Input buffer planes are too small: Y=%llu/%llu, "
                     "UV=%llu/%llu bytes.\n",
                     static_cast<unsigned long long>(buffer.size[0]),
                     static_cast<unsigned long long>(y_required),
                     static_cast<unsigned long long>(buffer.size[1]),
                     static_cast<unsigned long long>(uv_required));
        return -1;
    }

    return 0;
}

void swap_red_blue(cv::Mat &image)
{
    for (int row = 0; row < image.rows; ++row) {
        std::uint8_t *pixels = image.ptr<std::uint8_t>(row);
        for (int column = 0; column < image.cols; ++column) {
            const std::size_t offset =
                static_cast<std::size_t>(column) * 3U;
            const std::uint8_t blue = pixels[offset];
            pixels[offset] = pixels[offset + 2U];
            pixels[offset + 2U] = blue;
        }
    }
}

int convert_with_libyuv(const hb_mem_graphic_buf_t &input,
                        cv::Mat &output,
                        horddt_color::output_format format)
{
    const auto *source_y =
        static_cast<const std::uint8_t *>(input.virt_addr[0]);
    const auto *source_uv =
        static_cast<const std::uint8_t *>(input.virt_addr[1]);
    auto *destination = output.ptr<std::uint8_t>(0);

    if (output.step[0] >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        std::fprintf(stderr, "RGB output stride is too large for libyuv.\n");
        return -1;
    }

    const int destination_stride = static_cast<int>(output.step[0]);
    int ret = 0;

    // libyuv's RGB24 output is B,G,R byte order in memory. Use the RGB24
    // APIs for both output modes and swap R/B when the caller requests RGB.
    if (input.format == MEM_PIX_FMT_NV12) {
        ret = libyuv::NV12ToRGB24(
            source_y, input.stride,
            source_uv, input.stride,
            destination, destination_stride,
            input.width, input.height);
    } else {
        ret = libyuv::NV21ToRGB24(
            source_y, input.stride,
            source_uv, input.stride,
            destination, destination_stride,
            input.width, input.height);
    }

    if (ret == 0 && format == horddt_color::output_format::rgb) {
        swap_red_blue(output);
    }
    return ret;
}

}  // namespace

struct horddt_color::impl {
    int initialization_result = 0;
    bool initialized = true;
};

horddt_color::horddt_color()
    : implementation_(new impl)
{
}

horddt_color::~horddt_color() = default;

bool horddt_color::is_initialized() const noexcept
{
    return implementation_->initialized;
}

int horddt_color::initialization_status() const noexcept
{
    return implementation_->initialization_result;
}

int horddt_color::convert(const hb_mem_graphic_buf_t &input_buffer,
                          cv::Mat &output_image,
                          output_format format) const
{
    if (!implementation_->initialized) {
        output_image.release();
        return implementation_->initialization_result;
    }

    int ret = validate_input_buffer(input_buffer);
    if (ret != 0) {
        output_image.release();
        return ret;
    }

    try {
        output_image.create(input_buffer.height,
                            input_buffer.width,
                            CV_8UC3);
    } catch (const cv::Exception &exception) {
        std::fprintf(stderr, "Failed to allocate RGB output: %s\n",
                     exception.what());
        output_image.release();
        return -1;
    }

    if (output_image.empty() || output_image.type() != CV_8UC3) {
        std::fprintf(stderr, "Failed to create RGB output image.\n");
        output_image.release();
        return -1;
    }

    ret = convert_with_libyuv(input_buffer, output_image, format);
    if (ret != 0) {
        std::fprintf(stderr, "libyuv color conversion failed: %d\n", ret);
        output_image.release();
        return ret;
    }

    return 0;
}
