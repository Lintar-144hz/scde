# SCDE App API and plugin system

SCDE exposes a small native C API so applications and plugins can talk to the
desktop without linking against SCDE internals.  Everything lives in
`include/scde/*.h`; nothing under `src/` is public.

```
include/scde/common.h         version, status codes, logging
include/scde/event.h          event queue, subscriptions, dispatch
include/scde/notification.h   fixed notification ring
include/scde/config.h         read-only key=value config access
include/scde/window.h         minimal top-level window creation
include/scde/app.h            app lifecycle, services, scde_api_t table
include/scde/plugin.h         plugin ABI + plugin manager interface
```

The implementation is `src/api/*.c` + `src/plugin/*.c`, compiled into
`build/libscdeapp.a` (linked into `scde`, `tests/api_test`, and available to
third-party programs after `make install`).

Design rules:

* **No allocation.** Every table is fixed size and static (config 128 keys,
  events 64/64, notifications 32, services 16, windows 8, plugins 32).
  Exhaustion returns `SCDE_ERR_FULL`, never a crash.
* **Headless-safe.** Only the window API touches X, through its own
  `Display *`; without `$DISPLAY` every call fails with `SCDE_ERR_DISPLAY`.
* **Single-threaded**, like the rest of SCDE.
* **Versioned.** `SCDE_API_VERSION_MAJOR` / `MINOR` (`1.0`), checked with
  `scde_api_version_check()`.

## Version and status codes

```c
int rc = scde_api_version_check(SCDE_API_VERSION_MAJOR,
                                SCDE_API_VERSION_MINOR);   /* SCDE_OK */
if (rc != SCDE_OK)
        fprintf(stderr, "%s\n", scde_strerror(rc));
```

| code | meaning |
|------|---------|
| `SCDE_OK` | success |
| `SCDE_ERR` | generic failure |
| `SCDE_ERR_ARG` | bad argument |
| `SCDE_ERR_STATE` | not valid in the current state |
| `SCDE_ERR_DISPLAY` | no X display available |
| `SCDE_ERR_NOT_FOUND` | unknown key / service / file |
| `SCDE_ERR_VERSION` | API or plugin ABI mismatch |
| `SCDE_ERR_IO` | read/write or loader failure |
| `SCDE_ERR_FULL` | fixed-size table exhausted |

Logging: `scde_log(level, msg)` / `scde_logf(level, fmt, ...)`, sink
replaceable with `scde_log_set_sink()` (NULL restores stderr).

## Applications

```c
#include <scde/app.h>

scde_app_info_t info = { "myapp", "0.1", "what it does", "me" };

scde_app_init(&info);                 /* strings are copied */
/* ... */
scde_app_shutdown();                  /* idempotent */
```

`scde_app_init()` also registers the built-in services:

| service | args | out |
|---------|------|-----|
| `version` | – | API version string |
| `notify` | `"title\nbody"` (or just `title`) | – |
| `config.get` | config key | value (`SCDE_ERR_NOT_FOUND` if absent) |

Own services: `scde_service_register(name, fn, user)`,
`scde_service_unregister(name)`, `scde_service_call(name, args, out, len)`.

## Events

```c
static void on_event(const scde_event_t *ev, void *user) { ... }

scde_event_subscribe(SCDE_EVENT_ANY, on_event, NULL);   /* or a single type */
scde_event_post(&ev);                 /* queue, oldest dropped when full */
scde_event_dispatch();                /* delivers queued events, returns # */
```

Types: `SCDE_EVENT_APP_INIT/SHUTDOWN`, `NOTIFICATION`, `CONFIG_CHANGED`,
`PLUGIN_LOADED`, `PLUGIN_FAILED`, `WINDOW_EXPOSE/CLOSE/BUTTON/KEY/CONFIGURE`,
`SCDE_EVENT_CUSTOM` (1000+, for applications).
`SCDE_EVENT_ANY` is subscription-only and can never be posted.

## Notifications

```c
scde_notify("Title", "body");          /* ring (32) + log + event */
scde_notify_last(&n);                  /* newest, SCDE_ERR_NOT_FOUND if empty */
```

The ring keeps the newest 32 entries; `scde_notify_total()` counts all since
process start.

## Configuration

Same `key = value` file the session uses (`$SCDE_CONFIG` or
`~/.config/scde/config`), read independently of the window manager:

```c
scde_config_load(NULL);                /* or an explicit path */
const char *v = scde_config_get("name", "default");
int n = scde_config_get_int("gap", 4);
int on = scde_config_get_bool("focus_follows_mouse", 1);
```

`scde_config_load()` posts `SCDE_EVENT_CONFIG_CHANGED`.

## Windows

