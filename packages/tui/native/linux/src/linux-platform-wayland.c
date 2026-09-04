#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "clipboard-io.h"
#include "ext-data-control-client-protocol.h"
#include "wlr-data-control-client-protocol.h"

#define NAPI_AUTO_LENGTH ((size_t)-1)
#define MAX_OFFERS 8

typedef void* napi_env;
typedef void* napi_value;
typedef void* napi_callback_info;
typedef napi_value (*napi_callback)(napi_env, napi_callback_info);
typedef int (*napi_create_buffer_copy_fn)(napi_env, size_t, const void*, void**, napi_value*);
typedef int (*napi_create_function_fn)(napi_env, const char*, size_t, napi_callback, void*, napi_value*);
typedef int (*napi_create_string_utf8_fn)(napi_env, const char*, size_t, napi_value*);
typedef int (*napi_get_boolean_fn)(napi_env, bool, napi_value*);
typedef int (*napi_get_undefined_fn)(napi_env, napi_value*);
typedef int (*napi_set_named_property_fn)(napi_env, napi_value, const char*, napi_value);
typedef int (*napi_throw_error_fn)(napi_env, const char*, const char*);

typedef enum {
    DATA_CONTROL_NONE,
    DATA_CONTROL_EXT,
    DATA_CONTROL_WLR,
} data_control_protocol;

typedef struct {
    void* proxy;
    const char* text_mime;
    const char* image_mime;
    int text_rank;
    int image_rank;
} offer_info;

typedef struct {
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_seat* seat;
    struct ext_data_control_manager_v1* ext_manager;
    struct zwlr_data_control_manager_v1* wlr_manager;
    void* device;
    data_control_protocol protocol;
    offer_info offers[MAX_OFFERS];
    size_t offer_count;
    offer_info* selection;
    int64_t deadline;
} clipboard_state;

typedef struct {
    unsigned char* data;
    size_t length;
} clipboard_bytes;

static void* node_symbol(const char* name) {
    return dlsym(RTLD_DEFAULT, name);
}

static napi_value undefined_value(napi_env env) {
    napi_get_undefined_fn napi_get_undefined = (napi_get_undefined_fn)node_symbol("napi_get_undefined");
    napi_value result = 0;
    if (napi_get_undefined) napi_get_undefined(env, &result);
    return result;
}

static napi_value fail(napi_env env, const char* message) {
    napi_throw_error_fn napi_throw_error = (napi_throw_error_fn)node_symbol("napi_throw_error");
    if (napi_throw_error) napi_throw_error(env, 0, message);
    return undefined_value(env);
}

static int text_mime_rank(const char* mime, const char** canonical) {
    if (strcmp(mime, "text/plain;charset=utf-8") == 0) {
        *canonical = "text/plain;charset=utf-8";
        return 0;
    }
    if (strcmp(mime, "text/plain;charset=UTF-8") == 0) {
        *canonical = "text/plain;charset=UTF-8";
        return 1;
    }
    if (strcmp(mime, "UTF8_STRING") == 0) {
        *canonical = "UTF8_STRING";
        return 2;
    }
    if (strcmp(mime, "text/plain") == 0) {
        *canonical = "text/plain";
        return 3;
    }
    if (strcmp(mime, "STRING") == 0) {
        *canonical = "STRING";
        return 4;
    }
    return 100;
}

static int image_mime_rank(const char* mime, const char** canonical) {
    static const char* types[] = {
        "image/png",
        "image/jpeg",
        "image/webp",
        "image/gif",
        "image/bmp",
        "image/tiff",
    };
    for (int index = 0; index < 6; index++) {
        if (strcmp(mime, types[index]) == 0) {
            *canonical = types[index];
            return index;
        }
    }
    return 100;
}

static void record_mime(offer_info* info, const char* mime) {
    const char* canonical = 0;
    int rank = text_mime_rank(mime, &canonical);
    if (rank < info->text_rank) {
        info->text_rank = rank;
        info->text_mime = canonical;
    }
    rank = image_mime_rank(mime, &canonical);
    if (rank < info->image_rank) {
        info->image_rank = rank;
        info->image_mime = canonical;
    }
}

static void ext_offer_mime(
    void* data,
    struct ext_data_control_offer_v1* offer,
    const char* mime
) {
    (void)offer;
    record_mime(data, mime);
}

