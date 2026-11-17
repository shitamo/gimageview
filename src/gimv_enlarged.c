/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: telling an enlarged copy from an image that is really
 * larger (see gimv_enlarged.h).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_enlarged.h"

#include <math.h>
#include <string.h>

#define PHASES     16                  /* places between two pixels, each way */
#define CLASSES    (PHASES * PHASES)
#define NF         17                  /* 4 x 4 pixels and a constant */
#define SAMPLES    150000              /* pixels of the large image looked at */
#define MIN_TRAIN  40                  /* for the weights of a place */
#define MIN_TESTED 2000
#define MIN_SIDE   16                  /* of the smallest image made */
#define FLAT       0.3                 /* less detail than this (in 0..255):
                                          nothing to tell by */

typedef struct {
   gint    w, h;
   gfloat *y;                          /* luminance, 0..255 */
} Lum;


static gboolean
lum_from_pixbuf (Lum *l, GdkPixbuf *pb)
{
   gint x, y, nc = gdk_pixbuf_get_n_channels (pb);
   gint rs = gdk_pixbuf_get_rowstride (pb);
   const guchar *px = gdk_pixbuf_read_pixels (pb);

   if (gdk_pixbuf_get_bits_per_sample (pb) != 8 || nc < 3) return FALSE;
   l->w = gdk_pixbuf_get_width (pb);
   l->h = gdk_pixbuf_get_height (pb);
   l->y = g_new (gfloat, (gsize) l->w * l->h);
   for (y = 0; y < l->h; y++) {
      const guchar *p = px + (gsize) y * rs;
      gfloat *o = l->y + (gsize) y * l->w;
      for (x = 0; x < l->w; x++, p += nc)
         o[x] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
   }
   return TRUE;
}


/* a small pseudo-random sequence: the same pixels every time */
static inline guint32
next_rand (guint32 *s)
{
   *s ^= *s << 13;
   *s ^= *s >> 17;
   *s ^= *s << 5;
   return *s;
}


static inline gfloat
at (const Lum *l, gint x, gint y)
{
   x = CLAMP (x, 0, l->w - 1);
   y = CLAMP (y, 0, l->h - 1);
   return l->y[(gsize) y * l->w + x];
}


/* the pixel (x, y) of large: its place in small (the class) and the
 * pixels of small around it */
static inline gint
features (const Lum *s, const Lum *l, gint x, gint y, gdouble f[NF])
{
   gdouble u = (x + 0.5) * s->w / l->w - 0.5;
   gdouble v = (y + 0.5) * s->h / l->h - 0.5;
   gint u0 = (gint) floor (u), v0 = (gint) floor (v);
   gint pu = MIN ((gint) ((u - u0) * PHASES), PHASES - 1);
   gint pv = MIN ((gint) ((v - v0) * PHASES), PHASES - 1);
   gint dx, dy, k = 0;

   for (dy = -1; dy <= 2; dy++)
      for (dx = -1; dx <= 2; dx++)
         f[k++] = at (s, u0 + dx, v0 + dy);
   f[k] = 1.0;
   return pv * PHASES + pu;
}


/* solves a x = b (a symmetric, n x n) by Cholesky with a little ridge;
 * FALSE when it cannot */
static gboolean
solve (gdouble *a, gdouble *b, gdouble *x, gint n)
{
   gdouble tr = 0, *L = g_alloca (sizeof (gdouble) * n * n), *z = g_alloca (sizeof (gdouble) * n);
   gint i, j, k;

   for (i = 0; i < n; i++) tr += a[i * n + i];
   for (i = 0; i < n; i++) a[i * n + i] += 1e-6 * tr / n + 1e-9;
   memset (L, 0, sizeof (gdouble) * n * n);
   for (i = 0; i < n; i++) {
      for (j = 0; j <= i; j++) {
         gdouble sum = a[i * n + j];
         for (k = 0; k < j; k++) sum -= L[i * n + k] * L[j * n + k];
         if (i == j) {
            if (sum <= 0) return FALSE;
            L[i * n + i] = sqrt (sum);
         } else {
            L[i * n + j] = sum / L[j * n + j];
         }
      }
   }
   for (i = 0; i < n; i++) {
      gdouble sum = b[i];
      for (k = 0; k < i; k++) sum -= L[i * n + k] * z[k];
      z[i] = sum / L[i * n + i];
   }
   for (i = n - 1; i >= 0; i--) {
      gdouble sum = z[i];
      for (k = i + 1; k < n; k++) sum -= L[k * n + i] * x[k];
      x[i] = sum / L[i * n + i];
   }
   return TRUE;
}


/* the mean error of large predicted from small by the best fitted
 * enlarging; < 0: too few pixels */
