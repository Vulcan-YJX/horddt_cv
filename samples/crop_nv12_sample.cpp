#include "horddt_crop.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kInputWidth = 1920U;
constexpr std::uint32_t kInputHeight = 1080U;
constexpr std::uint32_t kHalfWidth = kInputWidth / 2U;
constexpr const char *kDefaultInput = "data/input_1920x1080.nv12";
constexpr const char *kDefaultLeft = "crop_left_960x1080.nv12";
constexpr const char *kDefaultRight = "crop_right_960x1080.nv12";

void print_usage(const char *program)
{
    std::fprintf(
        stderr,
        "Usage: %s [input.nv12 left.nv12 right.nv12]\n"
        "  Uses two PYM SRC ROI channels to split one 1920x1080 NV12 frame "
        "at its vertical center.\n"
        "  Defaults: %s -> %s + %s\n",
        program, kDefaultInput, kDefaultLeft, kDefaultRight);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 &&
        (std::strcmp(argv[1], "-h") == 0 ||
         std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc != 1 && argc != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string input = argc == 4 ? argv[1] : kDefaultInput;
    const std::vector<std::string> outputs = {
        argc == 4 ? argv[2] : kDefaultLeft,
        argc == 4 ? argv[3] : kDefaultRight,
    };

    horddt_crop::config cfg;
    cfg.input_width = kInputWidth;
    cfg.input_height = kInputHeight;
    cfg.regions = {
        {0U, 0U, kHalfWidth, kInputHeight},
        {kHalfWidth, 0U, kHalfWidth, kInputHeight},
    };
    cfg.verbose = true;

    horddt_crop cropper(cfg);
    if (!cropper.is_initialized()) {
        std::fprintf(stderr, "horddt_crop initialization failed: %d\n",
                     cropper.initialization_status());
        return EXIT_FAILURE;
    }

    const int ret = cropper.crop(input, outputs);
    if (ret != 0) {
        std::fprintf(stderr, "Hardware crop failed: %d\n", ret);
        return EXIT_FAILURE;
    }

    std::printf("Crop complete:\n  left : %s (960x1080)\n"
                "  right: %s (960x1080)\n",
                outputs[0].c_str(), outputs[1].c_str());
    return EXIT_SUCCESS;
}
