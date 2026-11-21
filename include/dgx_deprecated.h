/*
 * dgx_deprecated.h
 *
 *  A name that goes in DGX 1.0. It still builds and does what it did, and
 *  the compiler warns at every place a program uses it, naming what to use
 *  instead. The whole list is "Deprecated names" in docs/api-en.md.
 */

#ifndef DGX_DEPRECATED_H_
#define DGX_DEPRECATED_H_

#if defined(__GNUC__) || defined(__clang__)
#define DGX_DEPRECATED(instead) __attribute__((deprecated(instead)))
/* Around the places of DGX itself where one deprecated name is told by another: no warning there. */
#define DGX_DEPRECATED_USE_BEGIN _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define DGX_DEPRECATED_USE_END   _Pragma("GCC diagnostic pop")
#else
#define DGX_DEPRECATED(instead)
#define DGX_DEPRECATED_USE_BEGIN
#define DGX_DEPRECATED_USE_END
#endif

#endif /* DGX_DEPRECATED_H_ */