static void wlr_offer_mime(
    void* data,
    struct zwlr_data_control_offer_v1* offer,
    const char* mime
) {
    (void)offer;
    record_mime(data, mime);
}

static const struct ext_data_control_offer_v1_listener ext_offer_listener = {ext_offer_mime};
static const struct zwlr_data_control_offer_v1_listener wlr_offer_listener = {wlr_offer_mime};

static offer_info* find_offer(clipboard_state* state, void* proxy) {
    for (size_t index = 0; index < state->offer_count; index++) {
        if (state->offers[index].proxy == proxy) return &state->offers[index];
    }
    return 0;
}

static offer_info* add_offer(clipboard_state* state, void* proxy) {
    if (state->offer_count >= MAX_OFFERS) return 0;
    offer_info* info = &state->offers[state->offer_count++];
    info->proxy = proxy;
    info->text_mime = 0;
    info->image_mime = 0;
    info->text_rank = 100;
    info->image_rank = 100;
    return info;
}

static void ext_device_data_offer(
    void* data,
    struct ext_data_control_device_v1* device,
    struct ext_data_control_offer_v1* offer
) {
    (void)device;
    offer_info* info = add_offer(data, offer);
    if (info) ext_data_control_offer_v1_add_listener(offer, &ext_offer_listener, info);
}

static void wlr_device_data_offer(
    void* data,
    struct zwlr_data_control_device_v1* device,
    struct zwlr_data_control_offer_v1* offer
) {
    (void)device;
    offer_info* info = add_offer(data, offer);
    if (info) zwlr_data_control_offer_v1_add_listener(offer, &wlr_offer_listener, info);
}

static void ext_device_selection(
    void* data,
    struct ext_data_control_device_v1* device,
    struct ext_data_control_offer_v1* offer
) {
    (void)device;
    clipboard_state* state = data;
    state->selection = offer ? find_offer(state, offer) : 0;
}

static void wlr_device_selection(
    void* data,
    struct zwlr_data_control_device_v1* device,
    struct zwlr_data_control_offer_v1* offer
) {
    (void)device;
    clipboard_state* state = data;
    state->selection = offer ? find_offer(state, offer) : 0;
}

static void ext_device_finished(void* data, struct ext_data_control_device_v1* device) {
    (void)data;
    (void)device;
}

static void wlr_device_finished(void* data, struct zwlr_data_control_device_v1* device) {
    (void)data;
    (void)device;
}

static void ext_device_primary_selection(
    void* data,
    struct ext_data_control_device_v1* device,
    struct ext_data_control_offer_v1* offer
) {
    (void)data;
    (void)device;
    (void)offer;
}

static void wlr_device_primary_selection(
    void* data,
    struct zwlr_data_control_device_v1* device,
    struct zwlr_data_control_offer_v1* offer
) {
    (void)data;
    (void)device;
    (void)offer;
}

static const struct ext_data_control_device_v1_listener ext_device_listener = {
    ext_device_data_offer,
    ext_device_selection,
    ext_device_finished,
    ext_device_primary_selection,
};

static const struct zwlr_data_control_device_v1_listener wlr_device_listener = {
    wlr_device_data_offer,
    wlr_device_selection,
    wlr_device_finished,
    wlr_device_primary_selection,
};

static void registry_global(
    void* data,
    struct wl_registry* registry,
    uint32_t name,
    const char* interface,
    uint32_t version
) {
    clipboard_state* state = data;
    if (!state->seat && strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 2 ? version : 2);
    } else if (!state->ext_manager && strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        state->ext_manager = wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
    } else if (!state->wlr_manager && strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
        state->wlr_manager = wl_registry_bind(
            registry,
            name,
            &zwlr_data_control_manager_v1_interface,
            version < 2 ? version : 2
        );
    }
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {registry_global, registry_global_remove};

static bool flush_display(clipboard_state* state) {
    while (wl_display_flush(state->display) < 0) {
        if (errno != EAGAIN ||
            !(wait_for_fd(wl_display_get_fd(state->display), POLLOUT, state->deadline) & POLLOUT)) {
            return false;
        }
    }
    return true;
}

static void sync_done(void* data, struct wl_callback* callback, uint32_t serial) {
    (void)callback;
    (void)serial;
    *(bool*)data = true;
}

static const struct wl_callback_listener sync_listener = {sync_done};

