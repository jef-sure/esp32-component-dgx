#pragma once
/*
 * drivers/vscreen.h
 *
 *  Deprecated until DGX 1.0, kept for programs written before the header
 *  moved: a virtual screen is not a driver, and its calls are in
 *  dgx_vscreen.h.
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "dgx_vscreen.h"

/*
 * The warning a deprecated name gives, here for the header: a warning of
 * its own kind would stop a build with -Werror, as ESP-IDF builds, where
 * the deprecated names only warn.
 */
typedef int dgx_drivers_vscreen_h_is_deprecated DGX_DEPRECATED("include dgx_vscreen.h");
typedef dgx_drivers_vscreen_h_is_deprecated dgx_drivers_vscreen_h_included;
