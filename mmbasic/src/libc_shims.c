#include <stdio.h>
#include <stdlib.h>
#include <reent.h>

div_t div(int n, int d)
{
	div_t r;
	r.quot = d ? n / d : 0;
	r.rem = d ? n % d : 0;
	return r;
}

int fflush(FILE *f)
{
	(void)f;
	return 0;
}

void rewind(FILE *f)
{
	(void)f;
}

void exit(int c)
{
	(void)c;
	for (;;)
		;
}

struct _reent impure_data;
struct _reent *_impure_ptr = &impure_data;
