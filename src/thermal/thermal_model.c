/**
 * thermal_model.c — Extended Lumped-Parameter Thermal Model
 *
 * Implements Equation (2) from the paper:
 *
 *   θ̂_j(t+Δt) = θ_j + (Δt/C_j) * [
 *       P_j
 *       - G_j*(θ_j - θ_amb(t))              [vertical: die→ambient]
 *       - Σ_{k∈adj(j)} G_jk*(θ_j - θ_k)    [adjacent lateral coupling]
 *       - Σ_{l∈non-adj(j)} C_jl*(θ_j - θ_l) [non-adjacent: ~28% of G_jk]
 *   ]
 *
 * Forward-Euler explicit integration with Δt = 100 ms.
 * Stability: Δt = 0.1 s << 2*τ_min = 7.2 s [PASS].
 *
 * Calibrated parameters (Table 1, paper):
 *   C_j  = [1.42, 1.38, 1.35, 1.41] J/°C
 *   G_j  = [0.302, 0.383, 0.365, 0.320] W/°C
 *   G_jk = [G_01=0.042, G_12=0.038, G_23=0.045] W/°C (asymmetric)
 *   C_jl = [C_02=0.012, C_13=0.011] W/°C (non-adjacent, ~28-32% of G_jk)
 */

#include "thermal_model.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* Adjacency table for 4-tile linear array:
 *   adj[j]     = list of adjacent tile indices
 *   non_adj[j] = list of non-adjacent tile indices (|i-j|=2) */
static const int ADJ[4][2]     = {{1,-1}, {0,2}, {1,3}, {2,-1}};
static const int ADJ_CNT[4]    = {1, 2, 2, 1};
static const int NON_ADJ[4][1] = {{2}, {3}, {0}, {1}};
static const int NON_ADJ_CNT[4]= {1, 1, 1, 1};

/* G_jk index mapping for the 3 adjacent pairs (T0-T1, T1-T2, T2-T3) */
static inline float get_gjk(const thermal_params_t *p, int j, int k)
{
    if ((j==0 && k==1) || (j==1 && k==0)) return p->G_jk[0]; /* G_01 */
    if ((j==1 && k==2) || (j==2 && k==1)) return p->G_jk[1]; /* G_12 */
    if ((j==2 && k==3) || (j==3 && k==2)) return p->G_jk[2]; /* G_23 */
    return 0.0f;
}

/* C_jl index mapping for the 2 non-adjacent pairs (T0-T2, T1-T3) */
static inline float get_cjl(const thermal_params_t *p, int j, int l)
{
    if ((j==0 && l==2) || (j==2 && l==0)) return p->C_jl[0]; /* C_02 */
    if ((j==1 && l==3) || (j==3 && l==1)) return p->C_jl[1]; /* C_13 */
    return 0.0f;
}

void thermal_model_init(thermal_model_t *model, const thermal_params_t *params)
{
    memcpy(&model->params, params, sizeof(thermal_params_t));
    /* Verify Euler stability: dt ≤ 2*tau_min */
    float tau_min = model->params.tau_j[0];
    for (int j = 1; j < 4; j++) {
        if (model->params.tau_j[j] < tau_min)
            tau_min = model->params.tau_j[j];
    }
    model->tau_min_s = tau_min;
    model->euler_stable = (THERMAL_DT_S <= 2.0f * tau_min);
}

void thermal_predict(const thermal_model_t *model,
                     const tile_state_t *tiles,
                     float ambient_c, float dt_s,
                     float *theta_hat)
{
    const thermal_params_t *p = &model->params;

    for (int j = 0; j < 4; j++) {
        float theta_j = tiles[j].temp_c;

        /* Power dissipated by current tile assignment
         * Estimated from utilization × TDP fraction */
        float P_j = thermal_estimate_power(tiles[j].freq_mhz,
                                            tiles[j].utilization);

        /* Term 1: Vertical die-to-ambient conductance */
        float term_vertical = p->G_j[j] * (theta_j - ambient_c);

        /* Term 2: Adjacent lateral coupling (asymmetric G_jk) */
        float term_lateral = 0.0f;
        for (int n = 0; n < ADJ_CNT[j]; n++) {
            int k = ADJ[j][n];
            if (k < 0) continue;
            float G_jk = get_gjk(p, j, k);
            term_lateral += G_jk * (theta_j - tiles[k].temp_c);
        }

        /* Term 3: Non-adjacent coupling (C_jl ≈ 28-32% of G_jk)
         * NOTE: Although labeled "correction" for historical consistency with
         * prior lumped-parameter literature, C_jl is physically substantial
         * (28-32% of G_jk) and should be treated as a primary model term.
         * Omitting it produces 0.31°C steady-state error on T2 when T0 is
         * at max power. See §4.2 of the paper. */
        float term_nonadj = 0.0f;
        for (int n = 0; n < NON_ADJ_CNT[j]; n++) {
            int l = NON_ADJ[j][n];
            float C_jl = get_cjl(p, j, l);
            term_nonadj += C_jl * (theta_j - tiles[l].temp_c);
        }

        /* Forward-Euler update: θ̂_j(t+Δt) = θ_j + (Δt/C_j)*[...] */
        float d_theta = (dt_s / p->C_j[j]) *
                        (P_j - term_vertical - term_lateral - term_nonadj);

        theta_hat[j] = theta_j + d_theta;

        /* Clamp to physically plausible range */
        if (theta_hat[j] < ambient_c)   theta_hat[j] = ambient_c;
        if (theta_hat[j] > 120.0f)       theta_hat[j] = 120.0f;
    }
}

