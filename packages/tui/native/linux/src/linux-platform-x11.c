#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "clipboard-io.h"

#define NAPI_AUTO_LENGTH ((size_t)-1)

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

typedef struct {
    unsigned char* data;
    size_t length;
    uint32_t items;
    uint8_t format;
    xcb_atom_t type;
} property_data;

typedef struct {
    xcb_connection_t* connection;
    xcb_window_t window;
    xcb_atom_t clipboard;
    xcb_atom_t property;
    xcb_atom_t targets;
    xcb_atom_t incr;
    int64_t deadline;
} x11_clipboard;

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

static bool append_bytes(property_data* result, const unsigned char* bytes, size_t length) {
    if (length > MAX_CLIPBOARD_BYTES - result->length) return false;
    unsigned char* data = realloc(result->data, result->length + length + 1);
    if (!data) return false;
    memcpy(data + result->length, bytes, length);
    result->length += length;
    data[result->length] = 0;
    result->data = data;
    return true;
}

// Unlike Xlib, XCB reports broken connections without terminating the process.
static void* wait_for_reply(x11_clipboard* clipboard, unsigned int sequence) {
    if (xcb_flush(clipboard->connection) <= 0) return 0;
    while (monotonic_ms() < clipboard->deadline) {
        void* reply = 0;
        xcb_generic_error_t* error = 0;
        if (xcb_poll_for_reply(clipboard->connection, sequence, &reply, &error)) {
            if (error) {
                free(error);
                free(reply);
                return 0;
            }
            return reply;
        }
        if (xcb_connection_has_error(clipboard->connection) ||
            !(wait_for_fd(xcb_get_file_descriptor(clipboard->connection), POLLIN, clipboard->deadline) & POLLIN)) {
            break;
        }
    }
    return 0;
}

static xcb_generic_event_t* wait_for_event(x11_clipboard* clipboard, uint8_t type) {
    if (xcb_flush(clipboard->connection) <= 0) return 0;
    while (monotonic_ms() < clipboard->deadline) {
        xcb_generic_event_t* event = xcb_poll_for_event(clipboard->connection);
        if (event) {
            if ((event->response_type & 0x7f) == type) return event;
            free(event);
            continue;
        }
        if (xcb_connection_has_error(clipboard->connection) ||
            !(wait_for_fd(xcb_get_file_descriptor(clipboard->connection), POLLIN, clipboard->deadline) & POLLIN)) {
            break;
        }
    }
    return 0;
}

static bool read_property(x11_clipboard* clipboard, bool remove, property_data* result) {
    xcb_get_property_cookie_t cookie = xcb_get_property(
        clipboard->connection, remove, clipboard->window, clipboard->property,
        XCB_GET_PROPERTY_TYPE_ANY, 0, MAX_CLIPBOARD_BYTES / 4
    );
    xcb_get_property_reply_t* reply = wait_for_reply(clipboard, cookie.sequence);
    if (!reply) return false;
    bool copied = false;
    bool valid_format = reply->format == 8 || reply->format == 16 || reply->format == 32;
    uint64_t length = (uint64_t)reply->value_len * (reply->format / 8);
    if (reply->bytes_after == 0 && reply->type != XCB_NONE && valid_format &&
        length <= MAX_CLIPBOARD_BYTES && length <= (uint64_t)reply->length * 4) {
        result->type = reply->type;
        result->format = reply->format;
        result->items = reply->value_len;
        copied = append_bytes(result, xcb_get_property_value(reply), (size_t)length);
    }
    free(reply);
    return copied;
}

static bool append_property(property_data* result, const property_data* chunk) {
    if ((chunk->format != 8 && chunk->format != 16 && chunk->format != 32) ||
        chunk->type == XCB_NONE ||
        (uint64_t)chunk->items * (chunk->format / 8) != chunk->length ||
        (result->type != XCB_NONE && (result->type != chunk->type || result->format != chunk->format)) ||
        chunk->items > UINT32_MAX - result->items) return false;
    if (!append_bytes(result, chunk->data, chunk->length)) return false;
    result->type = chunk->type;
    result->format = chunk->format;
    result->items += chunk->items;
    return true;
}

