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
 * PIC loader: the image format of Yanagisawa's PIC tools for the Sharp
 * X68000 (65536 colours), also saved on the FM TOWNS and the PC-88VA.
 * Written after the description of the format, and checked against
 * RECOIL (Piotr Fusik, MIT licence) for the machines other than the
 * X68000.
 *
 *   "PIC", a comment ending with 0x1a, a second one ending with 0x00
 *   u16 the machine (big endian): 0 X68000; 0x1f X68000, 2 FM TOWNS
 *       (6 more bytes follow, skipped); 0xc2 FM TOWNS; 0x11 PC-88VA
 *       (GGGGGGRR RRRBBBBB); 0x21 PC-88VA, two pixels of GGGRRRBB a
 *       16-bit value, the lines twice as many as told
 *   u16 bits a pixel: 4, 8 (a palette follows), 15 or 16
 *   u16 width, height
 *   4, 8 bits: the palette, 16 or 256 * u16 colours of the X68000
 *   the picture: a stream of bits, the highest first
 *
 * The colours of the X68000 are GGGGGRRR RRBBBBBI (with 15 bits, without
 * I).  The picture goes left to right, top to bottom, as runs:
 *
 *   length: k ones, a zero, k + 1 bits v: v + 2^(k+1) - 1
 *   the run: length - 1 pixels of the colour before, then a new colour
 *   colour (15, 16 bits): 0 and the colour, kept in the slot of the 128
 *       least recently used; or 1 and the number of a slot (7 bits),
 *       which becomes the most recently used
 *   colour (4, 8 bits): the number in the palette
 *   1 bit: a chain follows.  Each step goes a line down: 01 a pixel to
 *       the left, 10 straight down, 11 a pixel to the right, 001 0 two
 *       pixels to the left, 001 1 two to the right, 000 the end.  The
 *       pixels reached have the new colour: the colour of a run changes
 *       there, without a run of its own (the outlines of shapes).
 */

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

static GimvImage *pic_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_pic_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "PIC",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        pic_load,
   }
};

static const gchar *pic_extensions[] = { "pic" };

