/**
 * dvfs_ctrl.c — Proactive DVFS Controller
 *
 * Controls per-tile clock frequencies via MMCM fractional-divider mode.
 * Frequency changes complete in <4 µs without gating the output clock.
 *
 * Supported frequencies: 100, 125, 150, 175, 200, 225, 250, 275, 300 MHz
 * (25 MHz steps via pre-loaded MMCM divider settings)
 *
 * DVFS events in 5-model benchmark at 25°C ambient:
 *   - 34% of epochs trigger at least one DVFS step-down
 *   - T3 accounts for 61% of all DVFS events (hottest tile)
 *   - Bias cost: +0.4°C overestimation causes ~1.3-1.7% unnecessary DVFS
 *
 * Reference: ThermaSched §4.4 (Proactive DVFS and Latency Budget)
 */

#include "dvfs_ctrl.h"

/* Pre-computed MMCM divider settings for 9 supported frequencies.
 * Fractional-divider mode: no clock gating, <4 µs lock re-acquisition.
 * Reference: Xilinx PG065 (MMCM/PLL for UltraScale+ FPGAs) */
static const mmcm_dividers_t FREQ_TABLE[9] = {
    {.freq_mhz=100, .d=1, .m=10.000f, .o=10}, /* VCO=1000 MHz */
    {.freq_mhz=125, .d=1, .m=12.500f, .o=10}, /* VCO=1250 MHz */
    {.freq_mhz=150, .d=1, .m=15.000f, .o=10}, /* VCO=1500 MHz */
    {.freq_mhz=175, .d=1, .m=10.500f, .o= 6}, /* VCO=1050 MHz */
    {.freq_mhz=200, .d=1, .m=12.000f, .o= 6}, /* VCO=1200 MHz */
    {.freq_mhz=225, .d=1, .m=13.500f, .o= 6}, /* VCO=1350 MHz */
    {.freq_mhz=250, .d=1, .m=15.000f, .o= 6}, /* VCO=1500 MHz */
    {.freq_mhz=275, .d=1, .m=11.000f, .o= 4}, /* VCO=1100 MHz */
    {.freq_mhz=300, .d=1, .m=12.000f, .o= 4}, /* VCO=1200 MHz */
};

void dvfs_init(dvfs_controller_t *ctrl,
               uint32_t freq_min_mhz, uint32_t freq_max_mhz,
               uint32_t step_mhz)
{
    ctrl->freq_min_mhz = freq_min_mhz;
    ctrl->freq_max_mhz = freq_max_mhz;
    ctrl->step_mhz     = step_mhz;
    ctrl->active_last_epoch = false;
    for (int j = 0; j < 4; j++) {
        ctrl->current_freq[j] = freq_max_mhz;
        ctrl->event_count[j]  = 0;
    }
}

int dvfs_set_freq(dvfs_controller_t *ctrl, int tile, uint32_t freq_mhz)
{
    if (freq_mhz < ctrl->freq_min_mhz) freq_mhz = ctrl->freq_min_mhz;
    if (freq_mhz > ctrl->freq_max_mhz) freq_mhz = ctrl->freq_max_mhz;

    /* Find matching entry in FREQ_TABLE */
    const mmcm_dividers_t *div = NULL;
    for (int i = 0; i < 9; i++) {
        if (FREQ_TABLE[i].freq_mhz == freq_mhz) {
            div = &FREQ_TABLE[i];
            break;
        }
    }
    if (!div) return -1;  /* Unsupported frequency */

    /* Write MMCM DRP registers via PS MMIO
     * (Actual register addresses depend on Vivado implementation) */
    mmcm_drp_write(tile, div->d, div->m, div->o);

    ctrl->current_freq[tile] = freq_mhz;
    ctrl->event_count[tile]++;
    return 0;
}
