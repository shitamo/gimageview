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
 * PIC2 loader: the format of A. Yanagisawa's pic2.x for the Sharp X68000
 * (up to 24 bits a pixel).  Written after the description of the format
 * in xvpic2.c of the Japanese extension of xv (IKEMOTO Masahiro, 1995),
 * and checked against Yanagisawa's own loader (pic2src, 1993).
 *
 * All numbers are big endian.
 *
 *   0    "P2DT"
 *   4    the author (18), the file name (8), CR LF, the title (30),
 *        CR LF, the saver (30), CR LF, 0x1a, 0
 *   98   u16 flags (1: a palette follows the header), u16 the number
 *   102  u32 the time, u32 the offset of the first block
 *   110  u16 bits a pixel (3, 6, ... 24), u16 x and y aspect of a dot,
 *        u16 width, height, u32 reserved
 *   124  [the palette: u8 bits, u16 count, count * RGB], the comment
 *        up to the first block
 *
 * A block: "P2SS", "P2SF", "P2BM" or "P2BI" (the first byte 0: the
 * end), u32 its size with this header, u16 flags (1: the colour of
 * "opaque" is transparent), u16 width, height, x, y, u32 opaque, u32
 * reserved; the data follow.  The blocks are drawn one on another.
 *
 * A pixel is R G B, depth / 3 bits each, R the highest.
 *
 * P2BM, P2BI: the pixels as bytes (3, 2 or 1 a pixel); 2: big endian
 * (P2BM) or little (P2BI), with 15 bits the X68000's GRB and a bit I.
 *
 * P2SF (bits, the highest first): the lines from the top.  A pixel is
 * the colour before (a run), the colour a chain from the line above
 * brought, or a new colour.  A run's length is k ones, a zero and k
 * bits v: v + 2^k - 1; 1023 means 1023 chain pixels before the next
 * length, more than 1023 one less.  A new colour: 0 and the colour
 * (G R B), kept in one of 256 caches of 64 (by the colour before), or
 * 1 and its place in the cache (6 bits).  After a new colour or a
 * chain pixel, a chain: 0 none, 11 down, 1011 one left, 1010 two left,
 * 1001 one right, 1000 two right.
 *
 * P2SS: 128 u16 probabilities, then a binary arithmetic code.  For each
 * pixel not reached by a chain, a bit tells whether the colour changes;
 * a new colour is found in one of 512 caches of 32 (by the pixel above)
 * or told as G, R, B near the mean of the pixels above and on the left.
 * Each changed pixel then tells where its chain goes on the next line.
 */

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

static GimvImage *pic2_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_pic2_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "PIC2",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        pic2_load,
   }
};

static const gchar *pic2_extensions[] = { "p2" };

