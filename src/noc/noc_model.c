/**
 * noc_model.c — Contention-Aware AXI4 NoC Cost Model
 *
 * Implements the S_comm scoring term from Eq. (3).
 * The AXI4-Stream crossbar uses a fixed-priority round-robin arbiter
 * (T0 highest priority). Bandwidth degrades under concurrent streams
 * due to head-of-line blocking.
 *
 * Measurement-derived lookup table (empirical, ZCU102 board):
 *   N_active=1: 100% BW (4.8 GB/s)
 *   N_active=2:  88% BW (4.2 GB/s)
 *   N_active=3:  71% BW (3.4 GB/s)  ← head-of-line blocking begins
 *   N_active=4:  58% BW (2.8 GB/s)  ← worst-case concurrent
 *
 * Directional asymmetry (tile-to-DDR vs tile-to-tile) accounts for <4%
 * additional variance beyond N_active — justified for scheduling accuracy.
 *
 * Reference: ThermaSched §3.3 (Contention-Aware AXI4 NoC Fabric)
 */

#include "noc_model.h"

static const float BW_FRACTION[5] = {0.0f, 1.00f, 0.88f, 0.71f, 0.58f};

void noc_model_init(noc_model_t *model, const float *bw_table, float peak_mbs)
{
    for (int i = 0; i <= 4; i++)
        model->bw_fraction[i] = bw_table[i];
    model->bw_peak_mbs = peak_mbs;
}

float noc_compute_s_comm(const noc_model_t *model,
                          uint32_t weight_bytes, int n_active)
{
    if (n_active < 1) n_active = 1;
    if (n_active > 4) n_active = 4;

    float bw_avail = model->bw_peak_mbs * model->bw_fraction[n_active];
    /* Weight fetch time at available bandwidth (ms) */
    float fetch_ms = ((float)weight_bytes / (1024.0f * 1024.0f))
                     / bw_avail * 1000.0f;
    /* Normalize: S_comm = 1 - (fetch_time / epoch_time) */
    float s_comm = 1.0f - fetch_ms / 100.0f;  /* 100 ms epoch */
    if (s_comm < 0.0f) s_comm = 0.0f;
    if (s_comm > 1.0f) s_comm = 1.0f;
    return s_comm;
}
