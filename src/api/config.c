/*
 * SCDE App API - read-only key=value configuration access.
 */
#include <scde/config.h>
#include <scde/event.h>

#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[SCDE_CONFIG_KEY_LEN];
    char value[SCDE_CONFIG_VALUE_LEN];
} entry_t;

static entry_t table[SCDE_CONFIG_MAX_KEYS];
static size_t table_len;
static char cfg_path[512];

static void post_config_changed(void)
{
    scde_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = SCDE_EVENT_CONFIG_CHANGED;
    ev.id = table_len;
    snprintf(ev.text, sizeof(ev.text), "%.120s", cfg_path);
    scde_event_post(&ev);
}

static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static void clear_table(void)
{
    memset(table, 0, sizeof(table));
    table_len = 0;
}

static const char *default_path(char *buf, size_t buf_len)
{
    const char *env = getenv("SCDE_CONFIG");
    const char *home;

    if (env && *env)
        return env;
    home = getenv("HOME");
    if (!home || !*home)
        return NULL;
    snprintf(buf, buf_len, "%s/.config/scde/config", home);
    return buf;
}

int scde_config_load(const char *path)
{
    char buf[512];
    char use_buf[512];
    char line[512];
    const char *use = path;
    FILE *f;

    if (!use) {
        use = default_path(buf, sizeof(buf));
        if (!use)
            return SCDE_ERR_NOT_FOUND;
    }
    /* snapshot: `use` may alias cfg_path (scde_config_reload passes it) */
    snprintf(use_buf, sizeof(use_buf), "%.*s", (int)(sizeof(use_buf) - 1),
             use);
    use = use_buf;

    clear_table();
    snprintf(cfg_path, sizeof(cfg_path), "%.*s", (int)(sizeof(cfg_path) - 1),
             use);

    f = fopen(use, "r");
    if (!f) {
        cfg_path[0] = '\0';
        return SCDE_ERR_NOT_FOUND;
    }

    while (fgets(line, sizeof(line), f) && table_len < SCDE_CONFIG_MAX_KEYS) {
        char *s = trim(line);
        char *eq;
        char *key;
        char *value;

        if (*s == '\0' || *s == '#')
            continue;
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = trim(s);
        value = trim(eq + 1);
        if (*key == '\0')
            continue;
        if (strlen(key) >= SCDE_CONFIG_KEY_LEN)
            continue;
        snprintf(table[table_len].key, sizeof(table[table_len].key), "%s",
                 key);
        snprintf(table[table_len].value, sizeof(table[table_len].value), "%s",
                 value);
        table_len++;
    }
    fclose(f);

    scde_logf(SCDE_LOG_DEBUG, "config: loaded %zu keys from %s", table_len,
              use);
    post_config_changed();
    return SCDE_OK;
}

int scde_config_reload(void)
{
    if (cfg_path[0] == '\0')
        return SCDE_ERR_STATE;
    return scde_config_load(cfg_path);
}

const char *scde_config_path(void)
{
    return cfg_path;
}

void scde_config_clear(void)
{
    clear_table();
    cfg_path[0] = '\0';
}

static const char *find_value(const char *key)
{
    size_t i;

    if (!key || !*key)
        return NULL;
    for (i = 0; i < table_len; i++) {
        if (strcmp(table[i].key, key) == 0)
            return table[i].value;
    }
    return NULL;
}

const char *scde_config_get(const char *key, const char *default_value)
{
    const char *v = find_value(key);

    return v ? v : default_value;
}

int scde_config_get_int(const char *key, int default_value)
{
    const char *v = find_value(key);
    char *end;
    long n;

    if (!v || !*v)
        return default_value;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0')
        return default_value;
    return (int)n;
}

int scde_config_get_bool(const char *key, int default_value)
{
    const char *v = find_value(key);

    if (!v)
        return default_value;
    if (!*v)
        return 0;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
        strcmp(v, "yes") == 0 || strcmp(v, "on") == 0)
        return 1;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
        strcmp(v, "no") == 0 || strcmp(v, "off") == 0)
        return 0;
    return default_value;
}

int scde_config_has(const char *key)
{
    return find_value(key) != NULL;
}

size_t scde_config_count(void)
{
    return table_len;
}

const char *scde_config_key(size_t index)
{
    if (index >= table_len)
        return NULL;
    return table[index].key;
}

const char *scde_config_value(size_t index)
{
    if (index >= table_len)
        return NULL;
    return table[index].value;
}
