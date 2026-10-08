/*
 * SCDE App API - internal helpers shared by the api translation units.
 * Not installed; applications only see the public headers in include/scde.
 */
#ifndef SCDE_API_INTERNAL_H
#define SCDE_API_INTERNAL_H

#include <scde/common.h>

/* CLOCK_MONOTONIC milliseconds, wraps after ~49 days (fine for deltas). */
uint32_t scde_event_now_ms(void);

#endif /* SCDE_API_INTERNAL_H */
