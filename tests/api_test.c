/*
 * SCDE App API + plugin system tests.
 *
 * Headless: no X server is needed.  The window tests only check that
 * everything fails cleanly without a DISPLAY.
 *
 * Usage:  tests/api_test <example.so> <badver.so> <failinit.so>
 * Without the plugin paths the tests that need real plugins are skipped.
 */
#include <scde/app.h>
#include <scde/common.h>
#include <scde/config.h>
#include <scde/event.h>
#include <scde/notification.h>
#include <scde/plugin.h>
#include <scde/security.h>
#include <scde/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));           \
        }                                                                    \
    } while (0)

#define CHECK_INT(got, want, msg)                                            \
    do {                                                                     \
        long g_ = (long)(got);                                               \
        long w_ = (long)(want);                                              \
        checks++;                                                            \
        if (g_ != w_) {                                                      \
            failures++;                                                      \
            printf("FAIL %s:%d: %s (got %ld, want %ld)\n", __FILE__,         \
                   __LINE__, (msg), g_, w_);                                 \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want, msg)                                            \
    do {                                                                     \
        const char *g_ = (got);                                              \
        const char *w_ = (want);                                             \
        checks++;                                                            \
        if (!g_ || !w_ || strcmp(g_, w_) != 0) {                             \
            failures++;                                                      \
            printf("FAIL %s:%d: %s (got \"%s\", want \"%s\")\n", __FILE__,   \
                   __LINE__, (msg), g_ ? g_ : "(null)",                      \
                   w_ ? w_ : "(null)");                                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------- log capture -- */

#define LOG_CAP 256
static char log_buf[LOG_CAP][256];
static int log_n;

static void log_sink(scde_log_level_t level, const char *message, void *user)
{
    (void)level;
    (void)user;
    if (log_n < LOG_CAP)
        snprintf(log_buf[log_n], sizeof(log_buf[0]), "%s",
                 message ? message : "");
    log_n++;
}

static int log_contains(const char *needle)
{
    int i;

    for (i = 0; i < log_n && i < LOG_CAP; i++) {
        if (strstr(log_buf[i], needle))
            return 1;
    }
    return 0;
}

/* -------------------------------------------------------- event taps -- */

static int ev_any;
static int ev_note;
static int ev_plugin_loaded;
static int ev_plugin_failed;
static int ev_any_type;
static uint64_t ev_any_id;
static char ev_any_text[160];

static void cb_any(const scde_event_t *ev, void *user)
{
    (void)user;
    ev_any++;
    ev_any_type = ev->type;
    ev_any_id = ev->id;
    snprintf(ev_any_text, sizeof(ev_any_text), "%s", ev->text);
}

static void cb_note(const scde_event_t *ev, void *user)
{
    (void)ev;
    (void)user;
    ev_note++;
}

static void cb_plugin(const scde_event_t *ev, void *user)
{
    (void)user;
    if (ev->type == SCDE_EVENT_PLUGIN_LOADED)
        ev_plugin_loaded++;
    else if (ev->type == SCDE_EVENT_PLUGIN_FAILED)
        ev_plugin_failed++;
}

static int echo_service(const char *args, char *out, size_t out_len,
                        void *user)
{
    (void)user;
    if (!out || out_len == 0)
        return SCDE_ERR_ARG;
    snprintf(out, out_len, "%s", args ? args : "");
    return SCDE_OK;
}

static int str_ends(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t fl = strlen(suffix);

    return sl >= fl && strcmp(s + sl - fl, suffix) == 0;
}

/* -------------------------------------------------------------- tests -- */

static void test_version(void)
{
    const scde_api_t *api;

    CHECK_INT(scde_api_version_check(SCDE_API_VERSION_MAJOR,
                                     SCDE_API_VERSION_MINOR), SCDE_OK,
              "current version accepted");
    CHECK_INT(scde_api_version_check(SCDE_API_VERSION_MAJOR,
                                     SCDE_API_VERSION_MINOR + 1),
              SCDE_ERR_VERSION, "newer minor rejected");
    CHECK_INT(scde_api_version_check(SCDE_API_VERSION_MAJOR + 1, 0),
              SCDE_ERR_VERSION, "newer major rejected");
    CHECK_INT(scde_api_version_check(SCDE_API_VERSION_MAJOR - 1, 0),
              SCDE_ERR_VERSION, "older major rejected");
    CHECK_STR(scde_api_version_string(), SCDE_API_VERSION_STRING,
              "version string");

    CHECK_STR(scde_strerror(SCDE_OK), "success", "strerror ok");
    CHECK_STR(scde_strerror(SCDE_ERR_VERSION), "version mismatch",
              "strerror version");
    CHECK_STR(scde_strerror(SCDE_ERR_DISPLAY), "no display available",
              "strerror display");
    CHECK_STR(scde_strerror(-999), "unknown status", "strerror unknown");

    api = scde_api_table();
    CHECK(api != NULL, "api table exists");
    if (api) {
        CHECK_INT(api->version_major, SCDE_API_VERSION_MAJOR, "table major");
        CHECK_INT(api->version_minor, SCDE_API_VERSION_MINOR, "table minor");
        CHECK(api->version_check && api->log && api->notify &&
                  api->config_get && api->config_get_int &&
                  api->event_subscribe && api->event_post &&
                  api->service_call,
              "table entries complete");
        CHECK_INT(api->version_check(SCDE_API_VERSION_MAJOR,
                                     SCDE_API_VERSION_MINOR), SCDE_OK,
                  "table version check");
    }
}

static void test_app_lifecycle(void)
{
    char name_buf[] = "apitest";
    char ver_buf[] = "2.5";
    scde_app_info_t info = { name_buf, ver_buf, "a test app", "tester" };
    scde_app_info_t bad = { "", "1.0", NULL, NULL };

    CHECK_INT(scde_app_is_running(), 0, "not running at start");
    CHECK(scde_app_info() == NULL, "no info at start");

    CHECK_INT(scde_app_init(&info), SCDE_OK, "init");
    CHECK_INT(scde_app_is_running(), 1, "running");
    CHECK(scde_app_info() != NULL, "info available");
    CHECK_STR(scde_app_info()->name, "apitest", "name");
    CHECK_STR(scde_app_info()->version, "2.5", "version");

    name_buf[0] = 'X';                        /* caller memory may go away */
    CHECK_STR(scde_app_info()->name, "apitest", "info strings copied");
    name_buf[0] = 'a';

    CHECK_INT(scde_app_init(&info), SCDE_ERR_STATE, "double init rejected");
    scde_app_shutdown();
    CHECK_INT(scde_app_is_running(), 0, "stopped");
    CHECK(scde_app_info() == NULL, "info gone");
    scde_app_shutdown();                      /* idempotent */

    CHECK_INT(scde_app_init(&bad), SCDE_ERR_ARG, "empty name rejected");
    CHECK_INT(scde_app_init(NULL), SCDE_OK, "anonymous init");
    CHECK_STR(scde_app_info()->name, "", "empty name stored");
    scde_app_shutdown();
    CHECK_INT(scde_app_is_running(), 0, "stopped again");
}

static void test_config(const char *cfg_path)
{
    CHECK_INT(scde_config_load(cfg_path), SCDE_OK, "load");
    CHECK(scde_config_count() >= 5, "keys parsed");
    CHECK_STR(scde_config_get("name", NULL), "hello world", "string value");
    CHECK_STR(scde_config_get("num", NULL), "-42", "raw int value");
    CHECK_INT(scde_config_get_int("num", 0), -42, "int value");
    CHECK_INT(scde_config_get_bool("yes", -1), 1, "bool true");
    CHECK_INT(scde_config_get_bool("no", -1), 0, "bool false");
    CHECK_INT(scde_config_get_bool("flag", -1), 1, "bool on");
    CHECK_STR(scde_config_get("missing", "fallback"), "fallback",
              "default string");
    CHECK_INT(scde_config_get_int("missing", 7), 7, "default int");
    CHECK_INT(scde_config_get_bool("missing", 1), 1, "default bool");
    CHECK_INT(scde_config_has("name"), 1, "has known key");
    CHECK_INT(scde_config_has("nope"), 0, "no unknown key");
    CHECK(scde_config_key(0) != NULL, "first key");
    CHECK(scde_config_value(0) != NULL, "first value");
    CHECK(scde_config_key(9999) == NULL, "key out of range");
    CHECK_STR(scde_config_path(), cfg_path, "path kept");

    CHECK_INT(scde_config_load("/nonexistent/scde-api-test.cfg"),
              SCDE_ERR_NOT_FOUND, "missing file");
    CHECK_INT(scde_config_count(), 0, "table cleared on failure");
    CHECK_INT(scde_config_load(cfg_path), SCDE_OK, "load again");
    CHECK_INT(scde_config_reload(), SCDE_OK, "reload");

    scde_config_clear();
    CHECK_INT(scde_config_count(), 0, "cleared");
    CHECK_STR(scde_config_path(), "", "path cleared");
    CHECK_INT(scde_config_reload(), SCDE_ERR_STATE, "reload needs a path");
}

static void test_events(void)
{
    scde_event_t ev;

    scde_event_clear();
    ev_any = ev_note = 0;

    CHECK_INT(scde_event_subscribe(SCDE_EVENT_ANY, cb_any, NULL), SCDE_OK,
              "subscribe any");
    CHECK_INT(scde_event_subscribe(SCDE_EVENT_NOTIFICATION, cb_note, NULL),
              SCDE_OK, "subscribe notification");
    CHECK_INT(scde_event_subscribe(SCDE_EVENT_ANY, NULL, NULL), SCDE_ERR_ARG,
              "subscribe without fn");
    CHECK_INT(scde_event_subscribe(-1, cb_any, NULL), SCDE_ERR_ARG,
              "subscribe negative type");

    scde_notify_clear();
    CHECK_INT(scde_notify("Title", "Body"), SCDE_OK, "notify");
    CHECK_INT(scde_event_pending(), 1, "event queued");
    CHECK_INT(scde_event_dispatch(), 2, "delivered to both subscribers");
    CHECK_INT(ev_any, 1, "any got it");
    CHECK_INT(ev_note, 1, "notification got it");
    CHECK_INT(ev_any_type, SCDE_EVENT_NOTIFICATION, "event type");
    CHECK(ev_any_id != 0, "notification id set");
    CHECK_STR(ev_any_text, "Title: Body", "text payload");

    memset(&ev, 0, sizeof(ev));
    ev.type = SCDE_EVENT_CUSTOM;
    ev.x = 42;
    snprintf(ev.text, sizeof(ev.text), "custom");
    CHECK_INT(scde_event_post(&ev), SCDE_OK, "post custom");
    CHECK_INT(scde_event_post(NULL), SCDE_ERR_ARG, "post NULL");
    ev.type = SCDE_EVENT_ANY;
    CHECK_INT(scde_event_post(&ev), SCDE_ERR_ARG, "cannot post ANY");
    CHECK_INT(scde_event_dispatch(), 1, "type filter respected");
    CHECK_INT(ev_any, 2, "any counted");
    CHECK_INT(ev_note, 1, "note not matched");
    CHECK_STR(ev_any_text, "custom", "custom text");

    CHECK_INT(scde_event_unsubscribe(cb_note, NULL), SCDE_OK, "unsubscribe");
    CHECK_INT(scde_event_unsubscribe(cb_note, NULL), SCDE_ERR_NOT_FOUND,
              "unsubscribe twice");
    CHECK_INT(scde_event_unsubscribe(NULL, NULL), SCDE_ERR_ARG,
              "unsubscribe without fn");

    scde_notify("T2", "B2");
    CHECK_INT(scde_event_dispatch(), 1, "only the wildcard left");
    CHECK_INT(ev_any, 3, "wildcard counted");
    CHECK_INT(ev_note, 1, "unsubscribed tap silent");

    scde_event_clear();
    memset(&ev, 0, sizeof(ev));
    ev.type = SCDE_EVENT_CUSTOM;
    for (int i = 0; i < 100; i++)
        CHECK_INT(scde_event_post(&ev), SCDE_OK, "post under load");
    CHECK_INT(scde_event_pending(), 64, "queue capped at 64");
    scde_event_clear();
    CHECK_INT(scde_event_pending(), 0, "queue cleared");

    CHECK_INT(scde_event_unsubscribe(cb_any, NULL), SCDE_OK,
              "unsubscribe any");
    scde_notify_clear();
}

static void test_services(const char *cfg_path)
{
    char out[128];
    uint64_t n0;

    CHECK_INT(scde_app_init(NULL), SCDE_OK, "app init");
    CHECK_INT(scde_service_count(), 3, "built-in services");

    CHECK_INT(scde_service_call("version", NULL, out, sizeof(out)), SCDE_OK,
              "version service");
    CHECK_STR(out, SCDE_API_VERSION_STRING, "version output");
    CHECK_INT(scde_service_call("nosuch", NULL, out, sizeof(out)),
              SCDE_ERR_NOT_FOUND, "unknown service");
    CHECK_INT(scde_service_call(NULL, NULL, out, sizeof(out)), SCDE_ERR_ARG,
              "service without name");

    CHECK_INT(scde_service_register("echo", echo_service, NULL), SCDE_OK,
              "register");
    CHECK_INT(scde_service_register("echo", echo_service, NULL),
              SCDE_ERR_STATE, "duplicate rejected");
    CHECK_INT(scde_service_register("bad", NULL, NULL), SCDE_ERR_ARG,
              "register without fn");
    CHECK_INT(scde_service_count(), 4, "one added");
    CHECK_INT(scde_service_call("echo", "ping", out, sizeof(out)), SCDE_OK,
              "call echo");
    CHECK_STR(out, "ping", "echo output");
    CHECK_INT(scde_service_unregister("echo"), SCDE_OK, "unregister");
    CHECK_INT(scde_service_unregister("echo"), SCDE_ERR_NOT_FOUND,
              "unregister twice");
    CHECK_INT(scde_service_unregister(NULL), SCDE_ERR_ARG,
              "unregister without name");

    CHECK_INT(scde_config_load(cfg_path), SCDE_OK, "config for service");
    CHECK_INT(scde_service_call("config.get", "name", out, sizeof(out)),
              SCDE_OK, "config.get");
    CHECK_STR(out, "hello world", "config.get output");
    CHECK_INT(scde_service_call("config.get", "nokey", out, sizeof(out)),
              SCDE_ERR_NOT_FOUND, "config.get unknown key");
    CHECK_INT(scde_service_call("config.get", NULL, out, sizeof(out)),
              SCDE_ERR_ARG, "config.get without key");

    scde_notify_clear();
    n0 = scde_notify_total();
    CHECK_INT(scde_service_call("notify", "Service\nbody", out, sizeof(out)),
              SCDE_OK, "notify service");
    CHECK_INT(scde_notify_total(), n0 + 1, "one notification posted");
    CHECK_INT(scde_notify_count(), 1, "notification stored");
    CHECK_INT(scde_service_call("notify", NULL, out, sizeof(out)),
              SCDE_ERR_ARG, "notify without title");

    scde_app_shutdown();
    CHECK_INT(scde_service_count(), 0, "services cleared");
    CHECK_INT(scde_service_call("version", NULL, out, sizeof(out)),
              SCDE_ERR_NOT_FOUND, "services gone");
    scde_config_clear();
    scde_notify_clear();
}

static void test_window_no_display(void)
{
    const char *disp = getenv("DISPLAY");
    char saved[64];
    scde_window_attrs_t attrs;
    scde_window_t *w = (scde_window_t *)&attrs;   /* cleared on failure */

    saved[0] = '\0';
    if (disp)
        snprintf(saved, sizeof(saved), "%s", disp);
    unsetenv("DISPLAY");

    CHECK_INT(scde_api_has_display(), 0, "no display open");
    CHECK_INT(scde_api_open_display(), SCDE_ERR_DISPLAY, "open fails");

    memset(&attrs, 0, sizeof(attrs));
    attrs.x = SCDE_WINDOW_AUTO;
    attrs.y = SCDE_WINDOW_AUTO;
    attrs.width = 100;
    attrs.height = 80;
    attrs.title = "headless";
    CHECK_INT(scde_window_create(&attrs, &w), SCDE_ERR_DISPLAY,
              "create fails without display");
    CHECK(w == NULL, "out cleared on failure");

    CHECK_INT(scde_window_create(NULL, NULL), SCDE_ERR_ARG, "create NULL out");
    attrs.width = 0;
    CHECK_INT(scde_window_create(&attrs, &w), SCDE_ERR_ARG, "zero size");
    CHECK_INT(scde_window_show(NULL), SCDE_ERR_ARG, "show NULL");
    CHECK_INT(scde_window_hide(NULL), SCDE_ERR_ARG, "hide NULL");
    CHECK_INT(scde_window_set_title(NULL, "t"), SCDE_ERR_ARG, "title NULL");
    CHECK_INT(scde_window_set_size(NULL, 1, 1), SCDE_ERR_ARG, "size NULL");
    CHECK_INT(scde_window_set_background(NULL, 0xff0000UL), SCDE_ERR_ARG,
              "background NULL");
    CHECK_INT(scde_window_id(NULL), 0, "id of NULL");
    CHECK_INT(scde_window_process_events(NULL, 0), SCDE_ERR_ARG,
              "process NULL");

    if (saved[0])
        setenv("DISPLAY", saved, 1);
}

static void test_plugins_invalid(const char *dir)
{
    char p1[512];
    char p2[512];
    int i;

    snprintf(p1, sizeof(p1), "%s/junk1.so", dir);
    snprintf(p2, sizeof(p2), "%s/junk2.so", dir);

    CHECK_INT(scde_plugins_init(dir), SCDE_OK, "init");
    CHECK_INT(scde_plugins_init(dir), SCDE_ERR_STATE, "double init");
    CHECK_STR(scde_plugins_search_path(), dir, "search path");
    CHECK_INT(scde_plugins_discover(), SCDE_OK, "discover");
    CHECK_INT(scde_plugins_count(), 2, "two junk files found");
    for (i = 0; i < 2; i++) {
        CHECK_INT(scde_plugins_state(i), SCDE_PLUGIN_DISCOVERED,
                  "state discovered");
        CHECK(scde_plugins_info(i) == NULL, "no info before opening");
        CHECK(str_ends(scde_plugins_file(i), ".so"), "file path ends .so");
        CHECK_STR(scde_plugin_state_name(scde_plugins_state(i)),
                  "discovered", "state name");
    }

    CHECK_INT(scde_plugins_load_all(), SCDE_OK, "load_all never fatal");
    CHECK_INT(scde_plugins_running(), 0, "nothing running");
    for (i = 0; i < 2; i++) {
        CHECK_INT(scde_plugins_state(i), SCDE_PLUGIN_FAILED, "state failed");
        CHECK(strlen(scde_plugins_error(i)) > 0, "failure explained");
        CHECK_STR(scde_plugin_state_name(scde_plugins_state(i)), "failed",
                  "state name failed");
    }

    CHECK_INT(scde_plugins_load("/no/such/plugin.so"), SCDE_ERR_NOT_FOUND,
              "missing file");
    CHECK_INT(scde_plugins_load(NULL), SCDE_ERR_ARG, "load without path");
    CHECK_INT(scde_plugins_unload("junk1"), SCDE_ERR_NOT_FOUND,
              "unknown plugin name");
    CHECK_INT(scde_plugins_count(), 2, "count stable");
    CHECK_INT(scde_plugins_unload_all(), SCDE_OK, "unload_all");

    scde_plugins_shutdown();
    CHECK_INT(scde_plugins_count(), 0, "forgotten");
    CHECK_INT(scde_plugins_discover(), SCDE_ERR_STATE, "manager is down");

    CHECK_INT(scde_plugins_init(NULL), SCDE_OK, "init with default path");
    CHECK_INT(scde_plugins_discover(), SCDE_OK, "default path discover");
    scde_plugins_shutdown();
}

static int idx_by_suffix(const char *suffix)
{
    int i;
    int n = scde_plugins_count();

    for (i = 0; i < n; i++) {
        if (str_ends(scde_plugins_file(i), suffix))
            return i;
    }
    return -1;
}

static void test_plugins_real(const char *dir, const char *example_path,
                              const char *badver_path,
                              const char *failinit_path)
{
    const scde_plugin_info_t *pi;
    uint64_t n0;
    int ex_i, bv_i, fi_i;
    char *ex_r = NULL, *bv_r = NULL, *fi_r = NULL;
    int have_real;

    /*
     * In-process dlopen is TRUSTED-ONLY by default (security gate): the
     * fixtures must be trusted explicitly, or the loader errors below
     * would never be reached.  realpath() resolves on the heap: a stack
     * buffer smaller than PATH_MAX trips FORTIFY's __realpath_chk.
     */
    ex_r = realpath(example_path, NULL);
    bv_r = realpath(badver_path, NULL);
    fi_r = realpath(failinit_path, NULL);
    have_real = (ex_r != NULL && bv_r != NULL && fi_r != NULL);
    CHECK(have_real, "realpath fixtures");
    if (have_real) {
        CHECK_INT(scde_trust_set(ex_r, SCDE_TRUST_TRUSTED), SCDE_OK,
                  "trust example fixture");
        CHECK_INT(scde_trust_set(bv_r, SCDE_TRUST_TRUSTED), SCDE_OK,
                  "trust badver fixture");
        CHECK_INT(scde_trust_set(fi_r, SCDE_TRUST_TRUSTED), SCDE_OK,
                  "trust failinit fixture");
    }
    free(ex_r);
    free(bv_r);
    free(fi_r);
    if (!have_real)
        goto done;

    scde_event_clear();
    ev_plugin_loaded = ev_plugin_failed = 0;
    CHECK_INT(scde_event_subscribe(SCDE_EVENT_ANY, cb_plugin, NULL), SCDE_OK,
              "subscribe plugin events");
    n0 = scde_notify_total();
    log_n = 0;

    CHECK_INT(scde_plugins_init(dir), SCDE_OK, "init");
    CHECK_INT(scde_plugins_discover(), SCDE_OK, "discover");
    CHECK_INT(scde_plugins_count(), 3, "three plugins found");
    ex_i = idx_by_suffix("libscde-example.so");
    bv_i = idx_by_suffix("test-badver.so");
    fi_i = idx_by_suffix("test-failinit.so");
    CHECK(ex_i >= 0, "example discovered");
    CHECK(bv_i >= 0, "badver discovered");
    CHECK(fi_i >= 0, "failinit discovered");
    if (ex_i < 0 || bv_i < 0 || fi_i < 0)
        goto done;
    CHECK_INT(scde_plugins_state(ex_i), SCDE_PLUGIN_DISCOVERED,
              "example not opened yet");
    CHECK(scde_plugins_info(ex_i) == NULL, "info only after opening");

    CHECK_INT(scde_plugins_load_all(), SCDE_OK, "load_all");
    CHECK_INT(scde_plugins_running(), 1, "one running");
    CHECK_INT(scde_plugins_state(ex_i), SCDE_PLUGIN_RUNNING,
              "example running");
    CHECK_INT(scde_plugins_state(bv_i), SCDE_PLUGIN_FAILED, "badver failed");
    CHECK_INT(scde_plugins_state(fi_i), SCDE_PLUGIN_FAILED,
              "failinit failed");
    CHECK(strstr(scde_plugins_error(bv_i), "host provides") != NULL,
          "version mismatch explained");
    CHECK(strstr(scde_plugins_error(fi_i), "init failed") != NULL,
          "init failure explained");
    CHECK_STR(scde_plugin_state_name(scde_plugins_state(ex_i)), "running",
              "state name running");

    pi = scde_plugins_info(ex_i);
    CHECK(pi != NULL, "example metadata");
    if (pi) {
        CHECK_STR(pi->name, "example", "plugin name");
        CHECK_STR(pi->version, "1.0.0", "plugin version");
        CHECK_STR(pi->description, "Minimal SCDE example plugin",
                  "plugin description");
        CHECK_INT(pi->abi_version, SCDE_PLUGIN_ABI_VERSION, "plugin abi");
        CHECK_INT(pi->api_major, SCDE_API_VERSION_MAJOR, "plugin api major");
        CHECK_INT(pi->api_minor, SCDE_API_VERSION_MINOR, "plugin api minor");
        CHECK_INT(pi->info_size, sizeof(scde_plugin_info_t), "info size");
    }
    pi = scde_plugins_info(bv_i);
    CHECK(pi != NULL, "badver metadata kept after rejection");
    if (pi) {
        CHECK_STR(pi->name, "badver", "rejected plugin name");
        CHECK_INT(pi->api_major, 99, "rejected api major");
    }

    CHECK(log_contains("example: initialized"), "plugin log captured");
    CHECK_INT(scde_notify_total(), n0 + 1, "example posted one notification");
    CHECK_INT(scde_event_dispatch(), 4,
              "4 events (1 loaded, 2 failed, 1 notification) x 1 tap");
    CHECK_INT(ev_plugin_loaded, 1, "loaded event");
    CHECK_INT(ev_plugin_failed, 2, "two failed events");

    CHECK_INT(scde_plugins_unload("example"), SCDE_OK, "unload by name");
    CHECK_INT(scde_plugins_state(ex_i), SCDE_PLUGIN_UNLOADED, "unloaded");
    CHECK_INT(scde_plugins_running(), 0, "none running");
    CHECK(log_contains("example: shutdown"), "shutdown logged");
    pi = scde_plugins_info(ex_i);
    CHECK(pi != NULL && pi->name && strcmp(pi->name, "example") == 0,
          "metadata survives unload");
    CHECK_INT(scde_plugins_unload("example"), SCDE_ERR_STATE,
              "unload twice");

    CHECK_INT(scde_plugins_load(example_path), SCDE_OK, "reload by path");
    CHECK_INT(scde_plugins_state(ex_i), SCDE_PLUGIN_RUNNING, "running again");
    CHECK_INT(scde_plugins_load(example_path), SCDE_ERR_STATE,
              "already running");

    CHECK_INT(scde_plugins_unload_all(), SCDE_OK, "unload_all");
    CHECK_INT(scde_plugins_running(), 0, "all stopped");
    CHECK_INT(scde_plugins_load(badver_path), SCDE_ERR_VERSION,
              "direct badver load");
    CHECK_INT(scde_plugins_load(failinit_path), SCDE_ERR,
              "direct failinit load");
    CHECK_INT(scde_plugins_count(), 3, "no new entries");
    CHECK_INT(scde_plugins_state(ex_i), SCDE_PLUGIN_UNLOADED,
              "unload_all did not restart example");

done:
    scde_plugins_shutdown();
    CHECK_INT(scde_plugins_count(), 0, "forgotten");
    CHECK_INT(scde_event_unsubscribe(cb_plugin, NULL), SCDE_OK,
              "unsubscribe plugin events");
}

/* --------------------------------------------------------------- main -- */

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    if (text)
        fputs(text, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    char tmp[] = "/tmp/scde-api-XXXXXX";
    char cfg[512];
    char junk1[512];
    char junk2[512];
    char plugin_dir[512];
    char *slash;
    const char *example = NULL;
    const char *badver = NULL;
    const char *failinit = NULL;

    printf("SCDE App API tests\n");
    scde_log_set_sink(log_sink, NULL);

    if (!mkdtemp(tmp)) {
        printf("FAIL: mkdtemp failed\n");
        return 1;
    }
    snprintf(cfg, sizeof(cfg), "%s/config", tmp);
    snprintf(junk1, sizeof(junk1), "%s/junk1.so", tmp);
    snprintf(junk2, sizeof(junk2), "%s/junk2.so", tmp);
    if (write_file(cfg,
                   "# SCDE API test config\n"
                   "name = hello world\n"
                   "num=-42\n"
                   "yes=true\n"
                   "no=0\n"
                   "flag = on\n"
                   "empty=\n") != 0 ||
        write_file(junk1, "this is not an ELF shared object\n") != 0 ||
        write_file(junk2, "") != 0) {
        printf("FAIL: cannot write temp files\n");
        rmdir(tmp);
        return 1;
    }

    if (argc >= 4) {
        example = argv[1];
        badver = argv[2];
        failinit = argv[3];
        snprintf(plugin_dir, sizeof(plugin_dir), "%s", example);
        slash = strrchr(plugin_dir, '/');
        if (slash)
            *slash = '\0';
    } else {
        printf("note: plugin paths not given, plugin tests skipped\n");
    }

    test_version();
    test_app_lifecycle();
    test_config(cfg);
    test_events();
    test_services(cfg);
    test_window_no_display();
    test_plugins_invalid(tmp);
    if (example && badver && failinit)
        test_plugins_real(plugin_dir, example, badver, failinit);

    unlink(cfg);
    unlink(junk1);
    unlink(junk2);
    rmdir(tmp);

    scde_log_set_sink(NULL, NULL);
    printf("api tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
