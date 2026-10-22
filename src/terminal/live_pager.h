/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_LIVE_PAGER_H
#define HAX_TERMINAL_LIVE_PAGER_H

#include <stdio.h>

struct spinner;
struct live_pager;

/* Foreground-thread only. The snapshot remains caller-owned. While open, stdout and stderr
 * spool to a private file and the pager owns stdin. Execution may continue on the caller. */
struct live_pager *live_pager_open(const char *command, FILE *snapshot, struct spinner *spinner);

/* Return true after restoring terminal ownership and replaying queued output. Does not free. */
int live_pager_poll(struct live_pager *pager);

/* Wait for the pager if necessary, restore output, and free. NULL is allowed. */
void live_pager_close(struct live_pager *pager);

#endif /* HAX_TERMINAL_LIVE_PAGER_H */
