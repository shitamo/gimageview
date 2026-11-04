/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001 Takuro Ashie
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
 *
 * $Id: mag.c,v 1.9 2004/09/21 08:44:27 makeinu Exp $
 */

/*
 *  These codes are mostly taken from Enfle.
 *  Enfle Copyright (C) 1998 Hiroshi Takekawa
 *
 *  See also http://www.jisyo.com/viewer/faq/mag_tech.htm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"

#include "mag.h"
#include "gimv_plugin.h"

#define LINE200  0x01
#define COLOR8   0x02
#define DIGITAL  0x04
#define COLOR256 0xf0

#ifndef BUF_SIZE
#define BUF_SIZE 4096
#endif

#define BitSet(byte, bit)  (((byte) & (bit)) == (bit))
#define ToL(buffer, off)  (buffer[(off)] | buffer[(off)+1]<<8 | buffer[(off)+2]<<16 | buffer[(off)+3]<<24)
#define ToS(buffer, off)  (buffer[(off)] | buffer[(off)+1]<<8);

static guchar *mag_decode_image (GimvImageLoader *loader,
                                 mag_info        *minfo,
                                 guchar           colormap[256][3]);


static GimvImageLoaderPlugin gimv_mag_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "MAG",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        mag_load,
   }
};

static const gchar *mag_extensions[] =
{
   "mag",
};

static GimvMimeTypeEntry mag_mime_types[] =
{
   {
      mime_type:      "image/x-mag",
      description:    "MAG Image",
      extensions:     mag_extensions,
      extensions_len: sizeof (mag_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_mag_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(mag_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("MAG Image Loader"),
   version:       "0.1.1",
   author:        N_("Takuro Ashie"),
   description:   NULL,
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


GimvImage *
mag_load (GimvImageLoader *loader, gpointer data)
{
   GimvIO *gio;
   GimvImage *image = NULL;
   mag_info minfo;
   guchar buffer[32], comment[BUF_SIZE], colormap[256][3], *dest;
   guint i;
   guint bytes_read;
   GimvIOStatus status;

   g_return_val_if_fail (loader, NULL);

   gio = gimv_image_loader_get_gio (loader);
   if (!gio) return NULL;

   status = gimv_io_read (gio, buffer, 8, &bytes_read);
   if (status != GIMV_IO_STATUS_NORMAL
       || !(!strncmp (buffer, "MAKI02  ", 8))) /* check data: 8byte */
   {
      return NULL;
   }

   status = gimv_io_read (gio, buffer, 4 + 18 + 2, &bytes_read);
   if (status != GIMV_IO_STATUS_NORMAL) {
      /* machine code:  4byte
         user name:    18byte
         unused:        2byte */
      return NULL;
   }

   i = 0;
   do {                                        /* comment: unlimited */
      status = gimv_io_read (gio, buffer, 1, &bytes_read);
      if (status != GIMV_IO_STATUS_NORMAL) return NULL;
      comment[i++] = buffer[0];
   } while (buffer[0] != '\0' && i < 4096);    /* start of header */

   gimv_io_tell (gio, &minfo.offset);
   minfo.offset--;

   status = gimv_io_read (gio, buffer, 31, &bytes_read);
   if (status != GIMV_IO_STATUS_NORMAL) {      /* header: 32byte (include end of comment) */
      return NULL;
   }

   minfo.mcode  = buffer[0];
   minfo.dcode  = buffer[1];
   minfo.screen = buffer[2];
   minfo.lx     = ToS (buffer, 3);
   minfo.uy     = ToS (buffer, 5);
   minfo.rx     = ToS (buffer, 7);
   minfo.dy     = ToS (buffer, 9);
   minfo.off_a  = ToL (buffer, 11);
   minfo.off_b  = ToL (buffer, 15);
   minfo.size_b = ToL (buffer, 19);
   minfo.off_p  = ToL (buffer, 23);
   minfo.size_p = ToL (buffer, 27);

   minfo.ncolors = (minfo.screen & COLOR256) ? 256 : 16;

   /* the values of the header are not to be trusted */
   if (minfo.rx < minfo.lx || minfo.dy < minfo.uy) return NULL;

   /* the rows of the file are whole units: 8 dots (16 colours) or 4 (256),
    * the image is the part of the header */
   {
      guint unit = (minfo.screen & COLOR256) ? 4 : 8;
      minfo.width  = minfo.rx - minfo.lx + 1;
      minfo.height = minfo.dy - minfo.uy + 1;
      minfo.lpad   = minfo.lx % unit;
      minfo.rpad   = unit - 1 - (minfo.rx % unit);
      minfo.lx    -= minfo.lpad;
      minfo.rx    += minfo.rpad;
      minfo.flagperline = (minfo.rx - minfo.lx + 1) / (unit / 2) / 2;
   }

   /* read palette */
   for (i = 0; i < minfo.ncolors; i++) {
      /* ORDER: GRB */
      status = gimv_io_read (gio, buffer, 3, &bytes_read);
      if (status != GIMV_IO_STATUS_NORMAL || bytes_read != 3) return NULL;
      colormap[i][1] = buffer[0]; /* G */
      colormap[i][0] = buffer[1]; /* R */
      colormap[i][2] = buffer[2]; /* B */
   }

   if (!gimv_image_loader_progress_update (loader))
      return NULL;

   dest = mag_decode_image (loader, &minfo, colormap);

   if (dest)
      image = gimv_image_create_from_data (dest,
                                           minfo.width, minfo.height,
                                           FALSE);

   return image;
}


