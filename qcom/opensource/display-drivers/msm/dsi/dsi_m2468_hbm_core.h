/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_M2468_HBM_CORE_H
#define DSI_M2468_HBM_CORE_H
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>

enum m2468_hbm_phase {
	M2468_HBM_OFF, M2468_HBM_ENABLING, M2468_HBM_ON,
	M2468_HBM_DISABLING, M2468_HBM_FAULT,
};
/* Internal command IDs, deliberately independent of the stock enum. */
enum m2468_hbm_command {
	M2468_LOCAL_ON, M2468_LOCAL_OFF, M2468_LEVEL_ON, M2468_LEVEL_OFF,
	M2468_ADFR, M2468_RESTORE,
};
struct m2468_hbm_state {
	enum m2468_hbm_phase phase;
	u64 generation;
	u32 requested, attempted, successful, dc_min;
	bool attempted_valid, successful_valid;
	u32 adfr, saved_adfr;
	bool adfr_valid;
};
struct m2468_hbm_ops {
	int (*send)(void *ctx, enum m2468_hbm_command command, u32 value);
	int (*restore)(void *ctx);
	void (*wait_ms)(void *ctx, u32 ms);
};
int m2468_hbm_rgb(u32 brightness, bool on, u8 rgb[3]);
void m2468_hbm_demura(u32 attempted, u32 requested, u32 dc_min, u8 payload[4]);
int m2468_hbm_delays(u32 hz, u32 *before, u32 *after);
int m2468_hbm_adfr_byte(u32 code);
int m2468_hbm_run(struct m2468_hbm_state *s, bool on, u32 hz, bool use_adfr,
		const struct m2468_hbm_ops *ops, void *ctx);
void m2468_hbm_invalidate(struct m2468_hbm_state *s, bool reset_complete);
bool m2468_hbm_ready_matches(const struct m2468_hbm_state *s, u64 generation);
#endif
