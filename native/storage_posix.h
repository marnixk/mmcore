#ifndef MMB_STORAGE_POSIX_H
#define MMB_STORAGE_POSIX_H

/* Point a physical drive (C:-H:) at a host directory instead of the default
 * <MMB_DRIVE_ROOT>/<letter>. Used by the startup --drive option (D:) and by
 * app mode. Only C: is created by default; a mounted drive is not created
 * under the root. Returns 0 on success, -1 for a bad letter or unusable path. */
int storage_posix_mount(int letter, const char *path);

/* Mount operations used to recover C: after a suspend/resume. The Linux build
 * fills these with the real mount commands; the host test overrides them with
 * stubs so the recovery/re-point path can run without a live ISO. */
typedef struct storage_mount_ops {
	int (*is_mountpoint)(const char *path);
	int (*mount_label)(const char *path, const char *label);
	int (*remount_rw)(const char *path);
	int (*overlay_writable)(void);
	int (*root_writable)(const char *root);
	const char *recovery_path; /* extra mountpoint, or NULL */
	int (*unmount_lazy)(const char *path); /* optional; may be NULL */
} storage_mount_ops;

#ifdef MMB_STORAGE_TEST
void mmb_storage_test_set_live(int live);
void mmb_storage_test_set_overlay(int overlay);
void mmb_storage_test_set_ops(const storage_mount_ops *ops);
/* Replace /proc/cmdline for boot-mode and drive/label parsing. */
void mmb_storage_test_set_cmdline(const char *cmdline);
/* Bound recovery retries; delay 0 makes a test run without sleeping. */
void mmb_storage_test_set_recovery(int attempts, int delay_ms);
/* Feed a simulated stale-mount errno through the real I/O retry path. */
int mmb_storage_test_io_error(int letter, int err);
void mmb_storage_test_reset(void);
#endif

#endif
