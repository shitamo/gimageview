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

/*
 * PBM / PGM / PPM loader (P1 - P6, maxval up to 65535).  gdk-pixbuf builds
 * its PNM loader only as a "weakly maintained" optional loader.
 */

#include "config.h"

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

static GimvImage *pnm_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_pnm_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "PNM",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        pnm_load,
   }
};

static const gchar *pnm_extensions[] = { "pnm" };
static const gchar *pbm_extensions[] = { "pbm" };
static const gchar *pgm_extensions[] = { "pgm" };
static const gchar *ppm_extensions[] = { "ppm" };

static GimvMimeTypeEntry pnm_mime_types[] =
{
   {
      mime_type:      "image/x-portable-anymap",
      description:    N_("Portable Any Map Image"),
      extensions:     pnm_extensions,
      extensions_len: G_N_ELEMENTS (pnm_extensions),
      icon:           NULL,
   },
   {
      mime_type:      "image/x-portable-bitmap",
      description:    N_("The PBM image format"),
      extensions:     pbm_extensions,
      extensions_len: G_N_ELEMENTS (pbm_extensions),
      icon:           NULL,
   },
   {
      mime_type:      "image/x-portable-graymap",
      description:    N_("The PGM image format"),
      extensions:     pgm_extensions,
      extensions_len: G_N_ELEMENTS (pgm_extensions),
      icon:           NULL,
   },
   {
      mime_type:      "image/x-portable-pixmap",
      description:    N_("The PPM image format"),
      extensions:     ppm_extensions,
      extensions_len: G_N_ELEMENTS (ppm_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_pnm_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(pnm_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("PNM Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("PBM, PGM and PPM images (ASCII and binary)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


typedef struct {
   const guchar *p, *end;
} PnmReader;


static void
pnm_skip_space (PnmReader *r)
{
   while (r->p < r->end) {
      if (*r->p == '#') {
         while (r->p < r->end && *r->p != '\n' && *r->p != '\r') r->p++;
      } else if (g_ascii_isspace (*r->p)) {
         r->p++;
      } else {
         break;
      }
   }
}


static gboolean
pnm_read_uint (PnmReader *r, guint *value)
{
   guint64 v = 0;

   pnm_skip_space (r);
   if (r->p >= r->end || !g_ascii_isdigit (*r->p)) return FALSE;
   while (r->p < r->end && g_ascii_isdigit (*r->p)) {
      v = v * 10 + (*r->p - '0');
      if (v > G_MAXUINT32) return FALSE;
      r->p++;
   }
   *value = (guint) v;

   return TRUE;
}


/* one sample of a raw (binary) PGM/PPM, scaled to 0..255 */
static inline guchar
pnm_raw_sample (const guchar **p, guint maxval)
{
   guint v;

   if (maxval < 256) {
      v = *(*p)++;
   } else {
      v = ((*p)[0] << 8) | (*p)[1];
      *p += 2;
   }
   if (v > maxval) v = maxval;

   return maxval == 255 ? v : (v * 255 + maxval / 2) / maxval;
}


static GimvImage *
pnm_load (GimvImageLoader *loader, gpointer data)
{
   guchar magic[3];
   GByteArray *file;
   PnmReader r;
   guint type, width = 0, height = 0, maxval = 1, x, y;
   guchar *dest = NULL, *d;
   gsize row_bytes, sample_bytes;

   g_return_val_if_fail (loader, NULL);

   /* "P1" ... "P6" followed by white space or a comment (P7 is xvpics / PAM) */
   if (gimv_loader_peek (loader, magic, 3) < 3) return NULL;
   if (magic[0] != 'P' || magic[1] < '1' || magic[1] > '6') return NULL;
   if (!g_ascii_isspace (magic[2]) && magic[2] != '#') return NULL;
   type = magic[1] - '0';

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;

   r.p   = file->data + 2;
   r.end = file->data + file->len;

   if (!pnm_read_uint (&r, &width) || !pnm_read_uint (&r, &height)) goto ERROR;
   if (type != 1 && type != 4) {
      if (!pnm_read_uint (&r, &maxval)) goto ERROR;
      if (maxval == 0 || maxval > 65535) goto ERROR;
   }
   if (!gimv_loader_size_ok (width, height)) goto ERROR;

   dest = g_try_malloc ((gsize) width * height * 3);
   if (!dest) goto ERROR;
   d = dest;

   if (type >= 4) {
      /* binary: exactly one white space character after the header */
      if (r.p >= r.end || !g_ascii_isspace (*r.p)) goto ERROR;
      r.p++;
   }

   switch (type) {
   case 1:   /* ASCII bitmap: 1 is black */
      for (y = 0; y < height; y++) {
         for (x = 0; x < width; x++) {
            guchar v;
            while (r.p < r.end && (g_ascii_isspace (*r.p) || *r.p == '#'))
               pnm_skip_space (&r);
            if (r.p >= r.end || (*r.p != '0' && *r.p != '1')) goto ERROR;
            v = *r.p++ == '1' ? 0 : 255;
            *d++ = v; *d++ = v; *d++ = v;
         }
      }
      break;

   case 2:   /* ASCII graymap */
   case 3:   /* ASCII pixmap */
      for (y = 0; y < height; y++) {
         for (x = 0; x < width; x++) {
            guint c, v[3];
            for (c = 0; c < (type == 2 ? 1u : 3u); c++) {
               if (!pnm_read_uint (&r, &v[c])) goto ERROR;
               if (v[c] > maxval) v[c] = maxval;
               v[c] = (v[c] * 255 + maxval / 2) / maxval;
            }
            if (type == 2) v[1] = v[2] = v[0];
            *d++ = v[0]; *d++ = v[1]; *d++ = v[2];
         }
      }
      break;

   case 4:   /* raw bitmap: rows padded to bytes, 1 is black */
      row_bytes = (width + 7) / 8;
      if ((gsize) (r.end - r.p) < row_bytes * height) goto ERROR;
      for (y = 0; y < height; y++) {
         const guchar *row = r.p + row_bytes * y;
         for (x = 0; x < width; x++) {
            guchar v = (row[x / 8] & (0x80 >> (x % 8))) ? 0 : 255;
            *d++ = v; *d++ = v; *d++ = v;
         }
      }
      break;

   case 5:   /* raw graymap */
   case 6:   /* raw pixmap */
      sample_bytes = maxval < 256 ? 1 : 2;
      if ((gsize) (r.end - r.p)
          < (gsize) width * height * sample_bytes * (type == 5 ? 1 : 3))
      {
         goto ERROR;
      }
      for (y = 0; y < height; y++) {
         for (x = 0; x < width; x++) {
            if (type == 5) {
               guchar v = pnm_raw_sample (&r.p, maxval);
               *d++ = v; *d++ = v; *d++ = v;
            } else {
               *d++ = pnm_raw_sample (&r.p, maxval);
               *d++ = pnm_raw_sample (&r.p, maxval);
               *d++ = pnm_raw_sample (&r.p, maxval);
            }
         }
      }
      break;
   }

   g_byte_array_free (file, TRUE);

   return gimv_image_create_from_data (dest, width, height, FALSE);

ERROR:
   g_free (dest);
   g_byte_array_free (file, TRUE);
   return NULL;
}
