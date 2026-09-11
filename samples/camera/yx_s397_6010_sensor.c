/***************************************************************************
 * Minimal yx_s397_6010 VIN configuration extracted from autocube_media.
 * It intentionally contains only the Camera/Deserializer/VIN fields needed
 * by realtime_vin_camera_pipeline and does not depend on autocube_media.
 ***************************************************************************/

#include "yx_s397_6010_sensor.h"

#define SENSOR_WIDTH 1088
#define SENSOR_HEIGHT 2560

static camera_config_t camera_config = {
    .name = "yx_s397_6010",
    .addr = 0x0,
    .serial_addr = 0x40,
    .sensor_mode = 0x5,
    .fps = 30,
    .width = SENSOR_WIDTH,
    .height = SENSOR_HEIGHT,
    .extra_mode = 0,
    .config_index = 1024,
    .end_flag = CAMERA_CONFIG_END_FLAG,
    .calib_lname = "disable",
};

static poc_config_t poc_config[] = {
    {
        .addr = 0x28,
        .poc_map = 0x1320,
        .end_flag = POC_CONFIG_END_FLAG,
    },
};

static deserial_config_t deserial_config = {
    .name = "max96712",
    .link_desp[0] = "yx_s397_6010:0@1024",
    .addr = 0x29,
    .poc_cfg = &poc_config[0],
    .gpio_mfp[CAMERA_DES_GPIO_TRIG0] = 5,
    .end_flag = DESERIAL_CONFIG_END_FLAG,
};

static vin_ichn_attr_t vin_ichn_attr = {
    .width = SENSOR_WIDTH,
    .height = SENSOR_HEIGHT,
    .format = 0x1e,
};

static vin_ochn_attr_t vin_ochn_attr = {
    .ddr_en = 1,
    .vin_basic_attr =
        {
            .format = 0x1e,
            .wstride = 0,
            .vstride = 0,
            .pack_mode = 1,
        },
    .pingpong_ring = 1,
    .roi_en = 0,
    .roi_attr =
        {
            .roi_x = 1280,
            .roi_y = 720,
            .roi_width = 64,
            .roi_height = 64,
        },
    .rawds_en = 0,
    .rawds_attr =
        {
            .rawds_mode = 0,
        },
    .magicNumber = 0x12345678,
};

static vin_node_attr_t vin_node_attr = {
    .vcon_attr =
        {
            .bus_main = 3,
            .bus_second = 3,
        },
    .cim_attr =
        {
            .mipi_en = 1,
            .cim_isp_flyby = 0,
            .cim_pym_flyby = 0,
            .mipi_rx = 4,
            .vc_index = 0,
            .ipi_channels = 1,
            .y_uv_swap = 0,
            .func =
                {
                    .enable_frame_id = 1,
                    .set_init_frame_id = 1,
                    .enable_pattern = 0,
                    .lpwm_trig_sel = 2,
                },
            .rdma_input =
                {
                    .rdma_en = 0,
                    .stride = 0,
                    .pack_mode = 1,
                    .buff_num = 6,
                },
        },
    .lpwm_attr =
        {
            .lpwm_chn_attr =
                {
                    {
                        .enable = 1,
                        .trigger_source = 0,
                        .trigger_mode = 0,
                        .period = 33333,
                        .offset = 11,
                        .duty_time = 3333,
                        .threshold = 0,
                        .adjust_step = 0,
                    },
                },
        },
};

horddt_vin_sensor_config_t horddt_yx_s397_6010_linear_1088x2560_yuv422_30fps = {
    .camera_config = &camera_config,
    .deserial_config = &deserial_config,
    .vin_node_attr = &vin_node_attr,
    .vin_ichn_attr = &vin_ichn_attr,
    .vin_ochn_attr = &vin_ochn_attr,
};
