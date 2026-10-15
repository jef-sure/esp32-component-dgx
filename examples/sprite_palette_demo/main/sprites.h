#pragma once
/* The pictures of the demo, in sprites.c, written by make_sprites.py. */
#include <stdint.h>

/* a walker of 16 x 24, 4 bits: standing, and in a stride */
extern const uint8_t walker_stand_data[192];
extern const uint8_t walker_stride_data[192];
/* an arrow of 16 x 16 pointing right, 1 bit */
extern const uint8_t arrow_data[32];
/* a coin of 12 x 12, 8 bits: its values are the indices COIN_FIRST .. COIN_FIRST + 5 of the frame */
extern const uint8_t coin_data[144];
#define COIN_FIRST 130
