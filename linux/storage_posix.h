#ifndef MMB_STORAGE_POSIX_H
#define MMB_STORAGE_POSIX_H

/* Point a physical drive (C:-H:) at a host directory instead of the default
 * <MMB_DRIVE_ROOT>/<letter>. Used for the startup --drive option.
 * Returns 0 on success, -1 for a bad letter or unusable path. */
int storage_posix_mount(int letter, const char *path);

#endif
