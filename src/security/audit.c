/*
 * SCDE Security Layer - security audit log.
 *
 * Fixed ring of SCDE_AUDIT_RING entries plus all-time counters per event,
 * so important events survive even when FILE_SCANNED keeps cycling through
 * the ring.  Only short metadata is recorded - never file contents.
 */
#define _DEFAULT_SOURCE
#include <scde/security.h>

#include "security_internal.h"

#include <stdio.h>
#include <string.h>

static scde_audit_entry_t ring[SCDE_AUDIT_RING];
static size_t ring_len;
static size_t ring_head;
static uint32_t totals[SCDE_AUDIT_EVENT_COUNT];

static int event_ok(int event)
{
    return event >= SCDE_AUDIT_FILE_SCANNED &&
           event < SCDE_AUDIT_EVENT_COUNT;
}

int scde_audit_log(int event, const char *subject, int risk,
                   const char *detail)
{
    scde_audit_entry_t *slot;

    if (!event_ok(event))
        return SCDE_ERR_ARG;
    if (risk < 0)
        risk = 0;
    if (risk > 100)
        risk = 100;

    if (ring_len == SCDE_AUDIT_RING) {
        slot = &ring[ring_head];
        ring_head = (ring_head + 1) % SCDE_AUDIT_RING;
    } else {
        slot = &ring[(ring_head + ring_len) % SCDE_AUDIT_RING];
        ring_len++;
    }
    memset(slot, 0, sizeof(*slot));
    slot->time_ms = scde_event_now_ms();
    slot->event = (int16_t)event;
    slot->risk = (int16_t)risk;
    snprintf(slot->subject, sizeof(slot->subject), "%.*s",
             (int)(sizeof(slot->subject) - 1), subject ? subject : "-");
    snprintf(slot->detail, sizeof(slot->detail), "%.*s",
             (int)(sizeof(slot->detail) - 1), detail ? detail : "");
    totals[event]++;

    /* mirror security-critical events into the session log (metadata only) */
    switch (event) {
    case SCDE_AUDIT_THREAT_DETECTED:
        scde_logf(SCDE_LOG_WARN, "security: threat risk=%d %s (%s)", risk,
                  slot->subject, slot->detail);
        break;
    case SCDE_AUDIT_THREAT_BLOCKED:
        scde_logf(SCDE_LOG_WARN, "security: blocked %s (%s)", slot->subject,
                  slot->detail);
        break;
    case SCDE_AUDIT_PLUGIN_REJECTED:
    case SCDE_AUDIT_CAPABILITY_DENIED:
    case SCDE_AUDIT_POLICY_DENIED:
    case SCDE_AUDIT_SANDBOX_FAILED:
    case SCDE_AUDIT_PROCESS_BLOCKED:
    case SCDE_AUDIT_TRUST_REQUIRED:
    case SCDE_AUDIT_SECURITY_FEATURE_UNAVAILABLE:
    case SCDE_AUDIT_BAIT_ACCESSED:
    case SCDE_AUDIT_BAIT_MODIFIED:
    case SCDE_AUDIT_BAIT_DELETED:
    case SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT:
    case SCDE_AUDIT_BAIT_PERMISSION_DENIED:
    case SCDE_AUDIT_MIRAGE_PROBED:
    case SCDE_AUDIT_MIRAGE_ACCESSED:
    case SCDE_AUDIT_MIRAGE_MODIFIED:
    case SCDE_AUDIT_MIRAGE_ESCAPE_ATTEMPT:
        scde_logf(SCDE_LOG_WARN, "security: %s %s (%s)",
                  scde_audit_event_name(event), slot->subject, slot->detail);
        break;
    case SCDE_AUDIT_PLUGIN_TRUSTED:
    case SCDE_AUDIT_SCANNER_MODE_CHANGED:
    case SCDE_AUDIT_SANDBOX_CREATED:
    case SCDE_AUDIT_CAPABILITY_GRANTED:
    case SCDE_AUDIT_PROCESS_RESTRICTED:
    case SCDE_AUDIT_POLICY_FALLBACK:
    case SCDE_AUDIT_BAIT_CREATED:
    case SCDE_AUDIT_MIRAGE_CREATED:
        scde_logf(SCDE_LOG_INFO, "security: %s %s (%s)",
                  scde_audit_event_name(event), slot->subject, slot->detail);
        break;
    default:
        break;                      /* FILE_SCANNED stays out of the log */
    }
    return SCDE_OK;
}

