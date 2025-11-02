/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdint.h>

typedef void (*pfts_on_process)(void *ctx,
                                float *in,
                                float *out,
                                uint32_t n_samples);

void pfts_init(int *argc, char **argv[]);
uint32_t pfts_get_rate();
void* pfts_main_loop_new();
int pfts_main_loop_run(void *ctx,
                       void *loop,
                       const char *name,
                       uint32_t rate,
                       const uint32_t quantum,
                       pfts_on_process on_processs);
int pfts_main_loop_quit(void *loop);
void pfts_deinit();