static GimvMimeTypeEntry pic_mime_types[] =
{
   {
      mime_type:      "image/x-x68k-pic",
      description:    N_("The PIC image format (X68000)"),
      extensions:     pic_extensions,
      extensions_len: G_N_ELEMENTS (pic_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_pic_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(pic_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("PIC Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("PIC images (X68000, FM TOWNS, PC-88VA)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


#define PIC_X68K      0x00
#define PIC_TOWNS     0x02
#define PIC_VA        0x11
#define PIC_X68K_EXT  0x1f
#define PIC_VA_HALF   0x21
#define PIC_TOWNS_2   0xc2

#define NO_COLOR      0xffffffffu


typedef struct {
   const guchar *p, *end;
   guint         n;          /* the bit of *p next (0: the highest) */
   gboolean      over;       /* read past the end */
} Bits;


static inline guint
bit (Bits *b)
{
   guint v;

   if (b->p >= b->end) {
      b->over = TRUE;
      return 0;
   }
   v = (*b->p >> (7 - b->n)) & 1;
   if (++b->n == 8) {
      b->n = 0;
      b->p++;
   }
   return v;
}


static inline guint
bits (Bits *b, guint n)
{
   guint v = 0;
   while (n--) v = (v << 1) | bit (b);
   return v;
}


/* 0: none (broken) */
static inline guint
read_length (Bits *b)
{
   guint k;

   for (k = 1; k < 21; k++) {
      if (!bit (b)) return bits (b, k) + (1u << k) - 1;
      if (b->over) return 0;
   }
   return 0;
}


/* the 128 colours last used: a ring, the most recent at head; a new
   colour goes in the slot of the least recent */
typedef struct {
   guint32 value[128];
   guint8  prev[128], next[128];
   guint8  head;
} Recent;


static void
recent_init (Recent *r)
{
   guint i;

   for (i = 0; i < 128; i++) {
      r->value[i] = 0;
      r->prev[i] = (i + 1) & 127;
      r->next[i] = (i - 1) & 127;
   }
   r->head = 0;
}


static void
recent_add (Recent *r, guint32 value)
{
   r->head = r->prev[r->head];
   r->value[r->head] = value;
}


static guint32
recent_get (Recent *r, guint key)
{
   if (key != r->head) {
      guint8 prev = r->prev[key], next = r->next[key], tail;

      r->next[prev] = next;
      r->prev[next] = prev;
      tail = r->prev[r->head];
      r->next[tail] = key;
      r->prev[key] = tail;
      r->prev[r->head] = key;
      r->next[key] = r->head;
      r->head = key;
   }
   return r->value[key];
}


/* GGGGGRRR RRBBBBBI -> 0xRRGGBB (I: the lowest bit of each) */
static guint32
x68k_rgb (guint c)
{
   guint32 rgb = (c & 0x07c0) << 13 | (c & 0xf800) | (c & 0x3e) << 2;

   if (c & 1) rgb |= 0x040404;
   return rgb | (rgb >> 6 & 0x030303);
}


/* GGGGGGRR RRRBBBBB -> 0xRRGGBB */
static guint32
va_rgb (guint c)
{
   guint32 rgb = (c & 0x3e0) << 14 | (c & 0xfc00) | (c & 0x1f) << 3;
   return rgb | (rgb >> 5 & 0x070007) | (rgb >> 6 & 0x000300);
}


/* GGGRRRBB -> 0xRRGGBB */
static guint32
va_byte_rgb (guint c)
{
   return ((c & 0x1c) * 0x49 >> 3) << 16
      | ((c >> 5) * 0x49 >> 1) << 8
      | (c & 3) * 0x55;
}


/* the pixels reached by a chain have the colour; FALSE: out of the
   picture (broken) */
static gboolean
read_chain (Bits *b, guint32 *pix, gsize total, gsize at, guint width, guint32 color)
{
   gssize p = (gssize) at;

   for (;;) {
      switch (bits (b, 2)) {
      case 0:
         if (!bit (b)) return !b->over;
         p += bit (b) ? 2 : -2;
         break;
      case 1:
         p--;
         break;
      case 2:
         break;
      default:
         p++;
         break;
      }
      if (b->over) return FALSE;
      p += width;
      if (p < 0 || (gsize) p >= total) return FALSE;
      pix[p] = color;
   }
}


GimvImage *
pic_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[3];
   GByteArray *file;
   const guchar *buf;
   gsize len, pos, total, k;
   guint machine, depth, width, height, out_w, out_h, i;
   guint32 pal[256], color = 0, *pix = NULL;
   guchar *dest = NULL;
   gssize at = -1;
   guint steps = 0;
   Recent recent;
   Bits b;

   g_return_val_if_fail (loader, NULL);

   if (gimv_loader_peek (loader, head, 3) < 3) return NULL;
   if (head[0] != 'P' || head[1] != 'I' || head[2] != 'C') return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;
   buf = file->data;
   len = file->len;

   /* the comments */
   pos = 3;
   while (pos < len && buf[pos] != 0x1a) pos++;
   pos++;
   while (pos < len && buf[pos] != 0x00) pos++;
   pos++;

   if (pos + 8 > len) goto ERROR;
   machine = (buf[pos] << 8) | buf[pos + 1];
   depth   = (buf[pos + 2] << 8) | buf[pos + 3];
   width   = (buf[pos + 4] << 8) | buf[pos + 5];
   height  = (buf[pos + 6] << 8) | buf[pos + 7];
   pos += 8;

   switch (machine) {
   case PIC_X68K:
   case PIC_TOWNS_2:
      break;
   case PIC_TOWNS:
   case PIC_X68K_EXT:
      pos += 6;
      break;
   case PIC_VA:
   case PIC_VA_HALF:
      if (depth != 16) goto ERROR;
      break;
   default:
      goto ERROR;
   }
   if (depth != 4 && depth != 8 && depth != 15 && depth != 16) goto ERROR;

   out_w = width;
   out_h = machine == PIC_VA_HALF ? height * 2 : height;
   if (width == 0 || height == 0 || !gimv_loader_size_ok (out_w, out_h)) goto ERROR;

   if (depth <= 8) {
      guint n = 1u << depth;
      if (pos + n * 2 > len) goto ERROR;
      for (i = 0; i < n; i++, pos += 2)
         pal[i] = x68k_rgb ((buf[pos] << 8) | buf[pos + 1]);
   }

   total = (gsize) width * height;
   pix = g_try_malloc (total * sizeof (guint32));
   if (!pix) goto ERROR;
   for (k = 0; k < total; k++) pix[k] = NO_COLOR;

   recent_init (&recent);
   b.p = buf + MIN (pos, len);
   b.end = buf + len;
   b.n = 0;
   b.over = FALSE;

   /* what is read of a file cut short is shown */
   while (!b.over) {
      guint n = read_length (&b);

      if (n == 0) break;
      /* the run: the colour before, changed where a chain reached */
      while (--n > 0 && (gsize) (at + 1) < total) {
         at++;
         if (pix[at] == NO_COLOR)
            pix[at] = color;
         else
            color = pix[at];
      }
      if ((gsize) (at + 1) >= total) break;

      /* a new colour */
      if (depth <= 8) {
         color = pal[bits (&b, depth)];
      } else if (!bit (&b)) {
         guint c = bits (&b, depth);
         if (machine == PIC_VA || machine == PIC_VA_HALF)
            color = c;                         /* converted at the end */
         else
            color = x68k_rgb (depth == 15 ? c << 1 : c);
         recent_add (&recent, color);
      } else {
         color = recent_get (&recent, bits (&b, 7));
      }
      if (b.over) break;
      pix[++at] = color;
      if ((gsize) (at + 1) >= total) break;

      if (bit (&b) && !read_chain (&b, pix, total, at, width, color))
         break;

      if (++steps % 4096 == 0 && !gimv_image_loader_progress_update (loader))
         goto ERROR;
   }

   dest = g_try_malloc ((gsize) out_w * out_h * 3);
   if (!dest) goto ERROR;
   for (k = 0; k < total; k++) {
      guint32 c = pix[k] == NO_COLOR ? 0 : pix[k];

      if (machine == PIC_VA_HALF) {
         guint32 lo = va_byte_rgb (c & 0xff), hi = va_byte_rgb ((c >> 8) & 0xff);
         guchar *d = dest + k * 6;
         d[0] = lo >> 16; d[1] = lo >> 8; d[2] = lo;
         d[3] = hi >> 16; d[4] = hi >> 8; d[5] = hi;
      } else {
         guchar *d = dest + k * 3;
         if (machine == PIC_VA) c = va_rgb (c);
         d[0] = c >> 16; d[1] = c >> 8; d[2] = c;
      }
   }

   g_free (pix);
   g_byte_array_free (file, TRUE);
   return gimv_image_create_from_data (dest, out_w, out_h, FALSE);

ERROR:
   g_free (pix);
   g_free (dest);
   g_byte_array_free (file, TRUE);
   return NULL;
}
