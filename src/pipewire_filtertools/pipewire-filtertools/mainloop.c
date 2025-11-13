/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>

#include <spa/param/latency-utils.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include "context.h"
#include "retargeting.h"

/* ---------- Processing ---------- */

static void on_process(void *userdata, struct spa_io_position *position)
{
    struct pfts_data *d = userdata;
    uint32_t n_samples = position->clock.duration;

    float *in = pw_filter_get_dsp_buffer(d->in_port, n_samples);
    float *out = pw_filter_get_dsp_buffer(d->out_port, n_samples);

    if (!in || !out)
        return;

    d->on_process_cb(d->user_ctx, in, out, n_samples);
}

static void on_param_changed(void *data,
                             void *port_data,
                             uint32_t id,
                             const struct spa_pod *param)
{
    struct pfts_data *d = data;

    if (param == NULL || id != SPA_PARAM_Format) {
        return;
    }

    if (spa_format_parse(param,
                         &d->format.media_type, &d->format.media_subtype) < 0) {
        return;
    }

    if (d->format.media_type != SPA_MEDIA_TYPE_audio ||
        d->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }

    spa_format_audio_raw_parse(param, &d->format.info.raw);

    fprintf(stdout, "[pfts] negotiated format: rate=%d channels=%d\n",
            d->format.info.raw.rate, d->format.info.raw.channels);
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
};

/* ---------- Signal handling ---------- */

static void do_quit(void *userdata, int signal_number)
{
    struct pfts_data *d = userdata;
    pw_main_loop_quit(d->loop);
}

/* ---------- API Implementation ---------- */

void pfts_init(int *argc, char **argv[])
{
    pw_init(argc, argv);
}

/* Retrieve default rate from the PipeWire context’s default quantum.
 * For simplicity, return a fixed common default if we can’t query it. */
uint32_t pfts_get_rate()
{
    /* In a full implementation, we’d query pw_context_get_object() for the graph
       or use pw_context_get_support() to get the SPA support structures.
       For simplicity, we return 48000, which is the typical PipeWire default. */
    return 48000;
}

void *pfts_main_loop_new()
{
    struct pfts_data *data = calloc(1, sizeof(struct pfts_data));
    data->loop = pw_main_loop_new(NULL);
    return data;
}

