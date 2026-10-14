/* SPDX-License-Identifier: GPL-3.0-or-later
 * Inert assets for filesystem regression tests only. Never link into PS5 releases.
 * This avoids executing/redistributing any embedded console modules in local test binaries. */
#ifndef E2A_HOST
#error Test assets are only for the host regression binary
#endif
#define ASSET(name) unsigned char name[] = {0x4f, 0x15, 0x3d, 0x1d}; unsigned int name##_len = 4
ASSET(libSceAmpr_sprx);
ASSET(libSceAmpr_debug_sprx);
ASSET(libScePlayGo_sprx);
ASSET(libSceAppContent_sprx);
ASSET(libSceGameUpdate_sprx);
ASSET(libSceNpEntitlementAccess_sprx);
unsigned char index_html[] = "host test fixture";
unsigned int index_html_len = sizeof index_html - 1;