static bool request_selection(x11_clipboard* clipboard, xcb_atom_t target, property_data* result) {
    memset(result, 0, sizeof(*result));
    xcb_delete_property(clipboard->connection, clipboard->window, clipboard->property);
    xcb_convert_selection(
        clipboard->connection, clipboard->window, clipboard->clipboard,
        target, clipboard->property, XCB_CURRENT_TIME
    );

    while (true) {
        xcb_selection_notify_event_t* event = (xcb_selection_notify_event_t*)wait_for_event(clipboard, XCB_SELECTION_NOTIFY);
        if (!event) return false;
        bool matches = event->selection == clipboard->clipboard && event->target == target;
        bool received = event->property != XCB_NONE;
        free(event);
        if (!matches) continue;
        if (!received || !read_property(clipboard, false, result)) return false;
        break;
    }
    if (result->type != clipboard->incr) return true;

    free(result->data);
    memset(result, 0, sizeof(*result));
    xcb_delete_property(clipboard->connection, clipboard->window, clipboard->property);

    while (true) {
        xcb_property_notify_event_t* event = (xcb_property_notify_event_t*)wait_for_event(clipboard, XCB_PROPERTY_NOTIFY);
        if (!event) return false;
        bool matches = event->atom == clipboard->property && event->state == XCB_PROPERTY_NEW_VALUE;
        free(event);
        if (!matches) continue;

        property_data chunk = {0};
        if (!read_property(clipboard, true, &chunk)) return false;
        bool appended = append_property(result, &chunk);
        bool finished = chunk.items == 0;
        free(chunk.data);
        if (!appended) return false;
        if (finished) return true;
    }
}

static xcb_atom_t intern_atom(x11_clipboard* clipboard, const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(clipboard->connection, false, strlen(name), name);
    xcb_intern_atom_reply_t* reply = wait_for_reply(clipboard, cookie.sequence);
    if (!reply) return XCB_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static bool open_clipboard(x11_clipboard* clipboard) {
    memset(clipboard, 0, sizeof(*clipboard));
    clipboard->deadline = monotonic_ms() + CLIPBOARD_TIMEOUT_MS;
    int screen_number = 0;
    clipboard->connection = xcb_connect(0, &screen_number);
    if (xcb_connection_has_error(clipboard->connection)) return false;
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(clipboard->connection));
    while (screen_number-- > 0 && screens.rem) xcb_screen_next(&screens);
    if (!screens.rem) return false;

    clipboard->window = xcb_generate_id(clipboard->connection);
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(
        clipboard->connection, XCB_COPY_FROM_PARENT, clipboard->window,
        screens.data->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask
    );
    clipboard->clipboard = intern_atom(clipboard, "CLIPBOARD");
    clipboard->property = intern_atom(clipboard, "PI_CLIPBOARD");
    clipboard->targets = intern_atom(clipboard, "TARGETS");
    clipboard->incr = intern_atom(clipboard, "INCR");
    return clipboard->clipboard && clipboard->property && clipboard->targets && clipboard->incr;
}

static void close_clipboard(x11_clipboard* clipboard) {
    if (clipboard->connection) xcb_disconnect(clipboard->connection);
}

static xcb_atom_t preferred_target(x11_clipboard* clipboard, bool image) {
    static const char* text_types[] = {
        "text/plain;charset=utf-8",
        "text/plain;charset=UTF-8",
        "UTF8_STRING",
        "text/plain",
        "STRING",
    };
    static const char* image_types[] = {
        "image/png",
        "image/jpeg",
        "image/webp",
        "image/gif",
        "image/bmp",
        "image/tiff",
    };
    const char** types = image ? image_types : text_types;
    size_t type_count = image ? 6 : 5;
    xcb_atom_t wanted[6];
    for (size_t index = 0; index < type_count; index++) {
        wanted[index] = intern_atom(clipboard, types[index]);
        if (wanted[index] == XCB_NONE) return XCB_NONE;
    }

    property_data targets = {0};
    if (request_selection(clipboard, clipboard->targets, &targets) &&
        targets.type == XCB_ATOM_ATOM && targets.format == 32 &&
        targets.items == targets.length / sizeof(xcb_atom_t) && targets.length % sizeof(xcb_atom_t) == 0) {
        xcb_atom_t* offered = (xcb_atom_t*)targets.data;
        for (size_t wanted_index = 0; wanted_index < type_count; wanted_index++) {
            for (uint32_t offered_index = 0; offered_index < targets.items; offered_index++) {
                if (offered[offered_index] == wanted[wanted_index]) {
                    free(targets.data);
                    return wanted[wanted_index];
                }
            }
        }
    }
    free(targets.data);
    return XCB_NONE;
}

static bool read_clipboard(bool image, property_data* result) {
    x11_clipboard clipboard;
    bool opened = open_clipboard(&clipboard);
    xcb_atom_t target = opened ? preferred_target(&clipboard, image) : XCB_NONE;
    bool received = target != XCB_NONE && request_selection(&clipboard, target, result);
    close_clipboard(&clipboard);
    if (!received) {
        free(result->data);
        memset(result, 0, sizeof(*result));
    }
    return received;
}

typedef enum { CLIPBOARD_PROBE, CLIPBOARD_TEXT, CLIPBOARD_HAS_IMAGE, CLIPBOARD_IMAGE } clipboard_operation;

typedef struct {
    size_t length;
    xcb_atom_t type;
} clipboard_response;

