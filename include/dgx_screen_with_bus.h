#pragma once
/*
 * dgx_screen_with_bus.h
 *
 *  Created on: 02.01.2023
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"

typedef enum {
    DGX_SCREEN_AREA_PROTO_STD16 = 0,
    DGX_SCREEN_AREA_PROTO_CMD8  = 1,
    DGX_SCREEN_AREA_PROTO_STD8  = 2,
} dgx_screen_area_protocol_t;

typedef struct _dgx_screen_with_bus_t {
    dgx_screen_t scr;
    dgx_bus_protocols_t *bus;
    uint8_t xcmd_set;
    uint8_t ycmd_set;
    uint8_t rcmd_send;
    uint8_t wcmd_send;
    uint8_t area_protocol;
    struct {
        uint16_t left;
        uint16_t right;
        uint16_t top;
        uint16_t bottom;
    } cached_area;
    /*
     * For a bus that takes pixels only together with a command, DGX_BUS_P8:
     * the window that was asked for and the next pixel of it. The display is
     * told of the window when pixels come, see dgx_screen_with_bus_write_window().
     */
    struct {
        uint16_t left;
        uint16_t right;
        uint16_t top;
        uint16_t bottom;
        uint16_t x;
        uint16_t y;
        bool     full;    /* both bounds are sent every time, changed or not */
        bool     started; /* pixels have come into it */
        bool     cut;     /* the scroll of the panel cuts it: it goes in pieces, each to where its rows lie in memory */
    } window;
    bool area_void; /* the window that was asked for lies off the screen: the pixels sent into it are dropped */
    /*
     * The scroll of the panel itself, of a controller that has one: an area
     * of its rows is shown from another row of its memory on, and the rows
     * that leave at one end come in at the other. The picture moves at once
     * and no pixel is sent. A row of the area then lies in memory elsewhere
     * than it is shown, by `shift`, and every window is led there: see
     * dgx_screen_with_bus_scroll().
     */
    struct {
        uint16_t lines;     /* rows of memory the controller scrolls along; 0: it has no scroll, or its driver does not use it */
        uint16_t first;     /* the area scrolled: its first address along that way, as a window has it, */
        uint16_t count;     /* and how many addresses it takes; 0: none */
        uint16_t shift;     /* an address of the area and this, turned round within the area, is where its row lies in memory */
        uint8_t  area_cmd;  /* the command that sets the area */
        uint8_t  start_cmd; /* the command that sets the row of memory the area is shown from */
        bool     columns;   /* that way is the columns of a window, not its rows: the axes are swapped */
        bool     mirror;    /* addresses along that way run against the rows of memory */
    } scroll;
} dgx_screen_with_bus_t;

static inline void dgx_screen_with_bus_init_area(dgx_screen_with_bus_t *scr, uint8_t xcmd, uint8_t ycmd, uint8_t wcmd, uint8_t rcmd,
                                                 dgx_screen_area_protocol_t protocol)
{
    scr->xcmd_set          = xcmd;
    scr->ycmd_set          = ycmd;
    scr->wcmd_send         = wcmd;
    scr->rcmd_send         = rcmd;
    scr->area_protocol     = (uint8_t)protocol;
    scr->cached_area.left  = 0xffff;
    scr->cached_area.right = 0xffff;
    scr->cached_area.top   = 0xffff;
    scr->cached_area.bottom = 0xffff;
    scr->window.left = scr->window.right = scr->window.top = scr->window.bottom = 0;
    scr->window.x = scr->window.y = 0;
    scr->window.full = scr->window.started = scr->window.cut = false;
    scr->area_void = false;
    scr->scroll.lines = scr->scroll.first = scr->scroll.count = scr->scroll.shift = 0;
    scr->scroll.area_cmd = scr->scroll.start_cmd = 0;
    scr->scroll.columns = scr->scroll.mirror = false;
}

