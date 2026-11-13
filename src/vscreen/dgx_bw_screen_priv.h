#pragma once
/*
 * dgx_bw_screen_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  The page screen of dgx_bw_init() for a driver that sends the pages
 *  straight from it: its pixels in memory of the bus.
 */

#include "bus/dgx_bus_protocols.h"
#include "dgx_bw_screen.h"

/*
 * As dgx_bw_init(), the pixels from `alloc`, the memory_allocator of the
 * bus the driver sends them on, which gives memory the bus sends from as it
 * lies; NULL for the memory dgx_bw_init() takes. The destructor of the
 * screen frees them with free(), as a bus frees what it gives.
 */
dgx_screen_t *dgx_bw_init_with(int width, int height, dgx_bus_malloc_func alloc);