// wl_display_roundtrip() can block indefinitely, even before a clipboard offer
// exists. Drive the connection ourselves so discovery shares the read deadline.
static bool roundtrip(clipboard_state* state) {
    bool done = false;
    struct wl_callback* callback = wl_display_sync(state->display);
    if (!callback) return false;
    if (wl_callback_add_listener(callback, &sync_listener, &done) != 0) {
        wl_callback_destroy(callback);
        return false;
    }

    while (!done && monotonic_ms() < state->deadline) {
        if (wl_display_dispatch_pending(state->display) < 0) break;
        if (done) break;
        if (wl_display_prepare_read(state->display) != 0) break;
        if (!flush_display(state) ||
            !(wait_for_fd(wl_display_get_fd(state->display), POLLIN, state->deadline) & POLLIN)) {
            wl_display_cancel_read(state->display);
            break;
        }
        if (wl_display_read_events(state->display) < 0) break;
    }
    wl_callback_destroy(callback);
    return done;
}

static bool open_clipboard(clipboard_state* state) {
    memset(state, 0, sizeof(*state));
    state->deadline = monotonic_ms() + CLIPBOARD_TIMEOUT_MS;
    state->display = wl_display_connect(0);
    if (!state->display) return false;
    state->registry = wl_display_get_registry(state->display);
    if (!state->registry || wl_registry_add_listener(state->registry, &registry_listener, state) != 0 ||
        !roundtrip(state) || !state->seat || (!state->ext_manager && !state->wlr_manager)) {
        return false;
    }

    if (state->ext_manager) {
        state->protocol = DATA_CONTROL_EXT;
        struct ext_data_control_device_v1* device =
            ext_data_control_manager_v1_get_data_device(state->ext_manager, state->seat);
        state->device = device;
        if (!device || ext_data_control_device_v1_add_listener(device, &ext_device_listener, state) != 0) {
            return false;
        }
    } else {
        state->protocol = DATA_CONTROL_WLR;
        struct zwlr_data_control_device_v1* device =
            zwlr_data_control_manager_v1_get_data_device(state->wlr_manager, state->seat);
        state->device = device;
        if (!device || zwlr_data_control_device_v1_add_listener(device, &wlr_device_listener, state) != 0) {
            return false;
        }
    }

    return roundtrip(state);
}

static void close_clipboard(clipboard_state* state) {
    for (size_t index = 0; index < state->offer_count; index++) {
        if (!state->offers[index].proxy) continue;
        if (state->protocol == DATA_CONTROL_EXT) {
            ext_data_control_offer_v1_destroy(state->offers[index].proxy);
        } else {
            zwlr_data_control_offer_v1_destroy(state->offers[index].proxy);
        }
    }
    if (state->device) {
        if (state->protocol == DATA_CONTROL_EXT) {
            ext_data_control_device_v1_destroy(state->device);
        } else {
            zwlr_data_control_device_v1_destroy(state->device);
        }
    }
    if (state->ext_manager) ext_data_control_manager_v1_destroy(state->ext_manager);
    if (state->wlr_manager) zwlr_data_control_manager_v1_destroy(state->wlr_manager);
    if (state->seat) wl_seat_destroy(state->seat);
    if (state->registry) wl_registry_destroy(state->registry);
    if (state->display) wl_display_disconnect(state->display);
}

