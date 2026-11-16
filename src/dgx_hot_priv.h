#pragma once
/*
 * DGX_HOT marks the code a display behind a bus runs for every window and
 * every short transfer, and the shapes that are made of such transfers. The
 * linker fragment of the component gathers it into one piece of flash.
 *
 * Why: the flash cache of an ESP32 has two ways, and lines of code a multiple
 * of 16 KB apart share a set. Left where the linker puts them, the functions
 * of one pixel lay in three places 16 KB and 64 KB apart, and a build where
 * three of their lines met in a set drew a pixel four times slower. One
 * piece shorter than 16 KB has one line a set at most.
 *
 * The section is named only in a build that has the fragment (CMakeLists.txt
 * defines DGX_HOT_SECTION); elsewhere the mark is empty.
 */
#ifdef DGX_HOT_SECTION
#define DGX_HOT_STR_(x) #x
#define DGX_HOT_STR(x)  DGX_HOT_STR_(x)
#define DGX_HOT         __attribute__((section(".dgx_hot." DGX_HOT_STR(__COUNTER__))))
#else
#define DGX_HOT
#endif
