/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdlib.h>
#include <string.h>

#include "gimv_page_layout.h"


GimvPageLayout *
gimv_page_layout_new (const gchar *name, gint cols, gint rows,
                      gboolean rtl, gboolean first_alone)
{
   GimvPageLayout *l = g_new0 (GimvPageLayout, 1);
   gchar **parts;

   /* ';' parts the layouts in the string */
   parts = g_strsplit (name ? name : "", ";", -1);
   l->name = g_strstrip (g_strjoinv ("", parts));
   g_strfreev (parts);
   l->cols = CLAMP (cols, 1, GIMV_PAGE_LAYOUT_MAX_COLS);
   l->rows = CLAMP (rows, 1, GIMV_PAGE_LAYOUT_MAX_ROWS);
   l->rtl = rtl != FALSE;
   l->first_alone = first_alone != FALSE;
   return l;
}


GimvPageLayout *
gimv_page_layout_copy (const GimvPageLayout *l)
{
   g_return_val_if_fail (l, NULL);
   return gimv_page_layout_new (l->name, l->cols, l->rows, l->rtl, l->first_alone);
}


void
gimv_page_layout_free (GimvPageLayout *l)
{
   if (!l) return;
   g_free (l->name);
   g_free (l);
}


GPtrArray *
gimv_page_layouts_parse (const gchar *str)
{
   GPtrArray *a = g_ptr_array_new_with_free_func ((GDestroyNotify) gimv_page_layout_free);
   gchar **entries;
   guint i;

   if (!str) return a;
   entries = g_strsplit (str, ";", -1);
   for (i = 0; entries[i]; i++) {
      gchar **f = g_strsplit (entries[i], ",", 4);
      gchar *end;
      glong cols, rows;

      if (g_strv_length (f) == 4) {
         cols = strtol (f[0], &end, 10);
         if (*end) cols = 0;
         rows = strtol (f[1], &end, 10);
         if (*end) rows = 0;
         if (cols >= 1 && cols <= GIMV_PAGE_LAYOUT_MAX_COLS
             && rows >= 1 && rows <= GIMV_PAGE_LAYOUT_MAX_ROWS)
         {
            g_ptr_array_add (a, gimv_page_layout_new (f[3], cols, rows,
                                                      strchr (f[2], 'r') != NULL,
                                                      strchr (f[2], 'a') != NULL));
         }
      }
      g_strfreev (f);
   }
   g_strfreev (entries);
   return a;
}


gchar *
gimv_page_layouts_to_string (GPtrArray *layouts)
{
   GString *s = g_string_new (NULL);
   guint i;

   for (i = 0; layouts && i < layouts->len; i++) {
      GimvPageLayout *l = g_ptr_array_index (layouts, i);
      GimvPageLayout *clean = gimv_page_layout_new (l->name, l->cols, l->rows,
                                                    l->rtl, l->first_alone);
      g_string_append_printf (s, "%s%d,%d,%s%s,%s", i ? ";" : "",
                              clean->cols, clean->rows,
                              clean->rtl ? "r" : "", clean->first_alone ? "a" : "",
                              clean->name);
      gimv_page_layout_free (clean);
   }
   return g_string_free (s, FALSE);
}


void
gimv_page_layout_place (gint cols, gboolean rtl, gint n,
                        const gint *width, gint height,
                        gint *x, gint *y,
                        gint *total_width, gint *total_height)
{
   gint i, r, rows, w = 0;

   if (cols < 1) cols = 1;
   rows = n > 0 ? (n + cols - 1) / cols : 0;

   /* the widest row */
   for (r = 0; r < rows; r++) {
      gint rw = 0;
      for (i = r * cols; i < n && i < (r + 1) * cols; i++)
         rw += width[i];
      w = MAX (w, rw);
   }

   for (r = 0; r < rows; r++) {
      gint pos = rtl ? w : 0;
      for (i = r * cols; i < n && i < (r + 1) * cols; i++) {
         if (rtl) {
            pos -= width[i];
            x[i] = pos;
         } else {
            x[i] = pos;
            pos += width[i];
         }
         y[i] = r * height;
      }
   }

   if (total_width) *total_width = w;
   if (total_height) *total_height = rows * height;
}
