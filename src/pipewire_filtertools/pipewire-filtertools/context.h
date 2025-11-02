/* SPDX-License-Identifier: MIT */

#pragma once

#include <spa/param/audio/format-utils.h>

#include "pipewire-filtertools.h"

struct pfts_port {
    struct pfts_data *data;
};

struct pfts_data {
    char *inp_name; /* Input stream */
    char *name;     /* Filter */
    char *src_name; /* Output source */

    uint32_t inp_id;
    uint32_t id;
    uint32_t src_id;

    uint32_t inp_out_port_id; /* Input stream's port to the filter */
    uint32_t in_port_id;      /* Filter's port from the input stream */
    uint32_t out_port_id;     /* Filter's port to the output source */
    uint32_t src_in_port_id;  /* Source's port from the filter */

    struct pw_proxy *inp_link; /* Link from input stream to the filter */
    struct pw_proxy *src_link; /* Link from filter to the output source */

    uint64_t src_serial; /* Source's serial for other streams to target */

    struct pw_main_loop *loop;
    struct pw_filter *filter;
    struct pfts_port *in_port;
    struct pfts_port *out_port;

    struct spa_audio_info format;
    uint32_t rate;
    uint32_t quantum;

    struct pw_core *core;

    struct pw_registry *registry;
    struct spa_hook registry_listener;

    struct pw_metadata *metadata;
    struct spa_hook metadata_listener;

    pfts_on_process on_process_cb;
    void *user_ctx;
};
