/* Compiles NetBSD's libhfs unicode.c with the per-file RCS-id macro it expects. */
#define __KERNEL_RCSID(sec, string) const char hfs_rcsid_unicode[] = string
#include "unicode.c"
