#ifndef LVDS_CALIBRATION_H
#define LVDS_CALIBRATION_H

#include "xil_types.h"

struct ad9361_rf_phy;

typedef struct {
    u8 valid;
    u8 rx_clock_delay;
    u8 rx_data_delay;
    u8 fpga_tap[7];
    u32 final_samples;
    u32 final_errors;
} lvds_cal_result_t;

int lvds_rx_auto_calibrate(struct ad9361_rf_phy *phy,
                           lvds_cal_result_t *result,
                           int verbose);

#endif

