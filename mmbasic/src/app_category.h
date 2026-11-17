#ifndef MMB_APP_CATEGORY_H
#define MMB_APP_CATEGORY_H

/* Shared app category list for APPS and the HUB (#1523). The order is fixed
 * and used everywhere in the UI, so callers must not reorder it. */
#define MMB_APP_CATEGORY_COUNT 10
#define MMB_APP_CATEGORY_OTHER 9

int mmb_app_category_count(void);
const char *mmb_app_category_name(int i);
int mmb_app_category_index(const char *name);
/* Non-zero when `name` is one of the ten ids (case-insensitive). */
int mmb_app_category_valid(const char *name);

#endif
