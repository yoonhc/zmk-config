/*
 * SPDX-License-Identifier: MIT
 *
 * A timestamp-driven three-way behavior:
 *   tap                  -> binding 0
 *   hold / interrupted   -> binding 1
 *   tap, then press/hold -> binding 2
 */

#define DT_DRV_COMPAT zmk_behavior_caps_multi_role

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define CAPS_MULTI_POSITION_FREE UINT32_MAX
#define CAPS_MULTI_BINDING_COUNT 3
#define CAPS_MULTI_MAX_ACTIVE CONFIG_ZMK_BEHAVIOR_CAPS_MULTI_ROLE_MAX_ACTIVE

enum caps_multi_state {
    CAPS_MULTI_FREE,
    CAPS_MULTI_FIRST_DOWN,
    CAPS_MULTI_TAP_PENDING,
    CAPS_MULTI_CTRL_HELD,
    CAPS_MULTI_LAYER_HELD,
};

enum caps_multi_child {
    CAPS_MULTI_TAP,
    CAPS_MULTI_HOLD,
    CAPS_MULTI_LAYER,
};

struct caps_multi_config {
    uint32_t tapping_term_ms;
    struct zmk_behavior_binding bindings[CAPS_MULTI_BINDING_COUNT];
};

struct caps_multi_active {
    enum caps_multi_state state;
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    const struct caps_multi_config *config;
    int64_t deadline;
    uint32_t generation;

    struct k_work_delayable timer;
    uint32_t timer_generation;
    bool timer_pending;
    bool timer_cleanup_pending;
};

static struct caps_multi_active active_sequences[CAPS_MULTI_MAX_ACTIVE];

static struct zmk_behavior_binding_event child_event(const struct caps_multi_active *active,
                                                     int64_t timestamp) {
    return (struct zmk_behavior_binding_event){
        .layer = 0,
        .position = active->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = active->source,
#endif
    };
}

static int invoke_child(const struct caps_multi_active *active, enum caps_multi_child child,
                        bool pressed, int64_t timestamp) {
    struct zmk_behavior_binding binding = active->config->bindings[child];
    return zmk_behavior_invoke_binding(&binding, child_event(active, timestamp), pressed);
}

static void invoke_tap(const struct caps_multi_active *active, int64_t timestamp) {
    int err = invoke_child(active, CAPS_MULTI_TAP, true, timestamp);
    if (err < 0) {
        LOG_ERR("Failed to press tap child behavior: %d", err);
    }

    err = invoke_child(active, CAPS_MULTI_TAP, false, timestamp);
    if (err < 0) {
        LOG_ERR("Failed to release tap child behavior: %d", err);
    }
}

static struct caps_multi_active *find_active(uint32_t position) {
    for (int i = 0; i < CAPS_MULTI_MAX_ACTIVE; i++) {
        if (active_sequences[i].state != CAPS_MULTI_FREE &&
            active_sequences[i].position == position) {
            return &active_sequences[i];
        }
    }

    return NULL;
}

static void invalidate_timer(struct caps_multi_active *active) {
    if (!active->timer_pending) {
        return;
    }

    active->timer_pending = false;
    active->timer_generation = 0;

    int result = k_work_cancel_delayable(&active->timer);
    active->timer_cleanup_pending = (result == -EINPROGRESS);
}

static void clear_active(struct caps_multi_active *active) {
    active->state = CAPS_MULTI_FREE;
    active->position = CAPS_MULTI_POSITION_FREE;
    active->config = NULL;
    active->deadline = 0;
}

static void resolve_pending_tap(struct caps_multi_active *active, int64_t timestamp) {
    invalidate_timer(active);
    invoke_tap(active, timestamp);
    clear_active(active);
}

static void resolve_first_hold(struct caps_multi_active *active, int64_t timestamp) {
    invalidate_timer(active);
    active->state = CAPS_MULTI_CTRL_HELD;

    int err = invoke_child(active, CAPS_MULTI_HOLD, true, timestamp);
    if (err < 0) {
        LOG_ERR("Failed to press hold child behavior: %d", err);
    }
}

static void resolve_deadline(struct caps_multi_active *active) {
    switch (active->state) {
    case CAPS_MULTI_FIRST_DOWN:
        resolve_first_hold(active, active->deadline);
        break;
    case CAPS_MULTI_TAP_PENDING:
        resolve_pending_tap(active, active->deadline);
        break;
    default:
        break;
    }
}

static void caps_multi_timer_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct caps_multi_active *active =
        CONTAINER_OF(delayable, struct caps_multi_active, timer);

    uint32_t fired_generation = active->timer_generation;
    active->timer_cleanup_pending = false;

    if (!active->timer_pending || fired_generation == 0 ||
        fired_generation != active->generation || active->state == CAPS_MULTI_FREE) {
        return;
    }

    active->timer_pending = false;
    active->timer_generation = 0;
    resolve_deadline(active);
}

static struct caps_multi_active *allocate_active(void) {
    for (int i = 0; i < CAPS_MULTI_MAX_ACTIVE; i++) {
        struct caps_multi_active *active = &active_sequences[i];
        if (active->state == CAPS_MULTI_FREE && !active->timer_cleanup_pending) {
            return active;
        }
    }

    return NULL;
}

