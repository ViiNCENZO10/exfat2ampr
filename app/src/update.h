/* SPDX-License-Identifier: GPL-3.0-or-later
 * The app's own updates. The project publishes update.json (version, package URL, size, SHA-256) with each
 * release; the app reads it at start and from Settings. Installing downloads the package over HTTPS (the
 * console's sceHttp, on a thread), hands it to the engine 1 MiB at a time (/api/update/...), and the engine
 * checks the SHA-256, keeps the running package aside (.old) and puts the new one in place: the next start
 * of the console runs it. Nothing runs without the player asking for it. */
#ifndef E2A_APP_UPDATE_H
#define E2A_APP_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_AVAILABLE, UPD_UNREACHABLE, UPD_DOWNLOADING, UPD_INSTALLED, UPD_FAILED } upd_state;

/* look for a newer version than current ("1.45"); on a thread, nothing if a check or an install runs */
void update_check(const char *current);
/* download and install the version found (state UPD_AVAILABLE) */
void update_install(void);
/* where it stands: progress 0..100 while downloading, the version found, the last error (English) */
upd_state update_state(int *progress, char *version, size_t vcap, uint64_t *size, char *err, size_t ecap);
/* true once after every change (redraw, toasts) */
bool update_changed(void);

#endif
