/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#pragma once
/* The diagnostic: one text with the versions, the state of the conversion, the storages and sources, the
 * settings, the plugins, and the last lines of the engine's and the app's logs (memory and files). The pairing
 * code and the console's address are blanked, so the text can be posted as it is. */
#include <stddef.h>

int diag_build(char **out, size_t *len);   /* malloc'd text; 0 or -1 (out of memory) */
/* writes it as exfat2ampr-diagnostic-<date>.txt in the homebrew folder and on the first USB or extended
 * drive; paths: where it went (", " separated); 0, or -1 with err */
int diag_write(char *paths, size_t pcap, char *err, size_t ecap);
