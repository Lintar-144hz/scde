/*
 * SCDE App API - notifications (fixed ring + event announcement).
 */
#include <scde/event.h>
#include <scde/notification.h>

#include "internal.h"

#include <stdio.h>
#include <string.h>

static scde_notification_t ring[SCDE_NOTIFICATION_RING];
static size_t ring_len;                 /* stored entries */
static size_t ring_head;                /* oldest entry */
static uint64_t notify_total;
static uint64_t notify_next_id = 1;

int scde_notify(const char *title, const char *body)
{
    scde_notification_t n;
    scde_event_t ev;
    size_t slot;
    int written;

    if (!title || !*title)
        return SCDE_ERR_ARG;

    memset(&n, 0, sizeof(n));
    n.id = notify_next_id++;
    n.time_ms = scde_event_now_ms();
    snprintf(n.title, sizeof(n.title), "%s", title);
    snprintf(n.body, sizeof(n.body), "%s", body ? body : "");

    if (ring_len == SCDE_NOTIFICATION_RING) {
        slot = ring_head;
        ring_head = (ring_head + 1) % SCDE_NOTIFICATION_RING;
    } else {
        slot = (ring_head + ring_len) % SCDE_NOTIFICATION_RING;
        ring_len++;
    }
    ring[slot] = n;
    notify_total++;

    scde_logf(SCDE_LOG_INFO, "notification: %s%s%s", n.title,
              n.body[0] ? " - " : "", n.body);

    memset(&ev, 0, sizeof(ev));
    ev.type = SCDE_EVENT_NOTIFICATION;
    ev.id = n.id;
    ev.time_ms = n.time_ms;
    written = snprintf(ev.text, sizeof(ev.text), "%s%s%s", n.title,
                       n.body[0] ? ": " : "", n.body);
    if (written < 0)
        ev.text[0] = '\0';
    scde_event_post(&ev);
    return SCDE_OK;
}

int scde_notify_last(scde_notification_t *out)
{
    if (!out)
        return SCDE_ERR_ARG;
    if (ring_len == 0)
        return SCDE_ERR_NOT_FOUND;
    *out = ring[(ring_head + ring_len - 1) % SCDE_NOTIFICATION_RING];
    return SCDE_OK;
}

size_t scde_notify_count(void)
{
    return ring_len;
}

uint64_t scde_notify_total(void)
{
    return notify_total;
}

void scde_notify_clear(void)
{
    memset(ring, 0, sizeof(ring));
    ring_len = 0;
    ring_head = 0;
}
