/**
 * thermal_model.h — Extended Lumped-Parameter Thermal Model Interface
 */

#ifndef THERMAL_MODEL_H
#define THERMAL_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include "thermasched.h"

#define THERMAL_DT_S  0.1f   /* Scheduling epoch duration (s) = 100 ms */
#define THERMAL_N_TILES  4

typedef struct {
    float C_j[4];    /* Thermal capacitances (J/°C): [1.42, 1.38, 1.35, 1.41] */
    float G_j[4];    /* Vertical conductances (W/°C): [0.302, 0.383, 0.365, 0.320] */
    float tau_j[4];  /* Time constants (s): [4.70, 3.60, 3.70, 4.40] */
    float G_jk[3];   /* Adjacent lateral: [G_01=0.042, G_12=0.038, G_23=0.045] */
    float C_jl[2];   /* Non-adjacent: [C_02=0.012, C_13=0.011] (~28-32% of G_jk) */
} thermal_params_t;

typedef struct {
    thermal_params_t params;
    float tau_min_s;
    bool  euler_stable;
} thermal_model_t;

void  thermal_model_init(thermal_model_t *model, const thermal_params_t *params);
void  thermal_predict(const thermal_model_t *model, const tile_state_t *tiles,
                      float ambient_c, float dt_s, float *theta_hat);
float thermal_estimate_power(uint32_t freq_mhz, float utilization);
float thermal_workload_delta(const thermal_model_t *model,
                              const layer_desc_t *layer, int tile);
int   thermal_load_params(const char *path, thermal_params_t *params);
void  thermal_model_print_params(const thermal_model_t *model);

#endif /* THERMAL_MODEL_H */
