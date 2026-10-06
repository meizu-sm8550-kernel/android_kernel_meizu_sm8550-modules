/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_NOTE_HBM_CORE_H
#define DSI_NOTE_HBM_CORE_H
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>

enum note_hbm_phase {
	NOTE_HBM_OFF, NOTE_HBM_ENABLING, NOTE_HBM_ON,
	NOTE_HBM_DISABLING, NOTE_HBM_FAULT,
};
/* Internal command IDs, deliberately independent of the stock enum. */
enum note_hbm_command {
	NOTE_LOCAL_ON, NOTE_LOCAL_OFF, NOTE_LEVEL_ON, NOTE_LEVEL_OFF,
	NOTE_ADFR, NOTE_RESTORE,
};
struct note_hbm_state {
	enum note_hbm_phase phase;
	u64 generation;
	u32 requested, attempted, successful, dc_min;
	bool attempted_valid, successful_valid;
	u32 adfr, saved_adfr;
	bool adfr_valid;
};
struct note_hbm_ops {
	int (*send)(void *ctx, enum note_hbm_command command, u32 value);
	int (*restore)(void *ctx);
	void (*wait_ms)(void *ctx, u32 ms);
};
int note_hbm_rgb(u32 brightness, bool on, u8 rgb[3]);
void note_hbm_demura(u32 attempted, u32 requested, u32 dc_min, u8 payload[4]);
int note_hbm_delays(u32 hz, u32 *before, u32 *after);
int note_hbm_adfr_byte(u32 code);
int note_hbm_run(struct note_hbm_state *s, bool on, u32 hz,
		const struct note_hbm_ops *ops, void *ctx);
void note_hbm_invalidate(struct note_hbm_state *s, bool reset_complete);
bool note_hbm_ready_matches(const struct note_hbm_state *s, u64 generation);
#endif
