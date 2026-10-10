/*
 * Host test for the OPTION NETWORK DRIVE helpers (#1128): UNC normalisation
 * and NTLM password hashing. Compiles netdrive_unc.c directly, so it needs no
 * SMB server or interpreter context. Prints one "ok <name>" / "FAIL <name>"
 * line per check and exits non-zero on any failure.
 */
#include "mmb_priv.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *name, int cond)
{
	printf("%s %s\n", cond ? "ok" : "FAIL", name);
	if (!cond)
		failures++;
}

static void check_norm(const char *name, const char *in, const char *want)
{
	char out[128];
	int rc = mmb_netdrive_normalize(in, out, sizeof(out));
	check(name, rc == 1 && strcmp(out, want) == 0);
	if (rc != 1 || strcmp(out, want) != 0)
		printf("  got '%s' want '%s'\n", out, want);
}

static void check_norm_bad(const char *name, const char *in)
{
	char out[128];
	check(name, mmb_netdrive_normalize(in, out, sizeof(out)) == 0);
}

int main(void)
{
	char out[128];

	/* Plain UNC and doubled backslashes both normalise. */
	check_norm("unc_plain", "\\\\host\\share", "\\\\host\\share");
	check_norm("unc_doubled", "\\\\\\\\host\\\\share", "\\\\host\\share");
	check_norm("unc_forward", "//host/share", "\\\\host\\share");
	check_norm("unc_smb_scheme", "smb://host/share", "\\\\host\\share");
	check_norm("unc_port", "smb://host:4455/share", "\\\\host:4455\\share");
	check_norm("unc_sub", "//host/share/sub/dir", "\\\\host\\share\\sub\\dir");
	check_norm("unc_mixed_seps", "\\\\host//share\\\\sub", "\\\\host\\share\\sub");
	check_norm("unc_trailing_sep", "//host/share/", "\\\\host\\share");

	/* Missing host or share is rejected. */
	check_norm_bad("unc_no_share", "host");
	check_norm_bad("unc_empty", "");
	check_norm_bad("unc_share_only", "//share");

	/* NTLM hash: MD4 over the UTF-16LE password ("secret" -> well-known). */
	mmb_netdrive_hash_password("secret", out, sizeof(out));
	check("hash_secret",
	      strcmp(out, "ntlm:878d8014606cda29677a44efa1353fc7") == 0);
	if (strcmp(out, "ntlm:878d8014606cda29677a44efa1353fc7") != 0)
		printf("  got '%s'\n", out);

	/* Empty password stays empty (guest). */
	mmb_netdrive_hash_password("", out, sizeof(out));
	check("hash_empty", out[0] == 0);

	/* An already-hashed value is kept verbatim. */
	mmb_netdrive_hash_password("ntlm:0123456789abcdef0123456789abcdef", out,
				   sizeof(out));
	check("hash_passthrough",
	      strcmp(out, "ntlm:0123456789abcdef0123456789abcdef") == 0);

	if (failures)
		printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
