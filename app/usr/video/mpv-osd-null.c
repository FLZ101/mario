#include <stdio.h>

#include "osd_state.h"

void osd_destroy_backend(struct osd_state *osd)
{
}

struct sub_bitmaps *osd_object_get_bitmaps(struct osd_state *osd,
                                          struct osd_object *obj, int format)
{
    return NULL;
}

void osd_set_external_remove_owner(struct osd_state *osd, void *owner)
{
}

void osd_get_text_size(struct osd_state *osd, int *out_screen_h, int *out_font_h)
{
    *out_screen_h = 24;
    *out_font_h = 1;
}

void osd_get_function_sym(char *buffer, size_t buffer_size, int osd_function)
{
    snprintf(buffer, buffer_size, OSD_CODEPOINTS_ESCAPE "%c", osd_function);
}
