/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_LAPY_ELEVATION_H
#define PW_LAPY_ELEVATION_H

#include <stdint.h>

/* Send the bundled one-shot ELF to the local elfldr listener and wait for the
 * versioned result for this process. Returns 0 on success, -1 with errno. */
int pw_lapy_elevation_request(int32_t pid);

/* Retry through a caller-verified stable title source when app0 is unavailable. */
int pw_lapy_elevation_request_at(int32_t pid, const char *helper_path);

/* Local diagnostics, not wire ABI. stage points to an immutable static string.
 * helper_status is the last helper message status that passed identity checks;
 * inspect it only when helper_status_valid is set. */
struct pw_lapy_elevation_diagnostics {
    const char *stage;
    uint32_t helper_status;
    int helper_status_valid;
    int error;
};
int pw_lapy_elevation_request_at_detailed(int32_t pid, const char *helper_path,
                                        struct pw_lapy_elevation_diagnostics *diagnostics);

#endif
