/*
 * SCDE Security Layer - internal helpers shared by the security sources.
 * Not installed; applications only see the public security.h header.
 */
#ifndef SCDE_SECURITY_INTERNAL_H
#define SCDE_SECURITY_INTERNAL_H

#include <scde/security.h>

/* provided by src/api/event.c (monotonic ms) */
uint32_t scde_event_now_ms(void);

/* owned by core.c */
scde_scan_stats_t *scde__stats(void);
void scde__ensure(void);                 /* lazy, idempotent init */
const char *scde__last_exec_subject(void); /* subject of the last exec gate */

/* hooks implemented in scanner.c and driven by core.c */
int scde__scanner_init(const char *db_dir);
void scde__scanner_shutdown(void);
int scde__scanner_reload(void);
int scde__scanner_set_mode(int mode);
int scde__scanner_watch_dir(const char *dir);
void scde__scanner_watch_stop(void);
int scde__scanner_watch_fd(void);
int scde__scanner_pump(void);

#endif /* SCDE_SECURITY_INTERNAL_H */