```c
scde_window_attrs_t a = {
    .x = SCDE_WINDOW_AUTO, .y = SCDE_WINDOW_AUTO,
    .width = 640, .height = 400, .title = "hello",
    .background = 0x1c1f26
};
scde_window_t *w = NULL;
if (scde_window_create(&a, &w) == SCDE_OK) {
        scde_window_show(w);
        while (running) {
                scde_window_process_events(w, 0);   /* -> scde_event_t */
                scde_event_dispatch();
        }
        scde_window_destroy(w);
}
```

SCDE's own event loop dispatches App API events every iteration, but it does
**not** pump API windows created by plugins (see Limitations).

## Plugins

A plugin is one shared object exporting exactly three symbols:

```c
SCDE_PLUGIN_EXPORT const scde_plugin_info_t *scde_plugin_info(void);
SCDE_PLUGIN_EXPORT int  scde_plugin_init(const scde_api_t *api);
SCDE_PLUGIN_EXPORT void scde_plugin_shutdown(void);
```

`scde_plugin_info()` returns static metadata:

```c
static const scde_plugin_info_t info = {
        .info_size   = sizeof(scde_plugin_info_t),
        .abi_version = SCDE_PLUGIN_ABI_VERSION,     /* 1 */
        .name        = "example",
        .version     = "1.0.0",
        .description = "Minimal SCDE example plugin",
        .api_major   = SCDE_API_VERSION_MAJOR,
        .api_minor   = SCDE_API_VERSION_MINOR
};
```

The host loads the file with `dlopen(RTLD_NOW|RTLD_LOCAL)` and validates, in
order, before `init()` is ever called:

1. the three entry points exist,
2. `info_size` covers at least the v1 layout,
3. `abi_version == SCDE_PLUGIN_ABI_VERSION`,
4. name and version are non-empty,
5. required API version passes `scde_api_version_check()`.

Then `scde_plugin_init(api)` runs: `0` marks the plugin **running**, any
other value marks it **failed** (and `shutdown()` is *not* called).
Failures are isolated - the offending plugin is logged and reported through
`SCDE_EVENT_PLUGIN_FAILED`, SCDE keeps running.

Plugins talk to SCDE **only** through the `scde_api_t` table passed to
`init()` (`version_check`, `log`, `notify`, `config_get`, `config_get_int`,
`event_subscribe`, `event_post`, `service_call`).  They never link SCDE
symbols, so a plugin build needs nothing but libc and the public headers:

```sh
gcc -Iinclude -fPIC -shared -o myplugin.so myplugin.c
```

### Manager

```c
scde_plugins_init(NULL);      /* path list, or $SCDE_PLUGIN_PATH, or defaults */
scde_plugins_discover();      /* scan for *.so, sorted by name */
scde_plugins_load_all();      /* never fails because a plugin fails */
scde_plugins_unload("example");
scde_plugins_shutdown();
```

Default search path: `~/.local/share/scde/plugins:/usr/local/lib/scde/plugins:/usr/lib/scde/plugins`.

States (`scde_plugin_state_name()` gives the string):

```
DISCOVERED -> LOADED -> RUNNING      (init returned 0)
     \           \-> FAILED          (open/validate/init failed)
RUNNING -> UNLOADED                  (shutdown + dlclose)
```

Inspection: `scde_plugins_count()`, `scde_plugins_running()`,
`scde_plugins_state(i)`, `scde_plugins_info(i)`, `scde_plugins_error(i)`,
`scde_plugins_file(i)`.

`scde` itself initializes the manager at startup
(`src/main.c`): `scde_app_init()`, then discover + load before the event
loop, and shut everything down on exit.

## Building and testing

```sh
make            # scde, test helpers, build/libscdeapp.a, plugins
make plugins    # only build/plugins/{libscde-example,test-badver,test-failinit}.so
make test       # HAL tests, App API tests, then the Xvfb suite
```

`tests/api_test <example.so> <badver.so> <failinit.so>` runs headless
(no X server needed) and covers version/status helpers, app lifecycle,
config, events, notifications, services, window calls without a display,
plugin discovery of junk files, and the real plugins:

* `plugins/example/example.c` - loads, logs, notifies, shuts down,
* `tests/plugins/badver.c` - requires API 99.0, rejected before `init()`,
* `tests/plugins/failinit.c` - `init()` returns non-zero, never running.

## Limitations

* The window API is minimal (create/show/hide/title/size/background and
  event translation); there are no widgets, no ICCCM/EWMH beyond
  `WM_DELETE_WINDOW`, and plugin windows are not pumped by SCDE's loop.
* Configuration access is read-only; changes are made by editing the file
  (the reload posts `CONFIG_CHANGED`).
* The plugin ABI is version 1 with a fixed 32-entry table; there is no
  sandboxing, per-plugin capabilities, or dependency resolution yet.
* Everything is single-threaded and fixed-size; long input is truncated by
  the static buffers rather than rejected.
