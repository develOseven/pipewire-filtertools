/* SPDX-License-Identifier: MIT */
#include <stdint.h>

typedef void (*pfts_on_buffer)(void *ctx, float *samples, uint32_t n_samples);

void pfts_init(int *argc, char **argv[]);
uint32_t pfts_get_rate();
void* pfts_main_loop_new();
void pfts_main_loop_run(void *ctx, void *loop, uint32_t rate, const uint32_t quantum, pfts_on_buffer on_capture, pfts_on_buffer on_playback);
void pfts_main_loop_quit(void *loop);
void pfts_deinit();
