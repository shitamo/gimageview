/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <string.h>
#include <pango/pango.h>

#include "gimv_xpm.h"

#define XPM_MAX_PIXELS (256 * 1024 * 1024)

typedef struct {
   guchar r, g, b, a;
} XpmColor;


GPtrArray *
gimv_xpm_get_strings (const gchar *p, gsize len)
{
   const gchar *end = p + len;
   GPtrArray *strings = g_ptr_array_new_with_free_func (g_free);

   while (p < end) {
      if (p + 1 < end && p[0] == '/' && p[1] == '*') {
         p += 2;
         while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
         p += 2;
      } else if (p + 1 < end && p[0] == '/' && p[1] == '/') {
         while (p < end && *p != '\n') p++;
      } else if (*p == '"') {
         GString *s = g_string_new (NULL);
         p++;
         while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) p++;
            g_string_append_c (s, *p++);
         }
         p++;
         g_ptr_array_add (strings, g_string_free (s, FALSE));
      } else {
         p++;
      }
   }

   return strings;
}


static gboolean
xpm_is_key (const gchar *word)
{
   return !strcmp (word, "c") || !strcmp (word, "m") || !strcmp (word, "g")
      || !strcmp (word, "g4") || !strcmp (word, "s");
}


/* "<key> <value> <key> <value> ..." where a value may be several words
   ("light blue"); the colour key c is preferred over g, g4 and m */
static void
xpm_parse_color (const gchar *spec, XpmColor *color)
{
   static const gchar *prefer[] = { "c", "g", "g4", "m" };
   gchar **words = g_strsplit_set (spec, " \t", -1);
   gchar *values[G_N_ELEMENTS (prefer)] = { NULL };
   gboolean ok = FALSE;
   guint i, k;

   for (i = 0; words[i];) {
      const gchar *key;
      GString *value;

      if (!*words[i] || !xpm_is_key (words[i])) { i++; continue; }
      key = words[i++];
      value = g_string_new (NULL);
      for (; words[i] && !xpm_is_key (words[i]); i++) {
         if (!*words[i]) continue;
         if (value->len) g_string_append_c (value, ' ');
         g_string_append (value, words[i]);
      }
      for (k = 0; k < G_N_ELEMENTS (prefer); k++) {
         if (!strcmp (key, prefer[k]) && !values[k]) {
            values[k] = g_string_free (value, FALSE);
            value = NULL;
            break;
         }
      }
      if (value) g_string_free (value, TRUE);
   }

   for (k = 0; k < G_N_ELEMENTS (prefer) && !ok; k++) {
      PangoColor pc;

      if (!values[k] || !*values[k]) continue;
      if (!g_ascii_strcasecmp (values[k], "none")) {
         color->r = color->g = color->b = color->a = 0;
         ok = TRUE;
      } else if (pango_color_parse (&pc, values[k])) {   /* X11 names, #hex */
         color->r = pc.red >> 8;
         color->g = pc.green >> 8;
         color->b = pc.blue >> 8;
         color->a = 255;
         ok = TRUE;
      }
   }

   for (k = 0; k < G_N_ELEMENTS (prefer); k++) g_free (values[k]);
   g_strfreev (words);

   if (!ok) {   /* unknown colour: black, like libXpm */
      color->r = color->g = color->b = 0;
      color->a = 255;
   }
}


