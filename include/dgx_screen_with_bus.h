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
}