static bool transfer_bytes(int fd, void* buffer, size_t length, bool sending, int64_t deadline) {
    unsigned char* cursor = buffer;
    while (length) {
        short events = sending ? POLLOUT : POLLIN;
        if (!(wait_for_fd(fd, events, deadline) & events)) return false;
        ssize_t count = sending ? send(fd, cursor, length, MSG_NOSIGNAL) : recv(fd, cursor, length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return false;
        cursor += count;
        length -= (size_t)count;
    }
    return true;
}

// XCB's connection setup (including DNS, authentication and the initial server
// reply) and flushes can block outside our polling loops. Keep all XCB work in
// a disposable child, never call Node APIs there, and enforce the deadline in
// the parent. Even a child blocked on inherited library state is terminable.
static bool run_clipboard_operation(clipboard_operation operation, property_data* result) {
    int64_t deadline = monotonic_ms() + CLIPBOARD_TIMEOUT_MS;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) != 0) return false;
    pid_t child = fork();
    if (child == 0) {
        close(sockets[0]);
        property_data contents = {0};
        bool success = true;
        if (operation == CLIPBOARD_TEXT || operation == CLIPBOARD_IMAGE) {
            success = read_clipboard(operation == CLIPBOARD_IMAGE, &contents);
        } else {
            x11_clipboard clipboard;
            bool opened = open_clipboard(&clipboard);
            contents.type = opened && (operation == CLIPBOARD_PROBE || preferred_target(&clipboard, true) != XCB_NONE);
            close_clipboard(&clipboard);
        }
        clipboard_response response = {0};
        response.length = contents.length;
        response.type = contents.type;
        if (success) {
            success = transfer_bytes(sockets[1], &response, sizeof(response), true, deadline) &&
                transfer_bytes(sockets[1], contents.data, contents.length, true, deadline);
        }
        _exit(success ? 0 : 1);
    }
    close(sockets[1]);
    if (child < 0) {
        close(sockets[0]);
        return false;
    }

    clipboard_response response = {0};
    bool received = transfer_bytes(sockets[0], &response, sizeof(response), false, deadline) &&
        response.length <= MAX_CLIPBOARD_BYTES;
    if (received) {
        result->data = malloc(response.length + 1);
        received = result->data && transfer_bytes(sockets[0], result->data, response.length, false, deadline);
        if (received) {
            result->length = response.length;
            result->data[result->length] = 0;
            result->type = response.type;
        }
    }
    close(sockets[0]);
    // The complete response is the commit point. Do not wait for child cleanup
    // or leave stalled children/zombies behind, including after a timeout.
    kill(child, SIGKILL);
    while (waitpid(child, 0, 0) < 0 && errno == EINTR) {}
    if (!received) {
        free(result->data);
        memset(result, 0, sizeof(*result));
    }
    return received;
}

static napi_value is_clipboard_available(napi_env env, napi_callback_info info) {
    (void)info;
    property_data contents = {0};
    bool available = run_clipboard_operation(CLIPBOARD_PROBE, &contents) && contents.type != 0;
    free(contents.data);

    napi_get_boolean_fn napi_get_boolean = (napi_get_boolean_fn)node_symbol("napi_get_boolean");
    napi_value result = 0;
    if (!napi_get_boolean || napi_get_boolean(env, available, &result) != 0) {
        return fail(env, "Could not inspect X11 clipboard availability");
    }
    return result;
}

static napi_value get_clipboard_text(napi_env env, napi_callback_info info) {
    (void)info;
    property_data contents = {0};
    if (!run_clipboard_operation(CLIPBOARD_TEXT, &contents)) return fail(env, "Could not read X11 clipboard text");

    // X11 STRING is ISO-8859-1, not UTF-8. Both N-API constructors share a signature.
    napi_create_string_utf8_fn create_string = (napi_create_string_utf8_fn)node_symbol(
        contents.type == XCB_ATOM_STRING ? "napi_create_string_latin1" : "napi_create_string_utf8"
    );
    napi_value result = 0;
    int status = create_string ? create_string(env, (const char*)contents.data, contents.length, &result) : 1;
    free(contents.data);
    return status == 0 ? result : fail(env, "Could not create clipboard text");
}

static napi_value has_clipboard_image(napi_env env, napi_callback_info info) {
    (void)info;
    property_data contents = {0};
    if (!run_clipboard_operation(CLIPBOARD_HAS_IMAGE, &contents)) return fail(env, "Could not inspect X11 clipboard");
    bool available = contents.type != 0;
    free(contents.data);

    napi_get_boolean_fn napi_get_boolean = (napi_get_boolean_fn)node_symbol("napi_get_boolean");
    napi_value result = 0;
    if (!napi_get_boolean || napi_get_boolean(env, available, &result) != 0) {
        return fail(env, "Could not inspect X11 clipboard");
    }
    return result;
}

static napi_value get_clipboard_image(napi_env env, napi_callback_info info) {
    (void)info;
    property_data contents = {0};
    if (!run_clipboard_operation(CLIPBOARD_IMAGE, &contents)) return fail(env, "Could not read X11 clipboard image");

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