guchar *
gimv_xpm_decode (const gchar * const *lines, guint n_lines,
                 gint *width_ret, gint *height_ret, gboolean *alpha_ret)
{
   GHashTable *table = NULL;
   XpmColor *colors = NULL, map1[256];
   gboolean alpha = FALSE;
   guint width, height, ncolors, cpp, x, y, i;
   guchar *dest = NULL, *d;

   g_return_val_if_fail (lines && n_lines > 0, NULL);

   if (sscanf (lines[0], "%u %u %u %u", &width, &height, &ncolors, &cpp) != 4)
      return NULL;
   if (width == 0 || height == 0 || (guint64) width * height > XPM_MAX_PIXELS)
      return NULL;
   if (ncolors == 0 || ncolors > 1 << 20 || cpp == 0 || cpp > 8)
      return NULL;
   if (n_lines < 1 + ncolors + height) return NULL;

   /* colour table */
   colors = g_new0 (XpmColor, ncolors);
   if (cpp == 1) {
      for (i = 0; i < 256; i++) {
         map1[i].r = map1[i].g = map1[i].b = 0;
         map1[i].a = 255;
      }
   } else {
      table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   }

   for (i = 0; i < ncolors; i++) {
      const gchar *line = lines[1 + i];

      if (!line || strlen (line) < cpp) goto ERROR;
      xpm_parse_color (line + cpp, &colors[i]);
      if (colors[i].a == 0) alpha = TRUE;

      if (cpp == 1)
         map1[(guchar) line[0]] = colors[i];
      else
         g_hash_table_insert (table, g_strndup (line, cpp), &colors[i]);
   }

   /* pixels */
   dest = g_try_malloc ((gsize) width * height * (alpha ? 4 : 3));
   if (!dest) goto ERROR;
   d = dest;

   for (y = 0; y < height; y++) {
      const gchar *line = lines[1 + ncolors + y];
      gsize line_len = line ? strlen (line) : 0;

      for (x = 0; x < width; x++) {
         XpmColor c = { 0, 0, 0, 255 };

         if ((gsize) (x + 1) * cpp <= line_len) {
            if (cpp == 1) {
               c = map1[(guchar) line[x]];
            } else {
               gchar key[9];
               XpmColor *found;
               memcpy (key, line + (gsize) x * cpp, cpp);
               key[cpp] = '\0';
               found = g_hash_table_lookup (table, key);
               if (found) c = *found;
            }
         }

         *d++ = c.r; *d++ = c.g; *d++ = c.b;
         if (alpha) *d++ = c.a;
      }
   }

   if (table) g_hash_table_destroy (table);
   g_free (colors);

   *width_ret  = width;
   *height_ret = height;
   *alpha_ret  = alpha;

   return dest;

ERROR:
   g_free (dest);
   if (table) g_hash_table_destroy (table);
   g_free (colors);
   return NULL;
}


static GdkPixbuf *
xpm_pixbuf_new (const gchar * const *lines, guint n_lines)
{
   gint width, height;
   gboolean alpha;
   guchar *pixels = gimv_xpm_decode (lines, n_lines, &width, &height, &alpha);

   if (!pixels) return NULL;

   return gdk_pixbuf_new_from_data (pixels, GDK_COLORSPACE_RGB, alpha, 8,
                                    width, height, width * (alpha ? 4 : 3),
                                    (GdkPixbufDestroyNotify) g_free, NULL);
}


GdkPixbuf *
gimv_xpm_pixbuf_new_from_data (const gchar * const *data)
{
   guint width, height, ncolors, cpp;

   g_return_val_if_fail (data && data[0], NULL);

   /* the array has no terminator: its length follows from the header */
   if (sscanf (data[0], "%u %u %u %u", &width, &height, &ncolors, &cpp) != 4)
      return NULL;

   return xpm_pixbuf_new (data, 1 + ncolors + height);
}


GdkPixbuf *
gimv_xpm_pixbuf_new_from_file (const gchar *filename)
{
   gchar *text = NULL;
   gsize len = 0;
   GPtrArray *strings;
   GdkPixbuf *pixbuf = NULL;

   g_return_val_if_fail (filename, NULL);

   if (!g_file_get_contents (filename, &text, &len, NULL)) return NULL;

   strings = gimv_xpm_get_strings (text, len);
   if (strings->len > 0)
      pixbuf = xpm_pixbuf_new ((const gchar * const *) strings->pdata, strings->len);

   g_ptr_array_free (strings, TRUE);
   g_free (text);

   return pixbuf;
}
