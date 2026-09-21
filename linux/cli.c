#include "cli.h"

#include "storage_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --drive DIR binds the host directory DIR to the D: drive. */
static int parse_drive(const char *dir)
{
	if (!dir || !dir[0])
		return -1;
	return storage_posix_mount('D', dir);
}

const char *mmb_cli_parse(int argc, char **argv)
{
	const char *line = 0;
	int i;

	for (i = 1; i < argc; i++)
	{
		const char *a = argv[i];

		if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
		{
			printf("Usage: %s [options] [\"line to run\"]\n\n", argv[0]);
			printf("  --drive DIR       bind the host directory DIR to the D: drive\n");
			printf("  --drive-root DIR  base directory for the other drives (default ~/.mmbasic)\n");
			printf("\nExamples:\n");
			printf("  %s --drive /media/usb\n", argv[0]);
			printf("  %s --drive /media/usb \"DIR \\\"D:/\\\"\"\n", argv[0]);
			exit(0);
		}
		if (strcmp(a, "--drive") == 0 && i + 1 < argc)
		{
			if (parse_drive(argv[++i]) != 0)
				fprintf(stderr, "mmbasic: bad --drive spec '%s'\n",
					argv[i]);
		}
		else if (strncmp(a, "--drive=", 8) == 0)
		{
			if (parse_drive(a + 8) != 0)
				fprintf(stderr, "mmbasic: bad --drive spec '%s'\n",
					a + 8);
		}
		else if (strcmp(a, "--drive-root") == 0 && i + 1 < argc)
		{
			setenv("MMB_DRIVE_ROOT", argv[++i], 1);
		}
		else if (strncmp(a, "--drive-root=", 13) == 0)
		{
			setenv("MMB_DRIVE_ROOT", a + 13, 1);
		}
		else if (a[0] != '-')
		{
			line = a;
		}
	}
	return line;
}
