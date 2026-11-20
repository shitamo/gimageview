/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: how two alike pictures of the duplicate scan differ.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_dupl_classify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* the RGB of a pixel, over white when it has alpha */
static inline void
pixel_rgb (const guchar *p, gboolean alpha, gint rgb[3])
{
   gint c;

   if (!alpha) {
      rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
      return;
   }
   for (c = 0; c < 3; c++)
      rgb[c] = (p[c] * p[3] + 255 * (255 - p[3]) + 127) / 255;
}


gboolean
gimv_dupl_diff (GdkPixbuf *a, GdkPixbuf *b, gint block, GimvDuplDiff *d)
{
   gint w, h, ra, rb, na, nb, x, y, bw, bh, i;
   gboolean aa, ab;
   const guchar *pa, *pb;
   gdouble *blocks, total = 0.0, sum[3] = { 0, 0, 0 };
   gint *counts;
   guint over = 0;

   g_return_val_if_fail (GDK_IS_PIXBUF (a) && GDK_IS_PIXBUF (b) && d, FALSE);

   memset (d, 0, sizeof (*d));
   w = gdk_pixbuf_get_width (a);
   h = gdk_pixbuf_get_height (a);
   if (w != gdk_pixbuf_get_width (b) || h != gdk_pixbuf_get_height (b)) return FALSE;
   if (w <= 0 || h <= 0) return TRUE;
   if (block < 1) block = 1;

   pa = gdk_pixbuf_read_pixels (a);
   pb = gdk_pixbuf_read_pixels (b);
   ra = gdk_pixbuf_get_rowstride (a);
   rb = gdk_pixbuf_get_rowstride (b);
   na = gdk_pixbuf_get_n_channels (a);
   nb = gdk_pixbuf_get_n_channels (b);
   aa = gdk_pixbuf_get_has_alpha (a);
   ab = gdk_pixbuf_get_has_alpha (b);

   /* blocks of block x block (those at the edges smaller) */
   bw = (w + block - 1) / block;
   bh = (h + block - 1) / block;
   blocks = g_new0 (gdouble, (gsize) bw * bh);
   counts = g_new0 (gint, (gsize) bw * bh);

   for (y = 0; y < h; y++) {
      const guchar *qa = pa + (gsize) y * ra, *qb = pb + (gsize) y * rb;
      gint row = (y / block) * bw;
      for (x = 0; x < w; x++, qa += na, qb += nb) {
         gint ca[3], cb[3], c, dsum = 0;
         gdouble dp;
         pixel_rgb (qa, aa, ca);
         pixel_rgb (qb, ab, cb);
         for (c = 0; c < 3; c++) {
            gint dc = ca[c] - cb[c];
            sum[c] += dc;
            dsum += ABS (dc);
         }
         dp = dsum / 3.0;
         total += dp;
         blocks[row + x / block] += dp;
         counts[row + x / block]++;
      }
   }

   d->mean = total / ((gdouble) w * h);
   for (i = 0; i < 3; i++) {
      gdouble s = ABS (sum[i]) / ((gdouble) w * h);
      if (s > d->shift) d->shift = s;
   }

   /* again, with the shifts taken out */
   {
      gdouble m[3], res = 0.0;
      for (i = 0; i < 3; i++) m[i] = sum[i] / ((gdouble) w * h);
      for (y = 0; y < h; y++) {
         const guchar *qa = pa + (gsize) y * ra, *qb = pb + (gsize) y * rb;
         for (x = 0; x < w; x++, qa += na, qb += nb) {
            gint ca[3], cb[3], c;
            pixel_rgb (qa, aa, ca);
            pixel_rgb (qb, ab, cb);
            for (c = 0; c < 3; c++) res += ABS (ca[c] - cb[c] - m[c]);
         }
      }
      d->residual = res / 3.0 / ((gdouble) w * h);
   }
   for (i = 0; i < bw * bh; i++) {
      gdouble m = counts[i] ? blocks[i] / counts[i] : 0.0;
      if (m > d->block_max) d->block_max = m;
      if (m > 8.0) over++;
   }
   d->share = (gdouble) over / ((gdouble) bw * bh);

   g_free (blocks);
   g_free (counts);
   return TRUE;
}


/* some blocks far off the rest: a part changed */
static gboolean
local_change (const GimvDuplDiff *d)
{
   return d->block_max > d->mean * 3.0 + 6.0;
}


GimvDuplRelation
gimv_dupl_classify_small (const GimvDuplDiff *d)
{
   g_return_val_if_fail (d, GIMV_DUPL_REL_DIFFERENT);

   /* small everywhere, the shifts of the colours taken out */
   if (!local_change (d) && d->residual <= 8.0) return GIMV_DUPL_REL_RESAVED;
   /* a few blocks only: a difference picture, writing */
   if (local_change (d) && d->share < 0.05) return GIMV_DUPL_REL_PARTLY;
   return GIMV_DUPL_REL_DIFFERENT;
}


GimvDuplRelation
gimv_dupl_classify_full (const GimvDuplDiff *d, gboolean lossy)
{
   g_return_val_if_fail (d, GIMV_DUPL_REL_DIFFERENT);

   if (d->mean == 0.0) return GIMV_DUPL_REL_PIXELS;
   /* the noise of a JPEG of low quality reaches 12 or so in the busiest
      blocks: a change must stand out more */
   if (lossy ? d->block_max > d->mean * 3.0 + 24.0 : local_change (d))
      return GIMV_DUPL_REL_PARTLY;
   if (d->shift > 1.0) return GIMV_DUPL_REL_ADJUSTED;
   return GIMV_DUPL_REL_RESAVED;
}


