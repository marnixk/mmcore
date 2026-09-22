#ifndef MMB_STORAGE_POSIX_H
#define MMB_STORAGE_POSIX_H

/* Point a physical drive (C:-H:) at a host directory instead of the default
 * <MMB_DRIVE_ROOT>/<letter>. Used by the startup --drive option (D:) and by
 * app mode. Only C: is created by default; a mounted drive is not created
 * under the root. Returns 0 on success, -1 for a bad letter or unusable path. */
int storage_posix_mount(int letter, const char *path);

#endif
