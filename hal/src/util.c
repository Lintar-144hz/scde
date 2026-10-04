#include "hal_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hal_status_t hal_read_text_file(const char *path, char *buf, size_t cap)
{
    FILE *f;
    size_t n;

    if (path == NULL || buf == NULL || cap == 0) {
        return HAL_ERR_INVALID;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return HAL_ERR_UNAVAILABLE;
        }
        if (errno == EACCES || errno == EPERM) {
            return HAL_ERR_PERMISSION;
        }
        return HAL_ERR_IO;
    }
    n = fread(buf, 1, cap - 1, f);
    if (ferror(f)) {
        fclose(f);
        return HAL_ERR_IO;
    }
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }
    return HAL_OK;
}

hal_status_t hal_parse_u64_hex(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out == NULL) {
        return HAL_ERR_INVALID;
    }
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '\0') {
        return HAL_ERR_INVALID;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text) {
        return HAL_ERR_INVALID;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return HAL_ERR_INVALID;
    }
    *out = (uint64_t)value;
    return HAL_OK;
}

bool hal_name_has_prefix(const char *name, const char *prefix,
                         unsigned long *number_out)
{
    size_t plen;
    size_t i;

    if (name == NULL || prefix == NULL) {
        return false;
    }
    plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) {
        return false;
    }
    if (name[plen] == '\0') {
        return false;
    }
    for (i = plen; name[i] != '\0'; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return false;
        }
    }
    if (number_out != NULL) {
        *number_out = strtoul(name + plen, NULL, 10);
    }
    return true;
}