int scde_audit_count(void)
{
    return (int)ring_len;
}

int scde_audit_get(int index, scde_audit_entry_t *out)
{
    if (!out || index < 0 || (size_t)index >= ring_len)
        return SCDE_ERR_NOT_FOUND;
    *out = ring[(ring_head + (size_t)index) % SCDE_AUDIT_RING];
    return SCDE_OK;
}

uint32_t scde_audit_total(int event)
{
    if (!event_ok(event))
        return 0;
    return totals[event];
}

int scde_audit_clear(void)
{
    memset(ring, 0, sizeof(ring));
    ring_len = 0;
    ring_head = 0;
    return SCDE_OK;
}

const char *scde_audit_event_name(int event)
{
    switch (event) {
    case SCDE_AUDIT_FILE_SCANNED:
        return "FILE_SCANNED";
    case SCDE_AUDIT_THREAT_DETECTED:
        return "THREAT_DETECTED";
    case SCDE_AUDIT_THREAT_BLOCKED:
        return "THREAT_BLOCKED";
    case SCDE_AUDIT_PLUGIN_REJECTED:
        return "PLUGIN_REJECTED";
    case SCDE_AUDIT_PLUGIN_TRUSTED:
        return "PLUGIN_TRUSTED";
    case SCDE_AUDIT_CAPABILITY_DENIED:
        return "CAPABILITY_DENIED";
    case SCDE_AUDIT_POLICY_DENIED:
        return "POLICY_DENIED";
    case SCDE_AUDIT_SCANNER_MODE_CHANGED:
        return "SCANNER_MODE_CHANGED";
    case SCDE_AUDIT_SANDBOX_CREATED:
        return "SANDBOX_CREATED";
    case SCDE_AUDIT_SANDBOX_FAILED:
        return "SANDBOX_FAILED";
    case SCDE_AUDIT_CAPABILITY_GRANTED:
        return "CAPABILITY_GRANTED";
    case SCDE_AUDIT_PROCESS_RESTRICTED:
        return "PROCESS_RESTRICTED";
    case SCDE_AUDIT_PROCESS_BLOCKED:
        return "PROCESS_BLOCKED";
    case SCDE_AUDIT_TRUST_REQUIRED:
        return "TRUST_REQUIRED";
    case SCDE_AUDIT_POLICY_FALLBACK:
        return "POLICY_FALLBACK";
    case SCDE_AUDIT_SECURITY_FEATURE_UNAVAILABLE:
        return "SECURITY_FEATURE_UNAVAILABLE";
    case SCDE_AUDIT_BAIT_CREATED:
        return "BAIT_CREATED";
    case SCDE_AUDIT_BAIT_ACCESSED:
        return "BAIT_ACCESSED";
    case SCDE_AUDIT_BAIT_MODIFIED:
        return "BAIT_MODIFIED";
    case SCDE_AUDIT_BAIT_DELETED:
        return "BAIT_DELETED";
    case SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT:
        return "BAIT_EXECUTION_ATTEMPT";
    case SCDE_AUDIT_BAIT_PERMISSION_DENIED:
        return "BAIT_PERMISSION_DENIED";
    case SCDE_AUDIT_MIRAGE_CREATED:
        return "MIRAGE_CREATED";
    case SCDE_AUDIT_MIRAGE_PROBED:
        return "MIRAGE_PROBED";
    case SCDE_AUDIT_MIRAGE_ACCESSED:
        return "MIRAGE_ACCESSED";
    case SCDE_AUDIT_MIRAGE_MODIFIED:
        return "MIRAGE_MODIFIED";
    case SCDE_AUDIT_MIRAGE_ESCAPE_ATTEMPT:
        return "MIRAGE_ESCAPE_ATTEMPT";
    default:
        return "UNKNOWN";
    }
}
