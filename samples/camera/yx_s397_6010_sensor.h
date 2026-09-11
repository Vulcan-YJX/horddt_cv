#ifndef HORDDT_CV_SAMPLES_CAMERA_YX_S397_6010_SENSOR_H_
#define HORDDT_CV_SAMPLES_CAMERA_YX_S397_6010_SENSOR_H_

#include "vin_cfg.h"
#include "hb_camera_data_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct horddt_vin_sensor_config_s {
    camera_config_t *camera_config;
    deserial_config_t *deserial_config;
    vin_node_attr_t *vin_node_attr;
    vin_ichn_attr_t *vin_ichn_attr;
    vin_ochn_attr_t *vin_ochn_attr;
} horddt_vin_sensor_config_t;

/*
 * Fixed configuration used by autocube_media's StereoCameraReader:
 * yx_s397_6010, 1088x2560 YUV422 at 30 fps, MAX96712, MIPI RX 4.
 */
extern horddt_vin_sensor_config_t
    horddt_yx_s397_6010_linear_1088x2560_yuv422_30fps;

#ifdef __cplusplus
}
#endif

#endif // HORDDT_CV_SAMPLES_CAMERA_YX_S397_6010_SENSOR_H_