int pfts_main_loop_run(void *ctx,
                       void *d,
                       const char *name,
                       bool auto_link,
                       uint32_t rate,
                       uint32_t quantum,
                       pfts_on_process on_process)
{
    int result = 0;

    struct pfts_data *data = d;
    data->inp_id = SPA_ID_INVALID;
    data->id = SPA_ID_INVALID;
    data->src_id = SPA_ID_INVALID;
    data->inp_out_port_id = SPA_ID_INVALID;
    data->in_port_id = SPA_ID_INVALID;
    data->out_port_id = SPA_ID_INVALID;
    data->src_in_port_id = SPA_ID_INVALID;
    data->src_serial = SPA_ID_INVALID;
    data->default_src_serial = SPA_ID_INVALID;

    struct pw_context *context = NULL;
    struct pw_core *core = NULL;
    struct pw_properties* props_sink = NULL;
    struct pw_properties* props_source = NULL;
    struct pw_proxy *proxy_stream_output_sink = NULL;
    struct pw_proxy *proxy_stream_input_source = NULL;

    data->user_ctx = ctx;
    data->on_process_cb = on_process;


    const uint32_t channels = 1;
    char rate_str[16], latency_str[16];
    snprintf(rate_str, sizeof(rate_str), "1/%u", rate);
    snprintf(latency_str, sizeof(latency_str), "%u/%u", quantum, rate);

    pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGINT, do_quit, data);
    pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGTERM, do_quit, data);

    data->name = strdup(name);
    if (!data->name) {
        goto fail;
    }

    data->inp_name = malloc(strlen(name) + strlen("_inp") + 1);
    if (!data->inp_name) {
        goto fail;
    }
    memcpy(data->inp_name, name, strlen(name));
    memcpy(data->inp_name + strlen(name), "_inp", strlen("_inp") + 1);

    data->src_name = malloc(strlen(name) + strlen("_src") + 1);
    if (!data->src_name) {
        goto fail;
    }
    memcpy(data->src_name, name, strlen(name));
    memcpy(data->src_name + strlen(name), "_src", strlen("_src") + 1);

    data->auto_link = auto_link;

    context = pw_context_new(pw_main_loop_get_loop(data->loop), NULL, 0);
    if (!context) {
        goto fail;
    }

    core = pw_context_connect(context, NULL, 0);
    if (!core) {
        goto fail;
    }
    data->core = core;

    result = setup_retargeting(data, core);
    if (result < 0) {
        goto fail;
    }

    /* Create filter */
    data->filter = pw_filter_new_simple(
        pw_main_loop_get_loop(data->loop),
        "pipewire-filtertools",
        pw_properties_new(
            PW_KEY_NODE_NAME, name,
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_RATE, rate_str,
            PW_KEY_NODE_LATENCY, latency_str,
            PW_KEY_NODE_MAX_LATENCY, latency_str,
            PW_KEY_NODE_FORCE_QUANTUM, "true",
            PW_KEY_NODE_LOCK_QUANTUM, "true",
            NULL),
        &filter_events,
        data);

    if (!data->filter) {
        goto fail;
    }

    /* Create input port */
    data->in_port = pw_filter_add_port(data->filter,
        PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(struct pfts_port),
        pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "input",
            NULL),
        NULL, 0);

    if (!data->in_port) {
        goto fail;
    }

    data->in_port->data = data;

    /* Create output port */
    data->out_port = pw_filter_add_port(data->filter,
        PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(struct pfts_port),
        pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "output",
            NULL),
        NULL, 0);

    if (!data->out_port) {
        goto fail;
    }

    data->out_port->data = data;

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_process_latency_build(&b,
                    SPA_PARAM_ProcessLatency,
                    &SPA_PROCESS_LATENCY_INFO_INIT(
                            .ns = 10 * SPA_NSEC_PER_MSEC
                    ));

    result = pw_filter_connect(data->filter,
                               PW_FILTER_FLAG_RT_PROCESS,
                               params,
                               1);
    if (result < 0) {
        fprintf(stderr, "[pfts] error connecting filter\n");
        goto fail;
    }

    /* Adapters */

    props_sink = pw_properties_new(NULL, NULL);
    if (!props_sink) {
        goto fail;
    }

    pw_properties_set(props_sink, PW_KEY_NODE_NAME, data->inp_name);
    pw_properties_set(props_sink, PW_KEY_NODE_VIRTUAL, "true");
    pw_properties_set(props_sink, "factory.name", "support.null-audio-sink");
    pw_properties_set(props_sink, PW_KEY_MEDIA_CLASS, "Stream/Input/Audio");
    pw_properties_set(props_sink, PW_KEY_NODE_AUTOCONNECT, "true");
    pw_properties_set(props_sink, "audio.position", "MONO");
    pw_properties_set(props_sink, "monitor.channel-volumes", "false");
    pw_properties_set(props_sink, "monitor.passthrough", "true");

    proxy_stream_output_sink = pw_core_create_object(
        core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props_sink->dict, 0);
    if (!proxy_stream_output_sink) {
        goto fail;
    }

    pw_properties_free(props_sink);
    props_sink = NULL;

    props_source = pw_properties_new(NULL, NULL);
    if (!props_source) {
        goto fail;
    }

    pw_properties_set(props_source, PW_KEY_NODE_NAME, data->src_name);
    pw_properties_set(props_source, PW_KEY_NODE_VIRTUAL, "true");
    pw_properties_set(props_source, "factory.name", "support.null-audio-sink");
    pw_properties_set(props_source, PW_KEY_MEDIA_CLASS, "Audio/Source/Virtual");
    pw_properties_set(props_source, "audio.position", "MONO");
    pw_properties_set(props_source, "monitor.channel-volumes", "false");
    pw_properties_set(props_source, "monitor.passthrough", "true");

    proxy_stream_input_source = pw_core_create_object(
        core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props_source->dict, 0);
    if (!proxy_stream_input_source) {
        goto fail;
    }

    pw_properties_free(props_source);
    props_source = NULL;

    result = pw_main_loop_run(data->loop);

    goto success;
fail:
    result = result < 0 ? result : -1;
success:
    pw_properties_free(props_sink);
    pw_properties_free(props_source);
    if (proxy_stream_output_sink) { pw_proxy_destroy(proxy_stream_output_sink); }
    if (proxy_stream_input_source) { pw_proxy_destroy(proxy_stream_input_source); }
    if (data->inp_link) { pw_proxy_destroy(data->inp_link); }
    if (data->src_link) { pw_proxy_destroy(data->src_link); }
    if (data->filter) { pw_filter_destroy(data->filter); }
    if (core) { pw_core_disconnect(core); }
    if (context) { pw_context_destroy(context); }
    pw_main_loop_destroy(data->loop);
    free(data->inp_name);
    free(data->name);
    free(data->src_name);
    free(data->default_src_name);

    return result;
}

void pfts_set_auto_link(void *d, bool auto_link)
{
    struct pfts_data *data = d;
    data->auto_link = auto_link;
}

int pfts_main_loop_quit(void *d)
{
    struct pfts_data *data = d;
    return pw_main_loop_quit(data->loop);
}

void pfts_main_loop_destroy(void *d)
{
    /* pw_main_loop_destroy was called in the loop thread */
    free(d);
}

void pfts_deinit()
{
    pw_deinit();
}