GimvDuplRelation
gimv_dupl_compare_small (GdkPixbuf *a, GdkPixbuf *b)
{
   gint wa, ha, wb, hb;
   GdkPixbuf *sb;
   GimvDuplDiff d;
   gdouble cross;

   g_return_val_if_fail (GDK_IS_PIXBUF (a) && GDK_IS_PIXBUF (b), GIMV_DUPL_REL_DIFFERENT);

   wa = gdk_pixbuf_get_width (a);  ha = gdk_pixbuf_get_height (a);
   wb = gdk_pixbuf_get_width (b);  hb = gdk_pixbuf_get_height (b);
   if (wa <= 0 || ha <= 0 || wb <= 0 || hb <= 0) return GIMV_DUPL_REL_DIFFERENT;

   /* the shape within 2 % (the small sizes are rounded) */
   cross = (gdouble) wa * hb - (gdouble) wb * ha;
   if (ABS (cross) > 0.02 * (gdouble) wa * hb) return GIMV_DUPL_REL_DIFFERENT;

   if (wa == wb && ha == hb)
      sb = g_object_ref (b);
   else
      sb = gdk_pixbuf_scale_simple (b, wa, ha, GDK_INTERP_BILINEAR);
   if (!sb) return GIMV_DUPL_REL_DIFFERENT;
   gimv_dupl_diff (a, sb, 8, &d);
   g_object_unref (sb);
   return gimv_dupl_classify_small (&d);
}


GdkPixbuf *
gimv_dupl_load (const gchar *path, gint size)
{
   GdkPixbuf *px, *oriented;

   g_return_val_if_fail (path, NULL);

   if (size > 0)
      px = gdk_pixbuf_new_from_file_at_scale (path, size, size, TRUE, NULL);
   else
      px = gdk_pixbuf_new_from_file (path, NULL);
   if (!px) return NULL;
   oriented = gdk_pixbuf_apply_embedded_orientation (px);
   g_object_unref (px);
   return oriented;
}


/******************************************************************************
 *   the quality of a JPEG file
 ******************************************************************************/

/* the luminance table of the JPEG standard (quality 50) */
static const guint8 std_luminance[64] = {
   16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
   14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
   18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
   49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};


static gint
cmp_int (gconstpointer a, gconstpointer b)
{
   return *(const gint *) a - *(const gint *) b;
}


/* the quality whose table (sorted) is nearest to table (sorted) */
static gint
quality_of_table (const gint table[64])
{
   gint q, best = -1;
   glong best_d = G_MAXLONG;

   for (q = 1; q <= 100; q++) {
      gint s = q < 50 ? 5000 / q : 200 - q * 2, t[64], i;
      glong d = 0;
      for (i = 0; i < 64; i++)
         t[i] = CLAMP ((std_luminance[i] * s + 50) / 100, 1, 255);
      qsort (t, 64, sizeof (gint), cmp_int);
      for (i = 0; i < 64; i++) d += ABS (table[i] - t[i]);
      if (d < best_d) {
         best_d = d;
         best = q;
      }
   }
   return best;
}


gint
gimv_dupl_jpeg_quality_of (const guchar *buf, gsize len)
{
   gsize pos = 2;

   if (!buf || len < 4 || buf[0] != 0xff || buf[1] != 0xd8) return -1;

   while (pos + 4 <= len) {
      guint marker, seglen;
      gsize end;

      if (buf[pos] != 0xff) return -1;
      marker = buf[pos + 1];
      if (marker == 0xff) { pos++; continue; }       /* fill */
      if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7)) { pos += 2; continue; }
      if (marker == 0xd9 || marker == 0xda) return -1;   /* no table before the data */
      seglen = (buf[pos + 2] << 8) | buf[pos + 3];
      if (seglen < 2) return -1;
      end = pos + 2 + seglen;
      if (marker == 0xdb) {
         gsize p = pos + 4;
         /* the tables of the segment: the first of id 0 (luminance) */
         while (p < end && p < len) {
            guint pq = buf[p] >> 4, tq = buf[p] & 0x0f, i;
            gsize n = pq ? 128 : 64;
            gint table[64];
            if (p + 1 + n > len || p + 1 + n > end) return -1;
            if (tq == 0) {
               for (i = 0; i < 64; i++)
                  table[i] = pq ? (buf[p + 1 + 2 * i] << 8) | buf[p + 2 + 2 * i]
                                : buf[p + 1 + i];
               qsort (table, 64, sizeof (gint), cmp_int);
               return quality_of_table (table);
            }
            p += 1 + n;
         }
      }
      pos = end;
   }
   return -1;
}


gint
gimv_dupl_jpeg_quality (const gchar *path)
{
   guchar buf[65536];
   gsize len = 0;
   FILE *f;
   gint q;

   g_return_val_if_fail (path, -1);

   f = fopen (path, "rb");
   if (!f) return -1;
   len = fread (buf, 1, sizeof (buf), f);
   fclose (f);
   q = gimv_dupl_jpeg_quality_of (buf, len);
   return q;
}
