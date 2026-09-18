#include "mmb_priv.h"

/*
 * Seed the A: ramdisk from the tree embedded by scripts/gen_ramdisk.py.
 * The generated table is build output (git-ignored); this file is the
 * hand-written half that walks it.
 */

extern const mmb_ramdisk_entry mmb_ramdisk_table[];
extern const int mmb_ramdisk_count;

void mmb_ramdisk_seed(void)
{
	int i;
	for (i = 0; i < mmb_ramdisk_count; i++)
		mmb_vfs_seed_file(mmb_ramdisk_table[i].path,
				  mmb_ramdisk_table[i].data,
				  mmb_ramdisk_table[i].len);
}