static GimvMimeTypeEntry pic2_mime_types[] =
{
   {
      mime_type:      "image/x-pic2",
      description:    N_("The PIC2 image format (X68000)"),
      extensions:     pic2_extensions,
      extensions_len: G_N_ELEMENTS (pic2_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_pic2_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(pic2_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("PIC2 Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("PIC2 images (X68000)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


#define HEADER_SIZE   124
#define BLOCK_HEADER  26
#define PAD           4         /* room on both sides of a line */
#define MAX_BLOCKS    4096


typedef struct {
   const guchar *p, *end;
   guint         n;            /* the bit of *p next (0: the highest) */
   gboolean      over;         /* read past the end */
   guint         extra;        /* the bits read past the end */
} Bits;


static inline guint
bit (Bits *b)
{
   guint v;

   if (b->p >= b->end) {
      b->extra++;
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


static inline guint32
bits (Bits *b, guint n)
{
   guint32 v = 0;
   while (n--) v = (v << 1) | bit (b);
   return v;
}


static inline guint
be16 (const guchar *p)
{
   return (p[0] << 8) | p[1];
}


static inline guint32
be32 (const guchar *p)
{
   return ((guint32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}


/* a left shift, or a right one for n < 0 */
static inline guint32
shift (guint32 v, gint n)
{
   return n >= 0 ? v << n : v >> -n;
}


/* the fields of R and G change places */
static inline guint32
swap_rg (guint32 p, guint cb)
{
   guint32 mask = 0xff >> (8 - cb);
   return ((p << cb) & (mask << (cb * 2))) | ((p >> cb) & (mask << cb)) | (p & mask);
}


/* cb bits -> 8 bits, the bits repeated */
static inline guchar
widen (guint32 c, guint cb)
{
   gint to = 8;
   guint32 v = 0;

   do {
      to -= cb;
      v |= shift (c, to);
   } while (to >= 0);
   return v & 0xff;
}


/* the working lines of a block: the pixels of three lines and the
   marks of chains and of the changes around */
typedef struct {
   guint    width, height, depth, cb;
   Bits     b;
   guint32 *v[3];       /* the line before, this one, the next */
   gint16  *f[2];       /* chains reached: this line, the next */
   gint16  *g[3];       /* changes near: this line, next, the one after */
   guint32  cache[512][32];
   guint16  cache_pos[512];
   /* the arithmetic code */
   guint16  mul[16384];
   gint32   range, code;
   guint    hit_ctx;
} Work;


static gboolean
work_init (Work *w, guint width)
{
   guint i;

   memset (w->cache, 0, sizeof (w->cache));
   memset (w->cache_pos, 0, sizeof (w->cache_pos));
   for (i = 0; i < 3; i++) {
      w->v[i] = g_try_new0 (guint32, width + PAD * 2);
      w->g[i] = g_try_new0 (gint16, width + PAD * 2);
   }
   for (i = 0; i < 2; i++)
      w->f[i] = g_try_new0 (gint16, width + PAD * 2);
   for (i = 0; i < 3; i++)
      if (!w->v[i] || !w->g[i]) return FALSE;
   return w->f[0] && w->f[1];
}


static void
work_free (Work *w)
{
   guint i;

   for (i = 0; i < 3; i++) {
      g_free (w->v[i]);
      g_free (w->g[i]);
   }
   g_free (w->f[0]);
   g_free (w->f[1]);
}


/* the next line becomes this one */
static void
work_rotate (Work *w)
{
   guint32 *v = w->v[0];
   gint16 *f = w->f[0], *g = w->g[0];

   w->v[0] = w->v[1]; w->v[1] = w->v[2]; w->v[2] = v;
   w->f[0] = w->f[1]; w->f[1] = f;
   w->g[0] = w->g[1]; w->g[1] = w->g[2]; w->g[2] = g;
}

#define PREV(w)   ((w)->v[0] + PAD)
#define NOW(w)    ((w)->v[1] + PAD)
#define NEXT(w)   ((w)->v[2] + PAD)
#define FNOW(w)   ((w)->f[0] + PAD)
#define FNEXT(w)  ((w)->f[1] + PAD)


/* ---- P2SS ---- */

/* FALSE: the code is broken */
static inline gboolean
ac_bit (Work *w, guint ctx, guint *out)
{
   guint pp = w->mul[((w->range & 0x7f00) >> 1) + (ctx & 127)];

   if (w->code >= (gint32) pp) {
      w->code -= pp;
      w->range -= pp;
      *out = 1;
   } else {
      w->range = pp;
      *out = 0;
   }
   if (w->range <= 0 || w->range > 0xffff) return FALSE;
   while (!(w->range & 0x8000)) {
      w->code = (w->code << 1) | bit (&w->b);
      w->range <<= 1;
   }
   return w->code >= 0 && w->code < 0x10000;
}

/* the bit, or -1 when broken */
#define AC(w, ctx, var) G_STMT_START { \
      if (!ac_bit ((w), (ctx), &(var))) return -1; } G_STMT_END


/* 0 .. 255: k (0 .. 7) told in unary by c + k, then its k low bits by
   c + 8 ..., 255 for no stop */
static gint
ac_number (Work *w, guint c)
{
   guint k, i, v, n;

   for (k = 0; k < 8; k++) {
      AC (w, c + k, v);
      if (v) break;
   }
   if (k == 8) return 255;
   n = (1u << k) - 1;
   for (i = 0; i < k; i++) {
      AC (w, c + 8 + i, v);
      if (v) n += 1u << i;
   }
   return n;
}


/* a value told as its distance from bef: 0, +1, -1, +2, -2 ..., then
   the rest on the side with room */
static gint
ac_value (Work *w, guint c, gint bef)
{
   gint max = 0xff >> (8 - w->cb), n = ac_number (w, c);

   if (n < 0) return -1;
   if (bef > (max >> 1)) {
      if (n > (max - bef) * 2)
         n = max - n;
      else if (n & 1)
         n = n / 2 + bef + 1;
      else
         n = bef - n / 2;
   } else if (n <= bef * 2) {
      if (n & 1)
         n = n / 2 + bef + 1;
      else
         n = bef - n / 2;
   }
   return n & max;
}


/* where the chain of the pixel at x goes on the next line; -1: broken */
static gint
ac_chain (Work *w, gint x, guint32 color)
{
   static const gint dx[] = { 0, -1, 1, -2, 2 };
   gint16 f = FNOW (w)[x];
   guint c = 80 + 6 * (f < 0 ? -f : 0), v, i;

   AC (w, c, v);
   if (v) return 0;                     /* none */
   for (i = 0; i < 4; i++) {
      AC (w, c + 1 + i, v);
      if (v) break;
   }
   NEXT (w)[x + dx[i]] = color;
   FNEXT (w)[x + dx[i]] = -(gint16) (i + 1);
   return 0;
}


static gint64
ac_color (Work *w, gint x)
{
   guint cb = w->cb, k, v;
   guint32 above = PREV (w)[x], left = NOW (w)[x - 1], mask = 0xff >> (8 - cb), color;
   gint max = mask, r, g, b, g0, r0, b0;

   k = (shift (above, -(gint) (cb - 3) * 3) & 0x1c0)
      | (shift (above, -(gint) (cb - 3) * 2) & 0x038)
      | (shift (above, -(gint) (cb - 3)) & 0x007);
   if (cb == 5) k = swap_rg (k, 3);

   AC (w, w->hit_ctx, v);
   if (v) {
      /* a new colour, near the mean of above and left */
      w->hit_ctx = 16;
      r = (((above >> (cb * 2)) & mask) + ((left >> (cb * 2)) & mask)) >> 1;
      g = (((above >> cb) & mask) + ((left >> cb) & mask)) >> 1;
      b = ((above & mask) + (left & mask)) >> 1;

      if ((g0 = ac_value (w, 32, g)) < 0) return -1;
      r = CLAMP (r + g0 - g, 0, max);
      b = CLAMP (b + g0 - g, 0, max);
      if ((r0 = ac_value (w, 48, r)) < 0) return -1;
      if ((b0 = ac_value (w, 64, b)) < 0) return -1;

      w->cache_pos[k] = (w->cache_pos[k] - 1) & 31;
      color = (r0 << (cb * 2)) | (g0 << cb) | b0;
      w->cache[k][w->cache_pos[k]] = color;
   } else {
      /* from the cache: the colour found goes to the front, the one
         half way to its place */
      gint j = ac_number (w, 17);
      guint m = w->cache_pos[k], i, at;

      if (j < 0) return -1;
      w->hit_ctx = 15;
      i = (m + j / 2) & 31;
      at = (m + j) & 31;
      color = w->cache[k][at];
      w->cache[k][at] = w->cache[k][i];
      w->cache[k][i] = w->cache[k][m];
      w->cache[k][m] = color;
   }
   return color;
}


static gboolean
ss_init (Work *w)
{
   guint i;

   if (w->b.p + 256 + 2 > w->b.end) return FALSE;
   for (i = 0; i < 16384; i++) {
      guint p = be16 (w->b.p + (i & 127) * 2);
      guint m = (i / 128 + 128) * p / 256;
      w->mul[i] = m ? MIN (m, 0xffff) : 1;
   }
   w->b.p += 256;
   w->range = 0xffff;
   w->code = bits (&w->b, 16);
   w->hit_ctx = 16;
   return TRUE;
}


/* FALSE: stop (broken or cut short) */
static gboolean
ss_line (Work *w, guint y)
{
   guint32 color = y == 0 ? 0 : PREV (w)[w->width - 1];
   gboolean last = y + 1 >= w->height;
   gint x;
   guint v;

   NOW (w)[-1] = color;
   memset (FNEXT (w), 0, w->width * sizeof (gint16));
   memset (w->g[2] + PAD, 0, w->width * sizeof (gint16));

   for (x = 0; x < (gint) w->width; x++) {
      if (FNOW (w)[x] < 0) {
         color = NOW (w)[x];
      } else {
         if (!ac_bit (w, w->g[0][PAD + x], &v)) return FALSE;
         if (!v) {
            NOW (w)[x] = color;
            continue;
         }
         /* changes are likely near a change */
         w->g[0][PAD + x + 1]++;
         w->g[0][PAD + x + 2]++;
         w->g[1][PAD + x - 1]++;
         w->g[1][PAD + x]++;
         w->g[1][PAD + x + 1]++;
         w->g[2][PAD + x - 1]++;
         w->g[2][PAD + x]++;
         w->g[2][PAD + x + 1]++;

         {
            gint64 c = ac_color (w, x);
            if (c < 0) return FALSE;
            color = NOW (w)[x] = (guint32) c;
         }
      }
      if (!last && ac_chain (w, x, color) < 0) return FALSE;
   }
   /* the code reads a little ahead of what it tells */
   return w->b.extra <= 32;
}


/* ---- P2SF ---- */

static gint
sf_length (Bits *b)
{
   guint k = 0;

   while (bit (b)) {
      if (b->over || ++k > 24) return -1;
   }
   if (k == 0) return 0;
   return bits (b, k) + (1u << k) - 1;
}


static void
sf_chain (Work *w, gint x, guint32 color)
{
   gint at;

   if (!bit (&w->b)) return;
   if (bit (&w->b))
      at = x;
   else if (bit (&w->b))
      at = bit (&w->b) ? x - 1 : x - 2;
   else
      at = bit (&w->b) ? x + 1 : x + 2;
   NEXT (w)[at] = color;
   FNEXT (w)[at] = -1;
}


static guint32
sf_color (Work *w, guint32 before)
{
   guint32 (*cache)[64] = (guint32 (*)[64]) w->cache;
   guint k = shift (swap_rg (before, w->cb), 8 - (gint) w->depth) & 0xff;
   guint32 color;

   if (!bit (&w->b)) {
      w->cache_pos[k] = (w->cache_pos[k] - 1) & 63;
      color = swap_rg (bits (&w->b, w->depth), w->cb);
      cache[k][w->cache_pos[k]] = color;
   } else {
      color = cache[k][(w->cache_pos[k] + bits (&w->b, 6)) & 63];
   }
   return color;
}


/* the run left (range) and the chain pixels left before one (code) */
static gboolean
sf_next_length (Work *w)
{
   gint n = sf_length (&w->b);

   if (n < 0) return FALSE;
   w->code = 0;
   if (n == 1023)
      w->code = 1023;
   else if (n > 1023)
      n--;
   w->range = n;
   return TRUE;
}


static gboolean
sf_line (Work *w, guint y)
{
   guint32 color = 0;
   gint x;

   if (y == 0) {
      if (!sf_next_length (w)) return FALSE;
   } else {
      color = PREV (w)[w->width - 1];
   }
   memset (FNEXT (w), 0, w->width * sizeof (gint16));

   for (x = 0; x < (gint) w->width; x++) {
      if (FNOW (w)[x] < 0) {
         color = NOW (w)[x];
         sf_chain (w, x, color);
         if (w->code > 0 && --w->code == 0 && !sf_next_length (w))
            return FALSE;
      } else if (w->code > 0 || --w->range >= 0) {
         NOW (w)[x] = color;
      } else {
         color = NOW (w)[x] = sf_color (w, color);
         sf_chain (w, x, color);
         if (!sf_next_length (w)) return FALSE;
      }
      if (w->b.over) return FALSE;
   }
   return TRUE;
}


/* ---- P2BM, P2BI ---- */

static gboolean
raw_line (Work *w, gboolean little)
{
   guint bytes = (w->depth + 7) / 8, x;
   const guchar *p = w->b.p;

   if ((gsize) (w->b.end - p) < (gsize) w->width * bytes) return FALSE;
   for (x = 0; x < w->width; x++, p += bytes) {
      guint32 c;

      if (bytes >= 3) {
         c = ((guint32) p[0] << 16) | (p[1] << 8) | p[2];
      } else if (bytes == 2) {
         c = little ? (p[1] << 8) | p[0] : (p[0] << 8) | p[1];
         if (w->cb == 5) c = swap_rg (c >> 1, 5);
      } else {
         c = p[0];
      }
      NOW (w)[x] = c;
   }
   w->b.p = p;
   return TRUE;
}


typedef enum {
   BLOCK_SS, BLOCK_SF, BLOCK_BM, BLOCK_BI, BLOCK_NONE
} BlockType;


static BlockType
block_type (const guchar *id)
{
   if (!memcmp (id, "P2SS", 4)) return BLOCK_SS;
   if (!memcmp (id, "P2SF", 4)) return BLOCK_SF;
   if (!memcmp (id, "P2BM", 4)) return BLOCK_BM;
   if (!memcmp (id, "P2BI", 4)) return BLOCK_BI;
   return BLOCK_NONE;
}


/* the block at buf + pos drawn on dest; FALSE: cancelled */
static gboolean
draw_block (GimvImageLoader *loader, const guchar *buf, gsize pos, gsize end,
            guint depth, guchar *dest, guint canvas_w, guint canvas_h)
{
   BlockType type = block_type (buf + pos);
   const guchar *h = buf + pos + 8;
   guint flags = be16 (h), width = be16 (h + 2), height = be16 (h + 4);
   guint bx = be16 (h + 6), by = be16 (h + 8), y, x;
   guint32 opaque = be32 (h + 10);
   gboolean ok = TRUE, keep_going = TRUE;
   Work *w;

   if (width == 0 || height == 0) return TRUE;

   w = g_try_new0 (Work, 1);
   if (!w) return TRUE;
   if (!work_init (w, width)) goto END;
   w->width = width;
   w->height = height;
   w->depth = depth;
   w->cb = depth / 3;
   w->b.p = buf + pos + BLOCK_HEADER;
   w->b.end = buf + end;

   if (type == BLOCK_SS && !ss_init (w)) goto END;

   /* what is read of a block cut short is shown */
   for (y = 0; y < height && ok; y++) {
      switch (type) {
      case BLOCK_SS: ok = ss_line (w, y); break;
      case BLOCK_SF: ok = sf_line (w, y); break;
      case BLOCK_BM: ok = raw_line (w, FALSE); break;
      case BLOCK_BI: ok = raw_line (w, TRUE); break;
      default: ok = FALSE; break;
      }
      if (!ok && type != BLOCK_SS && type != BLOCK_SF) break;

      if (by + y < canvas_h) {
         guchar *d = dest + ((gsize) (by + y) * canvas_w + bx) * 3;
         guint32 mask = 0xff >> (8 - w->cb);

         for (x = 0; x < width && bx + x < canvas_w; x++, d += 3) {
            guint32 c = NOW (w)[x];

            if ((flags & 1) && c == opaque) continue;
            d[0] = widen ((c >> (w->cb * 2)) & mask, w->cb);
            d[1] = widen ((c >> w->cb) & mask, w->cb);
            d[2] = widen (c & mask, w->cb);
         }
      }
      work_rotate (w);

      if (y % 16 == 15 && !gimv_image_loader_progress_update (loader)) {
         keep_going = FALSE;
         break;
      }
   }

END:
   work_free (w);
   g_free (w);
   return keep_going;
}


GimvImage *
pic2_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[4];
   GByteArray *file;
   const guchar *buf;
   gsize len, pos, first;
   guint depth, xa, ya, width, height, n, drawn = 0;
   guchar *dest = NULL;

   g_return_val_if_fail (loader, NULL);

   if (gimv_loader_peek (loader, head, 4) < 4) return NULL;
   if (memcmp (head, "P2DT", 4)) return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;
   buf = file->data;
   len = file->len;
   if (len < HEADER_SIZE) goto ERROR;

   first  = be32 (buf + 106);
   depth  = be16 (buf + 110);
   xa     = be16 (buf + 112);
   ya     = be16 (buf + 114);
   width  = be16 (buf + 116);
   height = be16 (buf + 118);
   if (depth == 0 || depth > 24 || depth % 3) goto ERROR;
   if (first < HEADER_SIZE) goto ERROR;

   /* the picture holds all the blocks */
   for (pos = first, n = 0; pos + 8 <= len && n < MAX_BLOCKS; n++) {
      guint32 size = be32 (buf + pos + 4);

      if (buf[pos] == 0 || size < BLOCK_HEADER) break;
      if (block_type (buf + pos) != BLOCK_NONE && pos + BLOCK_HEADER <= len) {
         const guchar *h = buf + pos + 8;
         width  = MAX (width, be16 (h + 6) + be16 (h + 2));
         height = MAX (height, be16 (h + 8) + be16 (h + 4));
      }
      if (size > len - pos) break;
      pos += size;
   }
   if (width == 0 || height == 0 || !gimv_loader_size_ok (width, height)) goto ERROR;

   dest = g_try_malloc0 ((gsize) width * height * 3);
   if (!dest) goto ERROR;

   for (pos = first, n = 0; pos + BLOCK_HEADER <= len && n < MAX_BLOCKS; n++) {
      guint32 size = be32 (buf + pos + 4);
      gsize end = size > len - pos ? len : pos + size;

      if (buf[pos] == 0 || size < BLOCK_HEADER) break;
      if (block_type (buf + pos) != BLOCK_NONE) {
         if (!draw_block (loader, buf, pos, end, depth, dest, width, height))
            goto ERROR;
         drawn++;
      }
      if (end >= len) break;
      pos = end;
   }
   if (drawn == 0) goto ERROR;

   g_byte_array_free (file, TRUE);

   /* the dots as the header tells (4:3 for the X68000's 512 wide) */
   if (xa && ya && xa != ya && xa <= ya * 4 && ya <= xa * 4) {
      if (xa > ya)
         return gimv_loader_image_with_aspect (dest, width, height, xa, ya, 1, 1);
      return gimv_loader_image_with_aspect (dest, width, height, 1, 1, ya, xa);
   }
   return gimv_image_create_from_data (dest, width, height, FALSE);

ERROR:
   g_free (dest);
   g_byte_array_free (file, TRUE);
   return NULL;
}
