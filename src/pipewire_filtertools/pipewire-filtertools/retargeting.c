/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>

#include <spa/utils/hook.h>
#include <spa/utils/json.h>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>

#include "context.h"

struct pfts_node_data {
    struct pfts_data *data;

    struct pw_proxy *proxy;

    struct spa_hook proxy_listener;
    struct spa_hook object_listener;

    char *target_object;
};

void on_node_info(void *data, const struct pw_node_info *info)
{
    struct pfts_node_data *d = data;

    if (!d->data->auto_link) {
        return;
    }

    if (d->target_object) {
        /* Do retarget only once. */
        return;
    }

    const char *name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
    const char *target_object = spa_dict_lookup(info->props,
                                                PW_KEY_TARGET_OBJECT);

    uint64_t target_object_serial = SPA_ID_INVALID;
    if (spa_atou64(target_object, &target_object_serial, 0)) {

        if (target_object_serial != SPA_ID_INVALID &&
            target_object_serial != d->data->default_src_serial &&
            target_object_serial != d->data->src_serial) {
                /* It is set up to record from some other specific
                   device that is not the default device, so no relink. */
                d->target_object = strdup(target_object);
                return;
        }
    } else if (target_object &&
               !spa_streq(target_object, d->data->default_src_name) &&
               !spa_streq(target_object, d->data->src_name)) {
        /* It is set up to record from some other specific
           device that is not the default device, so no relink. */
        d->target_object = strdup(target_object);
        return;
    }

    if (d->data->src_serial == SPA_ID_INVALID) {
        fprintf(stderr, "[pfts] error src serial\n");
        return;
    }

    char src_serial_str[32];
    if (snprintf(src_serial_str,
                    sizeof(src_serial_str),
                    "%"PRIu64, d->data->src_serial) >=
            sizeof(src_serial_str)) {
        fprintf(stderr, "[pfts] error formatting serial\n");
        return;
    }

    if (target_object) {
        fprintf(stderr,
                "[pfts] retargeting %s[%"PRIu32"] from %s\n",
                name ? name : "",
                info->id,
                target_object);
    }

    pw_metadata_set_property(d->data->metadata,
                             info->id,
                             "target.node",
                             "Spa:Id",
                             src_serial_str);

    pw_metadata_set_property(d->data->metadata,
                             info->id,
                             "target.object",
                             "Spa:Id",
                             src_serial_str);

    d->target_object = strdup(d->data->src_name);
}

static const struct pw_node_events node_events = {
    .info = on_node_info,
};

static void on_node_proxy_destroy(void *data) {
    struct pfts_node_data *d = data;
    spa_hook_remove(&d->proxy_listener);
    d->proxy = NULL;
    free(d->target_object);
    d->target_object = NULL;
}

static void on_node_proxy_removed(void *data) {
    struct pfts_node_data *d = data;
    spa_hook_remove(&d->object_listener);
    if (d->proxy) {
        pw_proxy_destroy(d->proxy);
    }
}

static const struct pw_proxy_events node_proxy_events = {
    .destroy = on_node_proxy_destroy,
    .removed = on_node_proxy_removed,
};

static void on_node(struct pfts_data *ctx_data,
                    uint32_t id,
                    const char *type,
                    const struct spa_dict *props)
{
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name) {
        if (strcmp(name, ctx_data->inp_name) == 0) {
            ctx_data->inp_id = id;
            return;
        }
        if (strcmp(name, ctx_data->name) == 0) {
            ctx_data->id = id;
            return;
        }
        if (strcmp(name, ctx_data->src_name) == 0) {
            ctx_data->src_id = id;
            uint64_t serial = SPA_ID_INVALID;
            if (spa_atou64(spa_dict_lookup(props,
                                           PW_KEY_OBJECT_SERIAL),
                           &serial,
                           0)) {
                ctx_data->src_serial = serial;
            }
            return;
        }
        if (ctx_data->default_src_name &&
            strcmp(name, ctx_data->default_src_name) == 0) {

            uint64_t serial = SPA_ID_INVALID;
            if (spa_atou64(spa_dict_lookup(props,
                                           PW_KEY_OBJECT_SERIAL),
                           &serial,
                           0)) {
                ctx_data->default_src_serial = serial;
            }
            return;
        }
    }

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class || strcmp(media_class, "Stream/Input/Audio") != 0) {
        return;
    }

    struct pw_proxy *proxy = pw_registry_bind(ctx_data->registry,
                                              id,
                                              type,
                                              PW_VERSION_NODE,
                                              sizeof(struct pfts_node_data));
    struct pfts_node_data *d = pw_proxy_get_user_data(proxy);
    d->data = ctx_data;
    d->proxy = proxy;
    d->target_object = NULL;

    pw_proxy_add_object_listener(proxy, &d->object_listener, &node_events, d);
    pw_proxy_add_listener(proxy, &d->proxy_listener, &node_proxy_events, d);
}

struct pfts_port_data {
    struct pfts_data *data;

    struct pw_proxy *proxy;

    struct spa_hook proxy_listener;
    struct spa_hook object_listener;

    uint64_t serial;
};

void on_port_proxy_destroy(void *data)
{
    struct pfts_port_data *d = data;
    spa_hook_remove(&d->proxy_listener);
    d->proxy = NULL;
}

void on_port_proxy_removed(void *data)
{
    struct pfts_port_data *d = data;

    if (d->object_listener.link.next || d->object_listener.link.prev) {
        spa_hook_remove(&d->object_listener);
    }

    if (d->proxy) {
        pw_proxy_destroy(d->proxy);
    }
}

static const struct pw_proxy_events port_proxy_events = {
    .destroy = on_port_proxy_destroy,
    .removed = on_port_proxy_removed,
};