/* the largest image taken (dots), and the largest part read at once */
#define MAG_MAX_DOTS  (64 * 1024 * 1024)
#define MAG_MAX_READ  (256 * 1024 * 1024)

static gboolean
mag_read_at (GimvIO *gio, gulong pos, guchar *buf, gulong len)
{
   guint bytes_read = 0;

   if (gimv_io_seek (gio, pos, SEEK_SET) != GIMV_IO_STATUS_NORMAL)
      return FALSE;
   if (!len) return TRUE;
   if (gimv_io_read (gio, buf, len, &bytes_read) != GIMV_IO_STATUS_NORMAL)
      return FALSE;
   return bytes_read == len;
}


/* The image is decoded into rows of the padded width (the width of the
 * file), then the part of the header is taken.  Every value of the file
 * is checked: a broken file gives NULL, never a read or a write out of
 * the buffers. */
static guchar *
mag_decode_image (GimvImageLoader *loader, mag_info *minfo, guchar colormap[256][3])
{
   static const gint offset[][2] = {
      { 0,  0},    {-1,  0},    {-2,  0},    {-4,  0},
      { 0, -1},    {-1, -1},    { 0, -2},    {-1, -2},
      {-2, -2},    { 0, -4},    {-1, -4},    {-2, -4},
      { 0, -8},    {-1, -8},    {-2, -8},    { 0, -16}
   };
   GimvIO *gio;
   gboolean c256 = (minfo->screen & COLOR256) != 0;
   /* a nibble of a flag: 2 bytes of pixels, 4 dots (16 colours) or 2 (256) */
   guint dots = c256 ? 2 : 4;
   gsize pwidth, height, nflags, nflag_a, nflag_b, n, i, x, y;
   guchar *flaga = NULL, *flagb = NULL, *pix = NULL, *buf = NULL, *dest = NULL;
   gsize pr = 0, nb = 0;

   gio = gimv_image_loader_get_gio (loader);
   g_return_val_if_fail (gio, NULL);
   g_return_val_if_fail (minfo, NULL);

   pwidth = minfo->rx - minfo->lx + 1;
   height = minfo->height;
   if (!minfo->flagperline || !height || pwidth * height > MAG_MAX_DOTS)
      return NULL;

   nflags  = (gsize) minfo->flagperline * height;   /* bytes of flag B decoded */
   nflag_a = (nflags + 7) / 8;
   if (minfo->off_b < minfo->off_a || minfo->off_b - minfo->off_a < nflag_a
       || minfo->size_p > MAG_MAX_READ || minfo->size_b > MAG_MAX_READ)
   {
      return NULL;
   }

   /* flag A: a bit for each byte of flag B, which holds only the ones
    * not 0 */
   flaga = g_malloc (nflag_a);
   if (!mag_read_at (gio, minfo->offset + minfo->off_a, flaga, nflag_a))
      goto ERROR;
   for (i = 0, nflag_b = 0; i < nflags; i++)
      if (flaga[i / 8] & (0x80 >> (i % 8))) nflag_b++;

   flagb = g_malloc0 (nflags);
   buf = g_malloc (MAX (nflag_b, 1));
   if (!mag_read_at (gio, minfo->offset + minfo->off_b, buf, nflag_b))
      goto ERROR;
   for (i = 0; i < nflags; i++)
      if (flaga[i / 8] & (0x80 >> (i % 8))) flagb[i] = buf[nb++];
   g_clear_pointer (&buf, g_free);
   g_clear_pointer (&flaga, g_free);

   /* each line XORed with the one above */
   for (i = minfo->flagperline; i < nflags; i++)
      flagb[i] ^= flagb[i - minfo->flagperline];

   if (!gimv_image_loader_progress_update (loader))
      goto ERROR;

   /* pixels */
   pix = g_malloc (MAX (minfo->size_p, 1));
   if (!mag_read_at (gio, minfo->offset + minfo->off_p, pix, minfo->size_p))
      goto ERROR;

   if (!gimv_image_loader_progress_update (loader))
      goto ERROR;

   buf = g_malloc0 (pwidth * height * 3);
   for (i = 0; i < nflags; i++) {
      guint nib;

      y = i / minfo->flagperline;
      for (nib = 0; nib < 2; nib++) {
         guint f = nib ? (flagb[i] & 15) : (flagb[i] >> 4);
         guchar *d;

         x = ((i % minfo->flagperline) * 2 + nib) * dots;
         d = buf + (y * pwidth + x) * 3;

         if (f) {   /* copy from a place before */
            gssize sx = (gssize) x + offset[f][0] * (gssize) dots;
            gssize sy = (gssize) y + offset[f][1];
            if (sx < 0 || sy < 0) goto ERROR;
            memcpy (d, buf + ((gsize) sy * pwidth + sx) * 3, dots * 3);
         } else {   /* 2 bytes of pixels */
            if (pr + 2 > minfo->size_p) goto ERROR;
            for (n = 0; n < 2; n++) {
               guint t = pix[pr++];
               if (c256) {
                  memcpy (d, colormap[t], 3);
                  d += 3;
               } else {
                  memcpy (d, colormap[t >> 4], 3);
                  memcpy (d + 3, colormap[t & 15], 3);
                  d += 6;
               }
            }
         }
      }
   }

   /* the part of the header */
   dest = g_malloc ((gsize) minfo->width * height * 3);
   for (y = 0; y < height; y++)
      memcpy (dest + y * minfo->width * 3,
              buf + (y * pwidth + minfo->lpad) * 3,
              (gsize) minfo->width * 3);

ERROR:
   g_free (flaga);
   g_free (flagb);
   g_free (pix);
   g_free (buf);
   return dest;
}
