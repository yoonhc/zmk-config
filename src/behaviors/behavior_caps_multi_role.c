/*
 * SPDX-License-Identifier: MIT
 *
 * A timestamp-driven three-way behavior:
 *   tap                  -> binding 0
 *   hold / interrupt     -> binding 1
 *   tap, then press/hold -> binding 2
 *
 * Interrupts can resolve immediately (hold-preferred) or by release order
 * (balanced). Balanced instances capture intervening position events until
 * either Caps, an interrupted key, or the logical deadline resolves them.
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
#define CAPS_MULTI_MAX_CAPTURED_EVENTS                                                        \
    CONFIG_ZMK_BEHAVIOR_CAPS_MULTI_ROLE_MAX_CAPTURED_EVENTS

enum caps_multi_interrupt_flavor {
    CAPS_MULTI_HOLD_PREFERRED,
    CAPS_MULTI_BALANCED,
};

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
    enum caps_multi_interrupt_flavor interrupt_flavor;
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

struct caps_multi_capture {
    struct caps_multi_active *owner;
    uint32_t owner_generation;
    int64_t first_interrupt_timestamp;
    uint8_t count;
    bool replaying;
    struct zmk_position_state_changed_event events[CAPS_MULTI_MAX_CAPTURED_EVENTS];
};

static struct caps_multi_capture captured;

extern const struct zmk_listener zmk_listener_behavior_caps_multi_role;

static bool is_balanced(const struct caps_multi_active *active) {
    return active->config->interrupt_flavor == CAPS_MULTI_BALANCED;
}

static bool capture_owned_by(const struct caps_multi_active *active) {
    return captured.owner == active && captured.owner_generation == active->generation;
}

static bool detach_capture(const struct caps_multi_active *active) {
    if (!capture_owned_by(active)) {
        return false;
    }

    captured.owner = NULL;
    captured.owner_generation = 0;
    captured.first_interrupt_timestamp = 0;
    return true;
}

static void replay_captured_events(void) {
    if (captured.count == 0) {
        return;
    }

    captured.replaying = true;
    uint8_t count = captured.count;

    for (int i = 0; i < count; i++) {
        int err = ZMK_EVENT_RAISE_AT(captured.events[i], behavior_caps_multi_role);
        if (err < 0) {
            LOG_ERR("Failed to replay captured position event: %d", err);
        }
    }

    captured.count = 0;
    captured.replaying = false;
}

static bool captured_key_is_down(const struct zmk_position_state_changed *event) {
    for (int i = 0; i < captured.count; i++) {
        const struct zmk_position_state_changed *captured_event = &captured.events[i].data;
        if (captured_event->position == event->position && captured_event->source == event->source &&
            captured_event->state) {
            return true;
        }
    }

    return false;
}

static int capture_position_event(const struct zmk_position_state_changed *event) {
    if (captured.count >= CAPS_MULTI_MAX_CAPTURED_EVENTS) {
        return -ENOMEM;
    }

    captured.events[captured.count++] = copy_raised_zmk_position_state_changed(event);
    return 0;
}

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
    bool replay = detach_capture(active);

    invalidate_timer(active);
    active->state = CAPS_MULTI_CTRL_HELD;

    int err = invoke_child(active, CAPS_MULTI_HOLD, true, timestamp);
    if (err < 0) {
        LOG_ERR("Failed to press hold child behavior: %d", err);
    }

    if (replay) {
        replay_captured_events();
    }
}

static void resolve_interrupted_tap(struct caps_multi_active *active, int64_t timestamp) {
    bool replay = detach_capture(active);

    invalidate_timer(active);
    invoke_tap(active, timestamp);
    clear_active(active);

    if (replay) {
        replay_captured_events();
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
            if (capture_owned_by(active)) {
                resolve_interrupted_tap(active, event.timestamp);
            } else {
                active->state = CAPS_MULTI_TAP_PENDING;
            }
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

    if (event == NULL || captured.replaying) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Enforce the logical deadline before handling a later physical event. */
    for (int i = 0; i < CAPS_MULTI_MAX_ACTIVE; i++) {
        struct caps_multi_active *active = &active_sequences[i];
        if (active->state == CAPS_MULTI_FREE || active->position == event->position) {
            continue;
        }

        if ((active->state == CAPS_MULTI_FIRST_DOWN ||
             active->state == CAPS_MULTI_TAP_PENDING) &&
            event->timestamp >= active->deadline) {
            resolve_deadline(active);
        }
    }

    if (captured.owner != NULL) {
        struct caps_multi_active *owner = captured.owner;

        if (!capture_owned_by(owner) || owner->state != CAPS_MULTI_FIRST_DOWN ||
            !is_balanced(owner)) {
            LOG_ERR("Discarding stale balanced capture owner");
            captured.owner = NULL;
            captured.owner_generation = 0;
            captured.first_interrupt_timestamp = 0;
            replay_captured_events();
        } else if (owner->position != event->position) {
            if (!event->state && !captured_key_is_down(event)) {
                return ZMK_EV_EVENT_BUBBLE;
            }

            int err = capture_position_event(event);
            if (err < 0) {
                LOG_WRN("Balanced capture buffer full; resolving as hold");
                resolve_first_hold(owner, captured.first_interrupt_timestamp);
                return ZMK_EV_EVENT_BUBBLE;
            }

            if (!event->state) {
                resolve_first_hold(owner, captured.first_interrupt_timestamp);
            }

            return ZMK_EV_EVENT_CAPTURED;
        }
    }

    if (!event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (int i = 0; i < CAPS_MULTI_MAX_ACTIVE; i++) {
        struct caps_multi_active *active = &active_sequences[i];
        if (active->state == CAPS_MULTI_FREE || active->position == event->position) {
            continue;
        }

        if (active->state == CAPS_MULTI_FIRST_DOWN) {
            if (is_balanced(active)) {
                captured.owner = active;
                captured.owner_generation = active->generation;
                captured.first_interrupt_timestamp = event->timestamp;
                captured.count = 0;

                int err = capture_position_event(event);
                if (err < 0) {
                    LOG_WRN("Unable to start balanced capture; resolving as hold");
                    resolve_first_hold(active, event->timestamp);
                    return ZMK_EV_EVENT_BUBBLE;
                }

                return ZMK_EV_EVENT_CAPTURED;
            }

            /* Hold-preferred keeps the original immediate interrupt behavior. */
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
        .interrupt_flavor = DT_INST_ENUM_IDX_OR(n, interrupt_flavor, 0),                         \
        .bindings = {ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                              \
                     ZMK_KEYMAP_EXTRACT_BINDING(1, DT_DRV_INST(n)),                              \
                     ZMK_KEYMAP_EXTRACT_BINDING(2, DT_DRV_INST(n))},                             \
    };                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, caps_multi_init, NULL, NULL, &caps_multi_config_##n, POST_KERNEL, \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &caps_multi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CAPS_MULTI_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