static struct pw_proxy* link_ports(struct pw_core *core,
                                   uint32_t output_node_id,
                                   uint32_t output_node_port_id,
                                   uint32_t input_node_id,
                                   uint32_t input_node_port_id)
{
    struct pw_proxy *proxy = NULL;

    struct pw_properties* props = pw_properties_new(NULL, NULL);
    if (!props) {
        fprintf(stderr, "[pfts] error creating a link\n");
        goto fail;
    }

    pw_properties_set(props, PW_KEY_LINK_PASSIVE, "true");
    pw_properties_set(props, PW_KEY_OBJECT_LINGER, "false");
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", output_node_id);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", output_node_port_id);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", input_node_id);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", input_node_port_id);

    proxy = pw_core_create_object(core,
                                  "link-factory",
                                  PW_TYPE_INTERFACE_Link,
                                  PW_VERSION_LINK,
                                  &props->dict,
                                  0);

    if (!proxy) {
        fprintf(stderr, "[pfts] error linking\n");
        goto fail;
    }

fail:
    pw_properties_free(props);

    return proxy;
}

static void on_port(struct pfts_data *ctx_data,
                    uint32_t id,
                    const char *type,
                    const struct spa_dict *props)
{
    const char *port_direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);

    uint32_t node_id = SPA_ID_INVALID;
    if (port_direction && spa_atou32(spa_dict_lookup(props,
                                                     PW_KEY_NODE_ID),
                                     &node_id,
                                     0)) {

        /* If stereo, then pick one. */

        if (ctx_data->inp_out_port_id == SPA_ID_INVALID &&
            node_id == ctx_data->inp_id &&
            strcmp(port_direction, "out") == 0) {
                ctx_data->inp_out_port_id = id;
        }

        if (ctx_data->in_port_id == SPA_ID_INVALID &&
            node_id == ctx_data->id &&
            strcmp(port_direction, "in") == 0) {
                ctx_data->in_port_id = id;
        }

        if (ctx_data->out_port_id == SPA_ID_INVALID &&
            node_id == ctx_data->id &&
            strcmp(port_direction, "out") == 0) {
                ctx_data->out_port_id = id;
        }

        if (ctx_data->src_in_port_id == SPA_ID_INVALID &&
            node_id == ctx_data->src_id &&
            strcmp(port_direction, "in") == 0) {
                ctx_data->src_in_port_id = id;
        }

        if (!ctx_data->inp_link &&
            ctx_data->inp_out_port_id != SPA_ID_INVALID &&
            ctx_data->in_port_id != SPA_ID_INVALID) {
                ctx_data->inp_link = link_ports(ctx_data->core,
                                                ctx_data->inp_id,
                                                ctx_data->inp_out_port_id,
                                                ctx_data->id,
                                                ctx_data->in_port_id);
                return;
        }

        if (!ctx_data->src_link &&
            ctx_data->out_port_id != SPA_ID_INVALID &&
            ctx_data->src_in_port_id != SPA_ID_INVALID) {
                ctx_data->src_link = link_ports(ctx_data->core,
                                                ctx_data->id,
                                                ctx_data->out_port_id,
                                                ctx_data->src_id,
                                                ctx_data->src_in_port_id);
                return;
        }
    }
}

static int on_metadata_property(void *data,
                                uint32_t id,
                                const char *key,
                                const char *type,
                                const char *value)
{
    struct pfts_data *ctx_data = data;

    if (key && value && strcmp(key, "default.audio.source") == 0) {
        struct spa_json it[2];
        const char *name = NULL;

        spa_json_init(&it[0], value, strlen(value));

        if (spa_json_enter_object(&it[0], &it[1]) > 0) {
            char subkey[1024];
            char subvalue[1024];

            while (spa_json_get_string(&it[1], subkey, sizeof(subkey)) > 0) {
                if (spa_json_get_string(&it[1], subvalue, sizeof(subvalue)) <= 0)
                    continue;

                if (spa_streq(subkey, "name")) {
                    name = subvalue;
                    break;
                }
            }
        }

        if (name) {
            if (ctx_data->default_src_name) {
                free(ctx_data->default_src_name);
            }
            ctx_data->default_src_name = strdup(name);
        }
    }

    return 0;
}

static const struct pw_metadata_events metadata_events = {
    .property = on_metadata_property,
};

static void on_metadata(struct pfts_data *ctx_data,
                        uint32_t id,
                        const char *type,
                        const struct spa_dict *props)
{
    const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);

    if (!name || strcmp(name, "default") != 0) {
        return;
    }

    ctx_data->metadata = pw_registry_bind(ctx_data->registry,
                                          id,
                                          type,
                                          PW_VERSION_METADATA,
                                          0);

    if (ctx_data->metadata) {
        int result = pw_metadata_add_listener(ctx_data->metadata,
                                              &ctx_data->metadata_listener,
                                              &metadata_events,
                                              ctx_data);
        if (result < 0) {
            fprintf(stderr, "[pfts] error metadata listener\n");
        }
    } else {
        fprintf(stderr, "[pfts] error metadata binding\n");
    }
}

static void on_registry_global(void *data,
                               uint32_t id,
                               uint32_t permissions,
                               const char *type,
                               uint32_t version,
                               const struct spa_dict *props)
{
    struct pfts_data *ctx_data = data;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        on_node(ctx_data, id, type, props);
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        on_port(ctx_data, id, type, props);
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        on_metadata(ctx_data, id, type, props);
    }
}

static const struct pw_registry_events registry_events = {
    .global = on_registry_global,
};

int setup_retargeting(struct pfts_data *data, struct pw_core *core)
{
    data->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!data->registry) {
        return -1;
    }
    return pw_registry_add_listener(data->registry,
                                    &data->registry_listener,
                                    &registry_events,
                                    data);
}