float thermal_estimate_power(uint32_t freq_mhz, float utilization)
{
    /* Simplified power model: P ≈ C_dynamic * f * V^2 * utilization
     * Calibrated against INA226 measurements.
     * At 300 MHz, full utilization: ~3.7 W per tile.
     * At 100 MHz, full utilization: ~0.9 W per tile (freq-cubic reduction). */
    float f_norm = (float)freq_mhz / 300.0f;
    return 3.7f * f_norm * f_norm * f_norm * utilization;
}

float thermal_workload_delta(const thermal_model_t *model,
                              const layer_desc_t *layer, int tile)
{
    /* Estimate incremental temperature rise from adding this layer
     * to the tile's assignment. Used to update theta_hat during
     * greedy assignment loop (prevents assigning too many layers to
     * a single tile within one epoch). */
    (void)model;
    float compute_fraction = (float)layer->mac_ops / 1e9f;  /* Normalized */
    float power_delta = 3.7f * compute_fraction;             /* W */
    float dt = (float)TS_EPOCH_MS / 1000.0f;                /* s */
    float C_j = model->params.C_j[tile];
    return (dt / C_j) * power_delta;
}

int thermal_load_params(const char *path, thermal_params_t *params)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    int loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char key[64];
        float v0, v1, v2, v3;
        if (sscanf(line, "%63[^,],%f,%f,%f,%f", key, &v0, &v1, &v2, &v3) == 5) {
            if (strncmp(key, "C_j", 3) == 0) {
                params->C_j[0]=v0; params->C_j[1]=v1;
                params->C_j[2]=v2; params->C_j[3]=v3;
                loaded++;
            } else if (strncmp(key, "G_j", 3) == 0) {
                params->G_j[0]=v0; params->G_j[1]=v1;
                params->G_j[2]=v2; params->G_j[3]=v3;
                loaded++;
            } else if (strncmp(key, "tau_j", 5) == 0) {
                params->tau_j[0]=v0; params->tau_j[1]=v1;
                params->tau_j[2]=v2; params->tau_j[3]=v3;
                loaded++;
            }
        }
        char key3[64];
        float u0, u1, u2;
        if (sscanf(line, "%63[^,],%f,%f,%f", key3, &u0, &u1, &u2) == 4) {
            if (strncmp(key3, "G_jk", 4) == 0) {
                params->G_jk[0]=u0; params->G_jk[1]=u1; params->G_jk[2]=u2;
                loaded++;
            }
        }
        char key2[64];
        float w0, w1;
        if (sscanf(line, "%63[^,],%f,%f", key2, &w0, &w1) == 3) {
            if (strncmp(key2, "C_jl", 4) == 0) {
                params->C_jl[0]=w0; params->C_jl[1]=w1;
                loaded++;
            }
        }
    }
    fclose(f);
    return (loaded >= 5) ? 0 : -2;
}

void thermal_model_print_params(const thermal_model_t *model)
{
    const thermal_params_t *p = &model->params;
    printf("=== Thermal Model Parameters ===\n");
    printf("  C_j  (J/C):  T0=%.3f  T1=%.3f  T2=%.3f  T3=%.3f\n",
           p->C_j[0], p->C_j[1], p->C_j[2], p->C_j[3]);
    printf("  G_j  (W/C):  T0=%.3f  T1=%.3f  T2=%.3f  T3=%.3f\n",
           p->G_j[0], p->G_j[1], p->G_j[2], p->G_j[3]);
    printf("  tau  (s):    T0=%.2f  T1=%.2f  T2=%.2f  T3=%.2f\n",
           p->tau_j[0], p->tau_j[1], p->tau_j[2], p->tau_j[3]);
    printf("  G_jk (W/C):  G01=%.3f  G12=%.3f  G23=%.3f\n",
           p->G_jk[0], p->G_jk[1], p->G_jk[2]);
    printf("  C_jl (W/C):  C02=%.3f  C13=%.3f (≈28-32%% of G_jk)\n",
           p->C_jl[0], p->C_jl[1]);
    printf("  Euler stability: dt=%.3f s, 2*tau_min=%.3f s  [%s]\n",
           THERMAL_DT_S, 2.0f * model->tau_min_s,
           model->euler_stable ? "PASS" : "FAIL");
}