static bool receive_offer(
    clipboard_state* state,
    offer_info* offer,
    const char* mime,
    clipboard_bytes* result
) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return false;

    if (state->protocol == DATA_CONTROL_EXT) {
        ext_data_control_offer_v1_receive(offer->proxy, mime, descriptors[1]);
    } else {
        zwlr_data_control_offer_v1_receive(offer->proxy, mime, descriptors[1]);
    }
    if (!flush_display(state)) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    close(descriptors[1]);

    size_t capacity = 16384;
    unsigned char* bytes = malloc(capacity);
    if (!bytes) {
        close(descriptors[0]);
        return false;
    }

    size_t length = 0;
    bool complete = false;
    while (length < MAX_CLIPBOARD_BYTES) {
        if (!(wait_for_fd(descriptors[0], POLLIN, state->deadline) & (POLLIN | POLLHUP))) break;

        if (length == capacity) {
            size_t next_capacity = capacity * 2;
            if (next_capacity > MAX_CLIPBOARD_BYTES) next_capacity = MAX_CLIPBOARD_BYTES;
            unsigned char* next = realloc(bytes, next_capacity);
            if (!next) break;
            bytes = next;
            capacity = next_capacity;
        }

        ssize_t count = read(descriptors[0], bytes + length, capacity - length);
        if (count > 0) {
            length += (size_t)count;
        } else if (count == 0) {
            complete = true;
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    close(descriptors[0]);

    if (!complete) {
        free(bytes);
        return false;
    }
    result->data = bytes;
    result->length = length;
    return true;
}

static bool read_clipboard(bool image, clipboard_bytes* result) {
    clipboard_state state;
    bool opened = open_clipboard(&state);
    const char* mime = opened && state.selection
        ? (image ? state.selection->image_mime : state.selection->text_mime)
        : 0;
    bool received = mime && receive_offer(&state, state.selection, mime, result);
    close_clipboard(&state);
    return received;
}

static napi_value is_clipboard_available(napi_env env, napi_callback_info info) {
    (void)info;
    clipboard_state state;
    bool available = open_clipboard(&state);
    close_clipboard(&state);

    napi_get_boolean_fn napi_get_boolean = (napi_get_boolean_fn)node_symbol("napi_get_boolean");
    napi_value result = 0;
    if (!napi_get_boolean || napi_get_boolean(env, available, &result) != 0) {
        return fail(env, "Could not inspect Wayland clipboard availability");
    }
    return result;
}

static napi_value get_clipboard_text(napi_env env, napi_callback_info info) {
    (void)info;
    clipboard_bytes contents = {0};
    if (!read_clipboard(false, &contents)) return fail(env, "Wayland clipboard does not contain text");

    napi_create_string_utf8_fn napi_create_string_utf8 =
        (napi_create_string_utf8_fn)node_symbol("napi_create_string_utf8");
    napi_value result = 0;
    int status = napi_create_string_utf8
        ? napi_create_string_utf8(env, (const char*)contents.data, contents.length, &result)
        : 1;
    free(contents.data);
    return status == 0 ? result : fail(env, "Could not create clipboard text");
}

static napi_value has_clipboard_image(napi_env env, napi_callback_info info) {
    (void)info;
    clipboard_state state;
    bool opened = open_clipboard(&state);
    bool available = opened && state.selection && state.selection->image_mime;
    close_clipboard(&state);

    napi_get_boolean_fn napi_get_boolean = (napi_get_boolean_fn)node_symbol("napi_get_boolean");
    napi_value result = 0;
    if (!napi_get_boolean || napi_get_boolean(env, available, &result) != 0) {
        return fail(env, "Could not inspect Wayland clipboard");
    }
    return result;
}

static napi_value get_clipboard_image(napi_env env, napi_callback_info info) {
    (void)info;
    clipboard_bytes contents = {0};
    if (!read_clipboard(true, &contents)) return fail(env, "Wayland clipboard does not contain an image");

    napi_create_buffer_copy_fn napi_create_buffer_copy =
        (napi_create_buffer_copy_fn)node_symbol("napi_create_buffer_copy");
    napi_value result = 0;
    int status = napi_create_buffer_copy
        ? napi_create_buffer_copy(env, contents.length, contents.data, 0, &result)
        : 1;
    free(contents.data);
    return status == 0 ? result : fail(env, "Could not create clipboard image buffer");
}

static void set_function_export(napi_env env, napi_value exports, const char* name, napi_callback callback) {
    napi_create_function_fn napi_create_function = (napi_create_function_fn)node_symbol("napi_create_function");
    napi_set_named_property_fn napi_set_named_property =
        (napi_set_named_property_fn)node_symbol("napi_set_named_property");
    napi_value fn = 0;
    if (napi_create_function && napi_set_named_property &&
        napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, 0, &fn) == 0) {
        napi_set_named_property(env, exports, name, fn);
    }
}

__attribute__((visibility("default"))) napi_value napi_register_module_v1(napi_env env, napi_value exports) {
    set_function_export(env, exports, "isClipboardAvailable", is_clipboard_available);
    set_function_export(env, exports, "getClipboardText", get_clipboard_text);
    set_function_export(env, exports, "hasClipboardImage", has_clipboard_image);
    set_function_export(env, exports, "getClipboardImage", get_clipboard_image);
    return exports;
}