static int start_first_press(const struct caps_multi_config *config,
                             struct zmk_behavior_binding_event event) {
    struct caps_multi_active *active = allocate_active();
    if (active == NULL) {
        LOG_ERR("No free Caps multi-role state slot");
        return -ENOMEM;
    }

    active->generation++;
    if (active->generation == 0) {
        active->generation = 1;
    }

    active->state = CAPS_MULTI_FIRST_DOWN;
    active->position = event.position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    active->source = event.source;
#endif
    active->config = config;
    active->deadline = event.timestamp + config->tapping_term_ms;
    active->timer_generation = active->generation;
    active->timer_pending = true;
    active->timer_cleanup_pending = false;

    int64_t delay_ms = active->deadline - k_uptime_get();
    if (delay_ms <= 0) {
        active->timer_pending = false;
        active->timer_generation = 0;
        resolve_deadline(active);
    } else {
        k_work_schedule(&active->timer, K_MSEC(delay_ms));
    }

    return 0;
}

static int caps_multi_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct caps_multi_config *config = dev->config;
    struct caps_multi_active *active = find_active(event.position);

    if (active == NULL) {
        start_first_press(config, event);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (active->state != CAPS_MULTI_TAP_PENDING) {
        LOG_WRN("Ignoring duplicate Caps multi-role press at position %u", event.position);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (event.timestamp >= active->deadline) {
        resolve_pending_tap(active, active->deadline);
        start_first_press(config, event);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    invalidate_timer(active);
    active->state = CAPS_MULTI_LAYER_HELD;

    int err = invoke_child(active, CAPS_MULTI_LAYER, true, event.timestamp);
    if (err < 0) {
        LOG_ERR("Failed to activate layer child behavior: %d", err);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int caps_multi_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    struct caps_multi_active *active = find_active(event.position);
    if (active == NULL) {
        LOG_WRN("Ignoring Caps multi-role release without an active sequence at position %u",
                event.position);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    switch (active->state) {
    case CAPS_MULTI_FIRST_DOWN:
        if (event.timestamp < active->deadline) {
            active->state = CAPS_MULTI_TAP_PENDING;
        } else {
            resolve_first_hold(active, active->deadline);
            int err = invoke_child(active, CAPS_MULTI_HOLD, false, event.timestamp);
            if (err < 0) {
                LOG_ERR("Failed to release hold child behavior: %d", err);
            }
            clear_active(active);
        }
        break;
    case CAPS_MULTI_CTRL_HELD: {
        int err = invoke_child(active, CAPS_MULTI_HOLD, false, event.timestamp);
        if (err < 0) {
            LOG_ERR("Failed to release hold child behavior: %d", err);
        }
        clear_active(active);
        break;
    }
    case CAPS_MULTI_LAYER_HELD: {
        int err = invoke_child(active, CAPS_MULTI_LAYER, false, event.timestamp);
        if (err < 0) {
            LOG_ERR("Failed to deactivate layer child behavior: %d", err);
        }
        clear_active(active);
        break;
    }
    case CAPS_MULTI_TAP_PENDING:
    case CAPS_MULTI_FREE:
        LOG_WRN("Ignoring unexpected Caps multi-role release state at position %u", event.position);
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int caps_multi_position_listener(const zmk_event_t *event_header) {
    const struct zmk_position_state_changed *event =
        as_zmk_position_state_changed(event_header);

    if (event == NULL || !event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (int i = 0; i < CAPS_MULTI_MAX_ACTIVE; i++) {
        struct caps_multi_active *active = &active_sequences[i];
        if (active->state == CAPS_MULTI_FREE || active->position == event->position) {
            continue;
        }

        if ((active->state == CAPS_MULTI_FIRST_DOWN ||
             active->state == CAPS_MULTI_TAP_PENDING) &&
            event->timestamp >= active->deadline) {
            resolve_deadline(active);
            continue;
        }

        if (active->state == CAPS_MULTI_FIRST_DOWN) {
            /* The hold child must be down before this event reaches the keymap listener. */
            resolve_first_hold(active, event->timestamp);
        } else if (active->state == CAPS_MULTI_TAP_PENDING) {
            /* Flush the tap child before this event reaches the keymap listener. */
            resolve_pending_tap(active, event->timestamp);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_caps_multi_role, caps_multi_position_listener);
ZMK_SUBSCRIPTION(behavior_caps_multi_role, zmk_position_state_changed);

static const struct behavior_driver_api caps_multi_driver_api = {
    .binding_pressed = caps_multi_binding_pressed,
    .binding_released = caps_multi_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int caps_multi_init(const struct device *dev) {
    ARG_UNUSED(dev);
    static bool initialized;

    if (!initialized) {
        for (int i = 0; i < CAPS_MULTI_MAX_ACTIVE; i++) {
            active_sequences[i].state = CAPS_MULTI_FREE;
            active_sequences[i].position = CAPS_MULTI_POSITION_FREE;
            k_work_init_delayable(&active_sequences[i].timer, caps_multi_timer_handler);
        }
        initialized = true;
    }

    return 0;
}

#define CAPS_MULTI_INST(n)                                                                        \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == CAPS_MULTI_BINDING_COUNT,                       \
                 "Caps multi-role requires exactly three bindings");                            \
    static const struct caps_multi_config caps_multi_config_##n = {                              \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                     \
        .bindings = {ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                              \
                     ZMK_KEYMAP_EXTRACT_BINDING(1, DT_DRV_INST(n)),                              \
                     ZMK_KEYMAP_EXTRACT_BINDING(2, DT_DRV_INST(n))},                             \
    };                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, caps_multi_init, NULL, NULL, &caps_multi_config_##n, POST_KERNEL, \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &caps_multi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CAPS_MULTI_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