static gdouble
predict_error (const Lum *s, const Lum *l, GCancellable *cancel)
{
   gdouble *ata = g_new0 (gdouble, (gsize) CLASSES * NF * NF);
   gdouble *atb = g_new0 (gdouble, (gsize) CLASSES * NF);
   gdouble *coef = g_new0 (gdouble, (gsize) CLASSES * NF);
   gint *n_train = g_new0 (gint, CLASSES);
   gboolean *ok = g_new0 (gboolean, CLASSES);
   gdouble f[NF], err = 0;
   guint64 n_all = (guint64) l->w * l->h;
   gint n = (gint) MIN ((guint64) SAMPLES, n_all), i, j, k, c, tested = 0;
   guint32 seed;

   /* the training half */
   seed = 2463534242u;
   for (i = 0; i < n; i++) {
      guint32 r = next_rand (&seed);
      guint64 idx = n == (gint) n_all ? (guint64) i : (guint64) r % n_all;
      gint x = idx % l->w, y = idx / l->w;
      gdouble t;
      gdouble *A, *B;

      if (next_rand (&seed) & 1) continue;
      c = features (s, l, x, y, f);
      t = l->y[(gsize) y * l->w + x];
      A = ata + (gsize) c * NF * NF;
      B = atb + (gsize) c * NF;
      for (j = 0; j < NF; j++) {
         gdouble fj = f[j];
         B[j] += fj * t;
         for (k = 0; k <= j; k++) A[j * NF + k] += fj * f[k];
      }
      n_train[c]++;
      if ((i & 0xffff) == 0 && g_cancellable_is_cancelled (cancel)) goto out;
   }
   for (c = 0; c < CLASSES; c++) {
      gdouble *A = ata + (gsize) c * NF * NF;
      if (n_train[c] < MIN_TRAIN) continue;
      for (j = 0; j < NF; j++)
         for (k = j + 1; k < NF; k++) A[j * NF + k] = A[k * NF + j];
      ok[c] = solve (A, atb + (gsize) c * NF, coef + (gsize) c * NF, NF);
   }

   /* tried on the other half */
   seed = 2463534242u;
   for (i = 0; i < n; i++) {
      guint32 r = next_rand (&seed);
      guint64 idx = n == (gint) n_all ? (guint64) i : (guint64) r % n_all;
      gint x = idx % l->w, y = idx / l->w;
      gdouble p = 0;
      const gdouble *w;

      if (!(next_rand (&seed) & 1)) continue;
      c = features (s, l, x, y, f);
      if (!ok[c]) continue;
      w = coef + (gsize) c * NF;
      for (j = 0; j < NF; j++) p += w[j] * f[j];
      err += fabs (l->y[(gsize) y * l->w + x] - p);
      tested++;
   }

out:
   g_free (ata);
   g_free (atb);
   g_free (coef);
   g_free (n_train);
   g_free (ok);
   if (g_cancellable_is_cancelled (cancel) || tested < MIN_TESTED) return -1;
   return err / tested;
}


static gdouble
score (GdkPixbuf *small, GdkPixbuf *large, GCancellable *cancel)
{
   Lum s = { 0 }, l = { 0 }, s2 = { 0 };
   GdkPixbuf *smaller = NULL;
   gdouble fx, fy, e, eref, ret = -1;
   gint w2, h2;

   g_return_val_if_fail (GDK_IS_PIXBUF (small) && GDK_IS_PIXBUF (large), -1);

   fx = (gdouble) gdk_pixbuf_get_width (large) / gdk_pixbuf_get_width (small);
   fy = (gdouble) gdk_pixbuf_get_height (large) / gdk_pixbuf_get_height (small);
   if (MIN (fx, fy) < GIMV_ENLARGED_MIN_FACTOR) return -1;

   /* small made smaller by as much */
   w2 = (gint) floor (gdk_pixbuf_get_width (small) / fx + 0.5);
   h2 = (gint) floor (gdk_pixbuf_get_height (small) / fy + 0.5);
   if (w2 < MIN_SIDE || h2 < MIN_SIDE) return -1;
   smaller = gdk_pixbuf_scale_simple (small, w2, h2, GDK_INTERP_HYPER);
   if (!smaller) return -1;

   if (lum_from_pixbuf (&s, small) && lum_from_pixbuf (&l, large)
       && lum_from_pixbuf (&s2, smaller))
   {
      eref = predict_error (&s2, &s, cancel);
      if (eref >= FLAT) {
         e = predict_error (&s, &l, cancel);
         if (e >= 0) ret = e / eref;
      }
   }
   g_free (s.y);
   g_free (l.y);
   g_free (s2.y);
   g_object_unref (smaller);
   return ret;
}


gdouble
gimv_enlarged_score (GdkPixbuf *small, GdkPixbuf *large)
{
   return score (small, large, NULL);
}


static GdkPixbuf *
read_turned (const gchar *path)
{
   GdkPixbuf *pb = gdk_pixbuf_new_from_file (path, NULL), *turned;

   if (!pb) return NULL;
   turned = gdk_pixbuf_apply_embedded_orientation (pb);
   g_object_unref (pb);
   return turned;
}


gdouble
gimv_enlarged_score_files (const gchar *small, const gchar *large, GCancellable *cancel)
{
   GdkPixbuf *s, *l;
   gdouble ret = -1;

   if (g_cancellable_is_cancelled (cancel)) return -1;
   s = read_turned (small);
   if (!s) return -1;
   l = g_cancellable_is_cancelled (cancel) ? NULL : read_turned (large);
   if (l) ret = score (s, l, cancel);
   g_object_unref (s);
   if (l) g_object_unref (l);
   return ret;
}
