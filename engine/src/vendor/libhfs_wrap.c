/* Compiles NetBSD's libhfs.c with the per-file RCS-id macro it expects. */
#define __KERNEL_RCSID(sec, string) const char hfs_rcsid_libhfs[] = string
#include "libhfs.c"
