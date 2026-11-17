#include "app_category.h"

static const char *const cat_names[MMB_APP_CATEGORY_COUNT] = {
	"GAMES", "DEMOS", "CREATIVE", "AUDIO", "PRODUCTIVITY",
	"DEVELOPMENT", "UTILITIES", "INTERNET", "LEARNING", "OTHER",
};

static char cat_upper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int cat_eq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b)
	{
		if (cat_upper(*a) != cat_upper(*b))
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

int mmb_app_category_count(void)
{
	return MMB_APP_CATEGORY_COUNT;
}

const char *mmb_app_category_name(int i)
{
	if (i < 0 || i >= MMB_APP_CATEGORY_COUNT)
		return 0;
	return cat_names[i];
}

int mmb_app_category_index(const char *name)
{
	int i;
	if (name)
	{
		for (i = 0; i < MMB_APP_CATEGORY_COUNT; i++)
			if (cat_eq(name, cat_names[i]))
				return i;
	}
	return MMB_APP_CATEGORY_OTHER;
}

int mmb_app_category_valid(const char *name)
{
	int i;
	if (!name || !name[0])
		return 0;
	for (i = 0; i < MMB_APP_CATEGORY_COUNT; i++)
		if (cat_eq(name, cat_names[i]))
			return 1;
	return 0;
}
