cmake_minimum_required(VERSION 3.10)

project(horddt_cv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(HOBOT_ROOT "/usr/hobot" CACHE PATH "D-Robotics multimedia SDK root")
set(LIBYUV_ROOT "${HOBOT_ROOT}" CACHE PATH "libyuv installation root")
option(HORDDT_BUILD_COLOR
       "Build the optional libyuv color converter and JPEG samples" ON)

find_path(HOBOT_INCLUDE_DIR
    NAMES hbn_vpf_interface.h hb_mem_mgr.h
    PATHS "${HOBOT_ROOT}/include"
    NO_DEFAULT_PATH
)

if(NOT HOBOT_INCLUDE_DIR)
    message(FATAL_ERROR
        "D-Robotics multimedia SDK headers were not found under "
        "${HOBOT_ROOT}/include. Set HOBOT_ROOT to the SDK installation path."
    )
endif()

find_path(GDC_INCLUDE_DIR
    NAMES gdc_cfg.h gdc_bin_cfg.h
    HINTS
        "${HOBOT_ROOT}/include"
        "${HOBOT_INCLUDE_DIR}"
)
if(NOT GDC_INCLUDE_DIR)
    message(FATAL_ERROR
        "The D-Robotics GDC headers gdc_cfg.h/gdc_bin_cfg.h were not found. "
        "Install the multimedia GDC development headers or set HOBOT_ROOT."
    )
endif()

foreach(lib_name IN ITEMS vpf vio cam hbmem)
    string(TOUPPER "${lib_name}" lib_name_upper)
    find_library(HOBOT_${lib_name_upper}_LIBRARY
        NAMES "${lib_name}"
        PATHS "${HOBOT_ROOT}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT HOBOT_${lib_name_upper}_LIBRARY)
        message(FATAL_ERROR
            "D-Robotics library lib${lib_name}.so was not found under "
            "${HOBOT_ROOT}/lib. Set HOBOT_ROOT to the SDK installation path."
        )
    endif()
endforeach()

set(HOBOT_LIBRARIES
    "${HOBOT_VPF_LIBRARY}"
    "${HOBOT_VIO_LIBRARY}"
    "${HOBOT_CAM_LIBRARY}"
    "${HOBOT_HBMEM_LIBRARY}"
)

# Hardware JPEG/H.264 codec API used by multimedia_samples/sample_codec.
find_path(MEDIA_CODEC_INCLUDE_DIR
    NAMES hb_media_codec.h
    HINTS
        "${HOBOT_ROOT}/include"
        "${HOBOT_INCLUDE_DIR}"
)
if(NOT MEDIA_CODEC_INCLUDE_DIR OR
   NOT EXISTS "${MEDIA_CODEC_INCLUDE_DIR}/hb_media_error.h")
    message(FATAL_ERROR
        "D-Robotics Media Codec headers hb_media_codec.h/hb_media_error.h "
        "were not found. Set HOBOT_ROOT to the SDK installation path."
    )
endif()

find_library(HOBOT_MULTIMEDIA_LIBRARY
    NAMES multimedia
    HINTS
        "${HOBOT_ROOT}/lib"
        "${HOBOT_ROOT}/lib/aarch64-linux-gnu"
)
if(NOT HOBOT_MULTIMEDIA_LIBRARY)
    message(FATAL_ERROR
        "D-Robotics library libmultimedia.so was not found. Set HOBOT_ROOT "
        "to the SDK installation path."
    )
endif()

add_library(horddt_resize STATIC src/horddt_resize.cpp)
target_include_directories(horddt_resize
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${HOBOT_INCLUDE_DIR}"
)
target_compile_options(horddt_resize PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(horddt_resize PUBLIC ${HOBOT_LIBRARIES})

# GDC distortion correction. The implementation uses the standalone feedback
# mode from sample_gdc and the split GDC attributes from sample_pipeline.
add_library(horddt_remap STATIC src/horddt_remap.cpp)
target_include_directories(horddt_remap
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${HOBOT_INCLUDE_DIR}"
        "${GDC_INCLUDE_DIR}"
)
target_compile_options(horddt_remap PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(horddt_remap PUBLIC ${HOBOT_LIBRARIES})

add_executable(remap_nv12 samples/remap_nv12_sample.cpp)
target_compile_options(remap_nv12 PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(remap_nv12 PRIVATE horddt_remap)


add_library(horddt_codec STATIC src/horddt_codec.cpp)
target_include_directories(horddt_codec
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${HOBOT_INCLUDE_DIR}"
        "${MEDIA_CODEC_INCLUDE_DIR}"
)
target_compile_options(horddt_codec PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(horddt_codec
    PUBLIC
        "${HOBOT_HBMEM_LIBRARY}"
        "${HOBOT_MULTIMEDIA_LIBRARY}"
        Threads::Threads
)

add_executable(codec_sample samples/codec_sample.cpp)
target_compile_options(codec_sample PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(codec_sample PRIVATE horddt_codec)

# horddt_color uses libyuv for NV12/NV21 to packed BGR/RGB conversion.
# OpenCV is retained only for the cv::Mat API and JPEG writing in samples.
if(HORDDT_BUILD_COLOR)
    find_path(LIBYUV_INCLUDE_DIR
        NAMES libyuv/convert_argb.h
        HINTS
            "${LIBYUV_ROOT}/include"
            "${HOBOT_ROOT}/include"
            "/usr/local/include"
            "/usr/include"
    )
    find_library(LIBYUV_LIBRARY
        NAMES yuv
        HINTS
            "${LIBYUV_ROOT}/lib"
            "${LIBYUV_ROOT}/lib/aarch64-linux-gnu"
            "${HOBOT_ROOT}/lib"
            "${HOBOT_ROOT}/lib/aarch64-linux-gnu"
            "/usr/local/lib"
            "/usr/lib"
            "/usr/lib/aarch64-linux-gnu"
    )
    find_package(OpenCV QUIET COMPONENTS core imgcodecs)

    if(LIBYUV_INCLUDE_DIR AND LIBYUV_LIBRARY AND OpenCV_FOUND)
        add_library(horddt_color STATIC src/horddt_color.cpp)
        target_include_directories(horddt_color
            PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${HOBOT_INCLUDE_DIR}"
                "${LIBYUV_INCLUDE_DIR}"
                ${OpenCV_INCLUDE_DIRS}
        )
        target_compile_options(horddt_color PRIVATE
            -O2 -Wall -Wextra -Werror)
        target_link_libraries(horddt_color
            PUBLIC
                "${HOBOT_HBMEM_LIBRARY}"
                "${LIBYUV_LIBRARY}"
                ${OpenCV_LIBS}
        )

        add_executable(resize_nv12 samples/resize_nv12_sample.cpp)
        target_include_directories(resize_nv12 PRIVATE ${OpenCV_INCLUDE_DIRS})
        target_compile_options(resize_nv12 PRIVATE
            -O2 -Wall -Wextra -Werror)
        target_link_libraries(resize_nv12 PRIVATE
            horddt_resize horddt_color ${OpenCV_LIBS})

        add_executable(color_convert samples/color_convert_sample.cpp)
        target_include_directories(color_convert PRIVATE ${OpenCV_INCLUDE_DIRS})
        target_compile_options(color_convert PRIVATE
            -O2 -Wall -Wextra -Werror)
        target_link_libraries(color_convert PRIVATE
            horddt_color ${OpenCV_LIBS})
    else()
        message(WARNING
            "libyuv and/or OpenCV development files were not found. "
            "Skipping horddt_color, resize_nv12, and color_convert; "
            "horddt_resize, horddt_remap, codec_sample, and remap_nv12 "
            "remain available. Set LIBYUV_ROOT if libyuv is installed "
            "outside the default paths."
        )
    endif()
endif()
