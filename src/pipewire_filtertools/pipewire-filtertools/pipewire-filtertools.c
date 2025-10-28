/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>

#include <pipewire/pipewire.h>

#include "pipewire-filtertools.h"

struct pfts_data {
    struct pw_main_loop *loop;
    struct pw_stream *capture;
    struct pw_stream *playback;

    struct spa_audio_info format;

    float *buffer;
    size_t buffer_size;
    size_t n_frames;
    bool have_data;

    pfts_on_buffer on_capture_cb;
    pfts_on_buffer on_playback_cb;
    void *user_ctx;
};

/* ---------- Callbacks ---------- */

static void on_capture_process(void *userdata)
{
    struct pfts_data *d = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *samples;
    uint32_t n_samples;

    if ((b = pw_stream_dequeue_buffer(d->capture)) == NULL)
        return;
    buf = b->buffer;
    if ((samples = buf->datas[0].data) == NULL)
        return;

    n_samples = buf->datas[0].chunk->size / sizeof(float);

    /* user callback (read samples) */
    if (d->on_capture_cb)
        d->on_capture_cb(d->user_ctx, samples, n_samples);

    /* store for playback */
    if (n_samples > d->buffer_size)
        n_samples = d->buffer_size;
    memcpy(d->buffer, samples, n_samples * sizeof(float));
    d->n_frames = n_samples / d->format.info.raw.channels;
    d->have_data = true;

    pw_stream_queue_buffer(d->capture, b);
}

static void on_playback_process(void *userdata)
{
    struct pfts_data *d = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *dst;
    uint32_t n_frames, stride;

    if ((b = pw_stream_dequeue_buffer(d->playback)) == NULL)
        return;
    buf = b->buffer;
    if ((dst = buf->datas[0].data) == NULL)
        return;

    stride = sizeof(float) * d->format.info.raw.channels;
    n_frames = buf->datas[0].maxsize / stride;

    if (d->have_data) {
        size_t copy_frames = SPA_MIN(d->n_frames, n_frames);
        memcpy(dst, d->buffer, copy_frames * stride);
        buf->datas[0].chunk->size = copy_frames * stride;
        d->have_data = false;
    } else {
        memset(dst, 0, n_frames * stride);
        buf->datas[0].chunk->size = n_frames * stride;
    }

    if (d->on_playback_cb)
        d->on_playback_cb(d->user_ctx, dst, buf->datas[0].chunk->size / sizeof(float));

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;

    pw_stream_queue_buffer(d->playback, b);
}

/* ---------- Format negotiation ---------- */
static void on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param)
{
    struct pfts_data *d = _data;
    if (param == NULL || id != SPA_PARAM_Format)
        return;

    if (spa_format_parse(param, &d->format.media_type, &d->format.media_subtype) < 0)
        return;

    if (d->format.media_type != SPA_MEDIA_TYPE_audio ||
        d->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    spa_format_audio_raw_parse(param, &d->format.info.raw);

    fprintf(stdout, "[pfts] negotiated format: rate=%d channels=%d\n",
            d->format.info.raw.rate, d->format.info.raw.channels);
}

static const struct pw_stream_events capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_stream_param_changed,
    .process = on_capture_process,
};

static const struct pw_stream_events playback_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_playback_process,
};

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

static void do_quit(void *userdata, int signal_number)
{
    struct pfts_data *d = userdata;
    pw_main_loop_quit(d->loop);
}

void *pfts_main_loop_new()
{
    return pw_main_loop_new(NULL);
}

void pfts_main_loop_run(void *ctx, void *loop, uint32_t rate, const uint32_t quantum,
                        pfts_on_buffer on_capture, pfts_on_buffer on_playback)
{
    struct pfts_data data = {0};
    data.user_ctx = ctx;
    data.on_capture_cb = on_capture;
    data.on_playback_cb = on_playback;

    data.loop = loop;

    const char *link_group = "pfts-loop";
    const uint32_t channels = 1;
    char rate_str[16], latency_str[16];
    snprintf(rate_str, sizeof(rate_str), "1/%u", rate);
    snprintf(latency_str, sizeof(latency_str), "%u/%u", quantum, rate);
    data.buffer_size = quantum * channels;
    data.buffer = calloc(data.buffer_size, sizeof(float));

    /* Register signal handlers */
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

    /* Capture stream */
    struct pw_properties *cap_props = pw_properties_new(
        PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
        PW_KEY_NODE_LINK_GROUP, link_group,
        PW_KEY_NODE_RATE, rate_str,
        PW_KEY_NODE_LATENCY, latency_str,
        NULL);

    data.capture = pw_stream_new_simple(
        pw_main_loop_get_loop(data.loop),
        "pfts-capture",
        cap_props,
        &capture_events,
        &data);

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
                                           &SPA_AUDIO_INFO_RAW_INIT(
                                               .format = SPA_AUDIO_FORMAT_F32,
                                               .channels = channels,
                                               .rate = rate));

    pw_stream_connect(data.capture,
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    /* Playback stream */
    struct pw_properties *pb_props = pw_properties_new(
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_LINK_GROUP, link_group,
        PW_KEY_NODE_RATE, rate_str,
        PW_KEY_NODE_LATENCY, latency_str,
        NULL);

    data.playback = pw_stream_new_simple(
        pw_main_loop_get_loop(data.loop),
        "pfts-playback",
        pb_props,
        &playback_events,
        &data);

    pw_stream_connect(data.playback,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    /* Run loop (blocking) */
    pw_main_loop_run(data.loop);

    /* Cleanup */
    pw_stream_destroy(data.capture);
    pw_stream_destroy(data.playback);
    pw_main_loop_destroy(data.loop);
    free(data.buffer);
}

void pfts_main_loop_quit(void *loop)
{
    pw_main_loop_quit((struct pw_main_loop *)loop);
}

void pfts_deinit()
{
    pw_deinit();
}
