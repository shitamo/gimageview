/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: joining the selected images into a new one.
 *
 * The images (two or more, in the order chosen) are put one below another,
 * side by side, or in a grid, made the same width (height) or not.  Where
 * one photo was split into pieces that overlap, the overlap of each two
 * next to each other is looked for (the end of the first and the start of
 * the second most alike: the JPEG is not quite the same twice) and can be
 * put right by hand, with the seam shown near its own size (also as the
 * difference of the two).  Saved next to the first image as
 * "name_join.jpg" (never over another file).
 *
 * The images are read, the overlaps looked for and the new image made and
 * written in threads.  Those GdkPixbuf can not read (MAG, Pi, ... of gimv's
 * own loaders) are read after, in the main loop (the loaders are not made
 * for threads), as they are shown (their pixels the shape of their machine).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_join_dialog.h"

#include <math.h>
#include <string.h>

#include "gimageview.h"
#include "gimv_crop.h"
#include "gimv_image.h"
#include "gimv_resize_dialog.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"
#include "prefs.h"


typedef enum {
   JOIN_VERTICAL,
   JOIN_HORIZONTAL,
   JOIN_GRID,
} JoinMode;

typedef enum {
   FIT_SMALLER,
   FIT_LARGER,
   FIT_NONE,
} JoinFit;

#define PREVIEW_SIZE 1200   /* [px] the longer side of the images shown */
#define SEARCH_SIZE  256    /* [px] across: the overlap looked for first */


typedef struct {
   gchar     *path;
   gchar     *error;     /* of GdkPixbuf: read by gimv's loaders then */
   GdkPixbuf *full;      /* turned as its EXIF says */
   GdkPixbuf *preview;
   gint       w, h;
} Item;

typedef struct {
   gint x, y, w, h;      /* in the new image */
} Place;

typedef struct Dialog_Tag {
   gint          ref;
   gboolean      closed;
   GtkWindow    *parent;
   GPtrArray    *paths;
   GPtrArray    *items;      /* Item *, NULL till read */

   GtkWidget    *window;
   GtkWidget    *list;
   GtkWidget    *mode_drop;
   GtkWidget    *cols_spin;
   GtkWidget    *cols_box;
   GtkWidget    *fit_drop;
   GtkWidget    *auto_check;
   GtkWidget    *quality_scale;
   GtkWidget    *metadata_check;
   GtkWidget    *comment_check;
   GtkWidget    *name_label;
   GtkWidget    *preview;
   GtkWidget    *size_label;
   GtkWidget    *seam_box;
   GtkWidget    *seam_drop;
   GtkWidget    *overlap_scale;
   GtkWidget    *overlap_spin;
   GtkWidget    *auto_label;
   GtkWidget    *diff_check;
   GtkWidget    *detail;
   GtkWidget    *legend;     /* how to read the detail */
   GtkWidget    *status;
   GtkWidget    *show_button;
   GtkWidget    *close_button;
   GtkWidget    *save_button;

   gint         *overlap;    /* [px of the new image] between i and i + 1 */
   gint         *found;      /* the ones found, -1: not looked for */
   gint          seam;       /* shown in the detail */
   gboolean      setting;    /* the controls set by the program */
   guint         detect_gen;
   GCancellable *detect_cancel;
   gboolean      detecting;
   gboolean      saving;
   gchar        *saved;
} Dialog;


static Dialog *
dialog_ref (Dialog *d)
{
   d->ref++;
   return d;
}


static void
item_free (gpointer p)
{
   Item *it = p;
   if (!it) return;
   g_free (it->path);
   g_free (it->error);
   g_clear_object (&it->full);
   g_clear_object (&it->preview);
   g_free (it);
}


static void
dialog_unref (Dialog *d)
{
   if (--d->ref > 0) return;
   if (d->detect_cancel) {
      g_cancellable_cancel (d->detect_cancel);
      g_object_unref (d->detect_cancel);
   }
   g_ptr_array_unref (d->paths);
   if (d->items) g_ptr_array_unref (d->items);
   g_free (d->overlap);
   g_free (d->found);
   g_free (d->saved);
   g_free (d);
}


static void
set_status (Dialog *d, const gchar *text, gboolean error)
{
   if (d->closed) return;
   gtk_label_set_text (GTK_LABEL (d->status), text ? text : "");
   if (error) gtk_widget_add_css_class (d->status, "error");
   else gtk_widget_remove_css_class (d->status, "error");
}


static JoinMode
mode_of (Dialog *d)
{
   return gtk_drop_down_get_selected (GTK_DROP_DOWN (d->mode_drop));
}


static JoinFit
fit_of (Dialog *d)
{
   return gtk_drop_down_get_selected (GTK_DROP_DOWN (d->fit_drop));
}


static guint
n_items (Dialog *d)
{
   return d->items ? d->items->len : 0;
}


static Item *
item_at (Dialog *d, guint i)
{
   return g_ptr_array_index (d->items, i);
}


/*** where each image goes ***/

/* the places of the images in the new image (of the size *cw x *ch) */
static Place *
layout (Dialog *d, gint *cw, gint *ch)
{
   guint n = n_items (d), i;
   JoinMode mode = mode_of (d);
   JoinFit fit = fit_of (d);
   Place *pl = g_new0 (Place, MAX (n, 1));
   gint target = 0, pos = 0, extent = 0;

   *cw = *ch = 0;
   if (n == 0) return pl;

   /* the common width (height side by side) */
   for (i = 0; i < n; i++) {
      Item *it = item_at (d, i);
      gint side = mode == JOIN_HORIZONTAL ? it->h : it->w;
      if (i == 0) target = side;
      else if (fit == FIT_SMALLER) target = MIN (target, side);
      else target = MAX (target, side);
   }
   for (i = 0; i < n; i++) {
      Item *it = item_at (d, i);
      if (fit == FIT_NONE) {
         pl[i].w = it->w;
         pl[i].h = it->h;
      } else if (mode == JOIN_HORIZONTAL) {
         pl[i].h = target;
         pl[i].w = MAX (1, (gint) floor ((gdouble) it->w * target / it->h + 0.5));
      } else {
         pl[i].w = target;
         pl[i].h = MAX (1, (gint) floor ((gdouble) it->h * target / it->w + 0.5));
      }
   }

   if (mode == JOIN_GRID) {
      gint cols = MAX (1, gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (d->cols_spin)));
      gint cell_w = 0, row_h = 0, y = 0;
      for (i = 0; i < n; i++) cell_w = MAX (cell_w, pl[i].w);
      for (i = 0; i < n; i++) {
         gint col = i % cols;
         if (col == 0 && i > 0) {
            y += row_h;
            row_h = 0;
         }
         pl[i].x = col * cell_w + (cell_w - pl[i].w) / 2;
         pl[i].y = y;
         row_h = MAX (row_h, pl[i].h);
      }
      *cw = cell_w * MIN ((gint) n, cols);
      *ch = y + row_h;
      return pl;
   }

   for (i = 0; i < n; i++) {
      gint along = mode == JOIN_HORIZONTAL ? pl[i].w : pl[i].h;
      if (i > 0) {
         gint before = mode == JOIN_HORIZONTAL ? pl[i - 1].w : pl[i - 1].h;
         gint ov = CLAMP (d->overlap[i - 1], 0, MIN (before, along) - 1);
         pos -= ov;
      }
      if (mode == JOIN_HORIZONTAL) pl[i].x = pos; else pl[i].y = pos;
      pos += along;
      extent = MAX (extent, mode == JOIN_HORIZONTAL ? pl[i].h : pl[i].w);
   }
   for (i = 0; i < n; i++) {
      if (mode == JOIN_HORIZONTAL) pl[i].y = (extent - pl[i].h) / 2;
      else pl[i].x = (extent - pl[i].w) / 2;
   }
   *cw = mode == JOIN_HORIZONTAL ? pos : extent;
   *ch = mode == JOIN_HORIZONTAL ? extent : pos;
   return pl;
}


/*** the overlaps ***/

typedef struct {
   guint8 *data;      /* [along][across] */
   gint    along, across;
} Gray;


static void
gray_free (Gray *g)
{
   g_free (g->data);
   g->data = NULL;
}


/* gray of a pixbuf; across: rows (TRUE: the columns, side by side) */
static Gray
gray_of (GdkPixbuf *p, gboolean transpose)
{
   gint w = gdk_pixbuf_get_width (p), h = gdk_pixbuf_get_height (p);
   gint nc = gdk_pixbuf_get_n_channels (p), rs = gdk_pixbuf_get_rowstride (p);
   const guchar *px = gdk_pixbuf_read_pixels (p);
   Gray g;
   gint x, y;

   g.along = transpose ? w : h;
   g.across = transpose ? h : w;
   g.data = g_malloc ((gsize) w * h);
   for (y = 0; y < h; y++) {
      const guchar *q = px + (gsize) y * rs;
      for (x = 0; x < w; x++, q += nc) {
         guint8 v = (q[0] * 3 + q[1] * 6 + q[2]) / 10;
         if (transpose) g.data[(gsize) x * h + y] = v;
         else g.data[(gsize) y * w + x] = v;
      }
   }
   return g;
}


/* the end of a (its last k lines) and the start of b: the mean
   difference; *texture: how much the lines of a vary */
static gdouble
cost_of (const Gray *a, const Gray *b, gint k, gint step_line, gint step_px,
         gdouble *texture)
{
   gint across = MIN (a->across, b->across), i, j;
   gdouble sum = 0, s1 = 0, s2 = 0;
   gsize n = 0;

   for (i = 0; i < k; i += step_line) {
      const guint8 *la = a->data + (gsize) (a->along - k + i) * a->across;
      const guint8 *lb = b->data + (gsize) i * b->across;
      for (j = 0; j < across; j += step_px) {
         sum += ABS ((gint) la[j] - (gint) lb[j]);
         s1 += la[j];
         s2 += (gdouble) la[j] * la[j];
         n++;
      }
   }
   if (n == 0) {
      *texture = 0;
      return 255;
   }
   *texture = sqrt (MAX (0, s2 / n - (s1 / n) * (s1 / n)));
   return sum / n;
}


/* the overlap of a and b in lines of them, 0: none */
static gint
search (const Gray *a, const Gray *b, gint k_min, gint k_max, gint step_px)
{
   gint k, best = 0;
   gdouble best_cost = G_MAXDOUBLE;

   for (k = k_min; k <= k_max; k++) {
      gdouble texture, cost;
      cost = cost_of (a, b, k, MAX (1, k / 64), step_px, &texture);
      /* a plain band (white) matches anywhere: not taken */
      if (texture < 6) continue;
      if (cost < 10 && cost < texture * 0.35 && cost < best_cost) {
         best_cost = cost;
         best = k;
      }
   }
   return best;
}


/* the end of the pixbuf p (as it is in the new image: w x h) in lines
   lines of it (start: its first lines), as gray */
static Gray
strip_of (GdkPixbuf *p, gint w, gint h, gboolean side, gboolean start, gint lines)
{
   gint sw = gdk_pixbuf_get_width (p), sh = gdk_pixbuf_get_height (p);
   gdouble sx = (gdouble) w / sw, sy = (gdouble) h / sh;
   GdkPixbuf *dest;
   Gray g;

   if (side) {
      lines = MIN (lines, w);
      dest = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, lines, h);
      gdk_pixbuf_scale (p, dest, 0, 0, lines, h, start ? 0 : -(w - lines), 0, sx, sy,
                        GDK_INTERP_BILINEAR);
   } else {
      lines = MIN (lines, h);
      dest = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, lines);
      gdk_pixbuf_scale (p, dest, 0, 0, w, lines, 0, start ? 0 : -(h - lines), sx, sy,
                        GDK_INTERP_BILINEAR);
   }
   g = gray_of (dest, side);
   g_object_unref (dest);
   return g;
}


typedef struct {
   GPtrArray *items;     /* Item *, referenced pixbufs */
   Place     *places;
   gboolean   side;      /* side by side */
   guint      n;
   gint      *found;
} DetectJob;


static void
detect_job_free (gpointer p)
{
   DetectJob *job = p;
   g_ptr_array_unref (job->items);
   g_free (job->places);
   g_free (job->found);
   g_free (job);
}


static void
detect_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   DetectJob *job = data;
   guint i;

   for (i = 0; i + 1 < job->n && !g_cancellable_is_cancelled (cancel); i++) {
      Item *ia = g_ptr_array_index (job->items, i), *ib = g_ptr_array_index (job->items, i + 1);
      Place *pa = &job->places[i], *pb = &job->places[i + 1];
      gint across = job->side ? pa->h : pa->w;
      gint la = job->side ? pa->w : pa->h, lb = job->side ? pb->w : pb->h;
      gdouble r;
      GdkPixbuf *sa, *sb;
      Gray ga, gb;
      gint k, k_full, lo, hi, best = 0;
      gdouble best_cost = G_MAXDOUBLE;

      job->found[i] = 0;
      /* not the same width (height): they do not go on from each other */
      if (across != (job->side ? pb->h : pb->w)) continue;

      /* small first */
      r = MIN (1.0, (gdouble) SEARCH_SIZE / across);
      sa = job->side
         ? gdk_pixbuf_scale_simple (ia->full, MAX (1, (gint) (la * r)), MAX (1, (gint) (across * r)),
                                    GDK_INTERP_BILINEAR)
         : gdk_pixbuf_scale_simple (ia->full, MAX (1, (gint) (across * r)), MAX (1, (gint) (la * r)),
                                    GDK_INTERP_BILINEAR);
      sb = job->side
         ? gdk_pixbuf_scale_simple (ib->full, MAX (1, (gint) (lb * r)), MAX (1, (gint) (across * r)),
                                    GDK_INTERP_BILINEAR)
         : gdk_pixbuf_scale_simple (ib->full, MAX (1, (gint) (across * r)), MAX (1, (gint) (lb * r)),
                                    GDK_INTERP_BILINEAR);
      ga = gray_of (sa, job->side);
      gb = gray_of (sb, job->side);
      g_object_unref (sa);
      g_object_unref (sb);
      k = search (&ga, &gb, 3, MIN (ga.along, gb.along) - 1, 1);
      gray_free (&ga);
      gray_free (&gb);
      if (k <= 0 || g_cancellable_is_cancelled (cancel)) continue;

      /* then near it in the size of the new image */
      k_full = (gint) floor (k / r + 0.5);
      lo = MAX (1, k_full - (gint) ceil (2 / r));
      hi = MIN (MIN (la, lb) - 1, k_full + (gint) ceil (2 / r));
      ga = strip_of (ia->full, pa->w, pa->h, job->side, FALSE, hi);
      gb = strip_of (ib->full, pb->w, pb->h, job->side, TRUE, hi);
      for (k = lo; k <= hi && k <= ga.along && k <= gb.along; k++) {
         gdouble texture, cost = cost_of (&ga, &gb, k, MAX (1, k / 200),
                                          MAX (1, across / 1000), &texture);
         if (cost < best_cost) {
            best_cost = cost;
            best = k;
         }
      }
      gray_free (&ga);
      gray_free (&gb);
      job->found[i] = best > 0 ? best : k_full;
   }
   g_task_return_boolean (task, TRUE);
}


static void update_all (Dialog *d);
static void remember (Dialog *d);

static void
detect_done (GObject *source, GAsyncResult *res, gpointer data)
{
   Dialog *d = data;
   DetectJob *job = g_task_get_task_data (G_TASK (res));
   guint gen = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (res), "gen"));

   if (!d->closed && gen == d->detect_gen && job->n == n_items (d)) {
      guint i;
      d->detecting = FALSE;
      for (i = 0; i + 1 < job->n; i++) {
         d->found[i] = job->found[i];
         d->overlap[i] = job->found[i];
      }
      set_status (d, NULL, FALSE);
      update_all (d);
   }
   dialog_unref (d);
}


static void
detect_start (Dialog *d)
{
   DetectJob *job;
   GTask *task;
   gint cw, ch;
   guint i;

   if (d->detect_cancel) {
      g_cancellable_cancel (d->detect_cancel);
      g_clear_object (&d->detect_cancel);
   }
   d->detect_gen++;
   for (i = 0; i + 1 < n_items (d); i++) d->found[i] = -1;
   if (n_items (d) < 2 || mode_of (d) == JOIN_GRID
       || !gtk_check_button_get_active (GTK_CHECK_BUTTON (d->auto_check)))
   {
      d->detecting = FALSE;
      return;
   }

   job = g_new0 (DetectJob, 1);
   job->items = g_ptr_array_ref (d->items);
   job->n = n_items (d);
   job->side = mode_of (d) == JOIN_HORIZONTAL;
   job->found = g_new0 (gint, job->n);
   /* the sizes without overlaps */
   for (i = 0; i + 1 < job->n; i++) d->overlap[i] = 0;
   job->places = layout (d, &cw, &ch);

   d->detecting = TRUE;
   d->detect_cancel = g_cancellable_new ();
   set_status (d, _("Looking for the overlaps..."), FALSE);
   task = g_task_new (NULL, d->detect_cancel, detect_done, dialog_ref (d));
   g_object_set_data (G_OBJECT (task), "gen", GUINT_TO_POINTER (d->detect_gen));
   g_task_set_task_data (task, job, detect_job_free);
   g_task_run_in_thread (task, detect_thread);
   g_object_unref (task);
}


/*** what is shown ***/

static void
draw_preview (GtkDrawingArea *area, cairo_t *cr, gint aw, gint ah, gpointer data)
{
   Dialog *d = data;
   Place *pl;
   gint cw, ch;
   gdouble s, ox, oy;
   guint i;

   if (n_items (d) == 0) return;
   pl = layout (d, &cw, &ch);
   if (cw <= 0 || ch <= 0) {
      g_free (pl);
      return;
   }
   s = MIN ((gdouble) (aw - 8) / cw, (gdouble) (ah - 8) / ch);
   ox = (aw - cw * s) / 2;
   oy = (ah - ch * s) / 2;

   cairo_set_source_rgb (cr, 1, 1, 1);
   cairo_rectangle (cr, ox, oy, cw * s, ch * s);
   cairo_fill (cr);

   G_GNUC_BEGIN_IGNORE_DEPRECATIONS
   for (i = 0; i < n_items (d); i++) {
      Item *it = item_at (d, i);
      gdouble pw = gdk_pixbuf_get_width (it->preview), ph = gdk_pixbuf_get_height (it->preview);
      cairo_save (cr);
      cairo_rectangle (cr, ox + pl[i].x * s, oy + pl[i].y * s, pl[i].w * s, pl[i].h * s);
      cairo_clip (cr);
      cairo_translate (cr, ox + pl[i].x * s, oy + pl[i].y * s);
      cairo_scale (cr, pl[i].w * s / pw, pl[i].h * s / ph);
      gdk_cairo_set_source_pixbuf (cr, it->preview, 0, 0);
      cairo_paint (cr);
      cairo_restore (cr);
   }
   G_GNUC_END_IGNORE_DEPRECATIONS

   /* the seams */
   if (mode_of (d) != JOIN_GRID) {
      gdouble dash[] = { 4, 3 };
      cairo_set_dash (cr, dash, 2, 0);
      cairo_set_line_width (cr, 1);
      for (i = 1; i < n_items (d); i++) {
         if ((gint) i - 1 == d->seam) cairo_set_source_rgb (cr, 0.9, 0.1, 0.1);
         else cairo_set_source_rgba (cr, 0.9, 0.1, 0.1, 0.45);
         if (mode_of (d) == JOIN_HORIZONTAL) {
            cairo_move_to (cr, floor (ox + pl[i].x * s) + 0.5, oy);
            cairo_line_to (cr, floor (ox + pl[i].x * s) + 0.5, oy + ch * s);
         } else {
            cairo_move_to (cr, ox, floor (oy + pl[i].y * s) + 0.5);
            cairo_line_to (cr, ox + cw * s, floor (oy + pl[i].y * s) + 0.5);
         }
         cairo_stroke (cr);
      }
   }
   g_free (pl);
}


/* item i into dest (the part of the new image from x0, y0 in the scale
   s); diff: the difference with what is there where both are */
static void
render_item (Item *it, Place *p, GdkPixbuf *dest, gdouble x0, gdouble y0, gdouble s,
             gboolean diff)
{
   gint dw = gdk_pixbuf_get_width (dest), dh = gdk_pixbuf_get_height (dest);
   gdouble ix = (p->x - x0) * s, iy = (p->y - y0) * s;
   gint rx0 = MAX (0, (gint) floor (ix)), ry0 = MAX (0, (gint) floor (iy));
   gint rx1 = MIN (dw, (gint) ceil (ix + p->w * s)), ry1 = MIN (dh, (gint) ceil (iy + p->h * s));
   GdkPixbuf *tmp;
   gint x, y;

   if (rx1 <= rx0 || ry1 <= ry0) return;
   tmp = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, dw, dh);
   gdk_pixbuf_fill (tmp, 0xffffffff);
   gdk_pixbuf_scale (it->full, tmp, rx0, ry0, rx1 - rx0, ry1 - ry0, ix, iy,
                     s * p->w / it->w, s * p->h / it->h,
                     s >= 0.75 ? GDK_INTERP_BILINEAR : GDK_INTERP_TILES);
   if (!diff) {
      gdk_pixbuf_copy_area (tmp, rx0, ry0, rx1 - rx0, ry1 - ry0, dest, rx0, ry0);
   } else {
      gint rs = gdk_pixbuf_get_rowstride (dest), rt = gdk_pixbuf_get_rowstride (tmp);
      guchar *pd = gdk_pixbuf_get_pixels (dest);
      const guchar *pt = gdk_pixbuf_read_pixels (tmp);
      for (y = ry0; y < ry1; y++)
         for (x = rx0; x < rx1; x++) {
            guchar *a = pd + (gsize) y * rs + x * 3;
            const guchar *b = pt + (gsize) y * rt + x * 3;
            a[0] = ABS (a[0] - b[0]);
            a[1] = ABS (a[1] - b[1]);
            a[2] = ABS (a[2] - b[2]);
         }
   }
   g_object_unref (tmp);
}


/* the seam chosen near its own size: the end of the first image, the start
   of the second (the overlap as their difference if asked: dark where they
   are the same) */
static void
draw_detail (GtkDrawingArea *area, cairo_t *cr, gint aw, gint ah, gpointer data)
{
   Dialog *d = data;
   gboolean side = mode_of (d) == JOIN_HORIZONTAL;
   gboolean diff = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->diff_check));
   Place *pl;
   GdkPixbuf *dest;
   gint cw, ch, a = d->seam, b = d->seam + 1, ov;
   gdouble s, x0, y0, center;

   if (n_items (d) < 2 || mode_of (d) == JOIN_GRID || aw <= 0 || ah <= 0
       || b >= (gint) n_items (d))
      return;
   pl = layout (d, &cw, &ch);
   ov = side ? pl[a].x + pl[a].w - pl[b].x : pl[a].y + pl[a].h - pl[b].y;
   ov = MAX (0, ov);
   /* the whole width near its own size, not smaller than a third */
   s = MIN (1.0, MAX ((gdouble) (side ? ah : aw) / (side ? ch : cw), 0.33));
   if (side) {
      center = pl[b].x + ov / 2.0;
      x0 = center - aw / (2 * s);
      y0 = ch / 2.0 - ah / (2 * s);
   } else {
      center = pl[b].y + ov / 2.0;
      y0 = center - ah / (2 * s);
      x0 = cw / 2.0 - aw / (2 * s);
   }

   dest = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, aw, ah);
   gdk_pixbuf_fill (dest, 0x808080ff);
   render_item (item_at (d, a), &pl[a], dest, x0, y0, s, FALSE);
   render_item (item_at (d, b), &pl[b], dest, x0, y0, s, FALSE);
   if (diff && ov > 0) {
      /* the band where both are: the difference */
      GdkPixbuf *band = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, aw, ah);
      Place inter = pl[a];
      gint bx0, by0, bx1, by1;
      gdk_pixbuf_fill (band, 0xffffffff);
      render_item (item_at (d, a), &pl[a], band, x0, y0, s, FALSE);
      render_item (item_at (d, b), &pl[b], band, x0, y0, s, TRUE);
      if (side) { inter.x = pl[b].x; inter.w = ov; }
      else      { inter.y = pl[b].y; inter.h = ov; }
      bx0 = CLAMP ((gint) floor ((inter.x - x0) * s), 0, aw);
      by0 = CLAMP ((gint) floor ((inter.y - y0) * s), 0, ah);
      bx1 = CLAMP ((gint) ceil ((inter.x + inter.w - x0) * s), 0, aw);
      by1 = CLAMP ((gint) ceil ((inter.y + inter.h - y0) * s), 0, ah);
      if (bx1 > bx0 && by1 > by0)
         gdk_pixbuf_copy_area (band, bx0, by0, bx1 - bx0, by1 - by0, dest, bx0, by0);
      g_object_unref (band);
   }

   G_GNUC_BEGIN_IGNORE_DEPRECATIONS
   gdk_cairo_set_source_pixbuf (cr, dest, 0, 0);
   G_GNUC_END_IGNORE_DEPRECATIONS
   cairo_paint (cr);
   g_object_unref (dest);

   /* where the second begins and the first ends: red triangles at the
      sides (white edged, seen on any picture) */
   {
      gdouble at[2];
      gint k;
      if (side) {
         at[0] = floor ((pl[b].x - x0) * s) + 0.5;
         at[1] = floor ((pl[a].x + pl[a].w - x0) * s) + 0.5;
      } else {
         at[0] = floor ((pl[b].y - y0) * s) + 0.5;
         at[1] = floor ((pl[a].y + pl[a].h - y0) * s) + 0.5;
      }
      for (k = 0; k < (ov > 0 ? 2 : 1); k++) {
         gdouble p = at[k], far = side ? ah : aw;
         if (side) {
            cairo_move_to (cr, p - 7, 0);   cairo_line_to (cr, p, 11);   cairo_line_to (cr, p + 7, 0);
            cairo_close_path (cr);
            cairo_move_to (cr, p - 7, far); cairo_line_to (cr, p, far - 11); cairo_line_to (cr, p + 7, far);
            cairo_close_path (cr);
         } else {
            cairo_move_to (cr, 0, p - 7);   cairo_line_to (cr, 11, p);   cairo_line_to (cr, 0, p + 7);
            cairo_close_path (cr);
            cairo_move_to (cr, far, p - 7); cairo_line_to (cr, far - 11, p); cairo_line_to (cr, far, p + 7);
            cairo_close_path (cr);
         }
      }
      cairo_set_source_rgb (cr, 0.9, 0.1, 0.1);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 1, 1, 1);
      cairo_set_line_width (cr, 1);
      cairo_stroke (cr);
   }
   g_free (pl);
}


/* the controls of the seam chosen */
static void
update_seam (Dialog *d)
{
   guint n = n_items (d);
   gboolean seams = n >= 2 && mode_of (d) != JOIN_GRID;
   Place *pl;
   gint cw, ch, max, a = d->seam, b = d->seam + 1;
   gchar *text;

   gtk_widget_set_visible (d->seam_box, seams);
   gtk_widget_set_visible (d->detail, seams);
   gtk_widget_set_visible (d->legend, seams);
   if (!seams) return;
   pl = layout (d, &cw, &ch);
   max = mode_of (d) == JOIN_HORIZONTAL ? MIN (pl[a].w, pl[b].w) - 1 : MIN (pl[a].h, pl[b].h) - 1;
   g_free (pl);

   d->setting = TRUE;
   gtk_range_set_range (GTK_RANGE (d->overlap_scale), 0, MAX (1, max));
   gtk_spin_button_set_range (GTK_SPIN_BUTTON (d->overlap_spin), 0, MAX (1, max));
   d->overlap[a] = CLAMP (d->overlap[a], 0, MAX (0, max));
   gtk_range_set_value (GTK_RANGE (d->overlap_scale), d->overlap[a]);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->overlap_spin), d->overlap[a]);
   d->setting = FALSE;

   if (d->detecting)
      text = g_strdup (_("(looking for it...)"));
   else if (d->found[a] > 0)
      text = g_strdup_printf (_("(found: %d px)"), d->found[a]);
   else if (d->found[a] == 0)
      text = g_strdup (_("(none found)"));
   else
      text = g_strdup ("");
   gtk_label_set_text (GTK_LABEL (d->auto_label), text);
   g_free (text);
}


static void
update_all (Dialog *d)
{
   Place *pl;
   gint cw = 0, ch = 0;
   gchar *text;

   if (d->closed) return;
   pl = layout (d, &cw, &ch);
   g_free (pl);
   text = n_items (d) ? g_strdup_printf (_("%d × %d px"), cw, ch) : g_strdup ("");
   gtk_label_set_text (GTK_LABEL (d->size_label), text);
   g_free (text);
   gtk_widget_set_visible (d->cols_box, mode_of (d) == JOIN_GRID);
   gtk_widget_set_sensitive (d->auto_check, mode_of (d) != JOIN_GRID);
   update_seam (d);
   gtk_widget_set_sensitive (d->save_button, n_items (d) >= 2 && !d->saving);
   gtk_widget_queue_draw (d->preview);
   gtk_widget_queue_draw (d->detail);
}


/* the list of the images, in their order */
static void
fill_list (Dialog *d)
{
   GtkWidget *child;
   guint i;

   while ((child = gtk_widget_get_first_child (d->list)))
      gtk_list_box_remove (GTK_LIST_BOX (d->list), child);
   for (i = 0; i < n_items (d); i++) {
      Item *it = item_at (d, i);
      gchar *name = g_filename_display_basename (it->path);
      gchar *text = g_strdup_printf ("%u. %s (%d × %d)", i + 1, name, it->w, it->h);
      GtkWidget *label = gtk_label_new (text);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_set_tooltip_text (label, it->path);
      gtk_list_box_append (GTK_LIST_BOX (d->list), label);
      g_free (text);
      g_free (name);
   }
}


static void
fill_seams (Dialog *d)
{
   GtkStringList *names = gtk_string_list_new (NULL);
   guint i;

   for (i = 0; i + 1 < MAX (n_items (d), 1); i++) {
      gchar *text = g_strdup_printf (_("%u and %u"), i + 1, i + 2);
      gtk_string_list_append (names, text);
      g_free (text);
   }
   d->setting = TRUE;
   gtk_drop_down_set_model (GTK_DROP_DOWN (d->seam_drop), G_LIST_MODEL (names));
   g_object_unref (names);
   d->seam = CLAMP (d->seam, 0, MAX (0, (gint) n_items (d) - 2));
   gtk_drop_down_set_selected (GTK_DROP_DOWN (d->seam_drop), d->seam);
   d->setting = FALSE;
}


static void
cb_layout_changed (GObject *object, GParamSpec *pspec, Dialog *d)
{
   if (d->setting) return;
   remember (d);
   if (!d->items) return;
   detect_start (d);
   update_all (d);
}


static void
cb_auto_toggled (GtkCheckButton *check, Dialog *d)
{
   cb_layout_changed (NULL, NULL, d);
}


static void
cb_seam_changed (GObject *drop, GParamSpec *pspec, Dialog *d)
{
   if (d->setting) return;
   d->seam = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->seam_drop));
   update_all (d);
}


static void
set_overlap (Dialog *d, gint value)
{
   if (d->setting || n_items (d) < 2) return;
   d->overlap[d->seam] = value;
   update_all (d);
}


static void
cb_overlap_scale (GtkRange *range, Dialog *d)
{
   set_overlap (d, (gint) gtk_range_get_value (range));
}


static void
cb_overlap_spin (GtkSpinButton *spin, Dialog *d)
{
   set_overlap (d, gtk_spin_button_get_value_as_int (spin));
}


static void
cb_diff_toggled (GtkCheckButton *check, Dialog *d)
{
   gtk_widget_queue_draw (d->detail);
}


/* the image chosen in the list one up (-1) or down (1) */
static void
move_item (Dialog *d, gint dir)
{
   GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (d->list));
   gint i, j;
   gpointer tmp;

   if (!row || !d->items) return;
   i = gtk_list_box_row_get_index (row);
   j = i + dir;
   if (j < 0 || j >= (gint) n_items (d)) return;
   tmp = d->items->pdata[i];
   d->items->pdata[i] = d->items->pdata[j];
   d->items->pdata[j] = tmp;
   fill_list (d);
   gtk_list_box_select_row (GTK_LIST_BOX (d->list),
                            gtk_list_box_get_row_at_index (GTK_LIST_BOX (d->list), j));
   {
      gchar *name = g_filename_display_basename (item_at (d, 0)->path), *dot, *text;
      if ((dot = strrchr (name, '.')) && dot != name) *dot = '\0';
      text = g_strdup_printf (_("Saved next to the first image as %s_join"), name);
      gtk_label_set_text (GTK_LABEL (d->name_label), text);
      g_free (text);
      g_free (name);
   }
   detect_start (d);
   update_all (d);
}


static void
cb_up (GtkButton *button, Dialog *d)
{
   move_item (d, -1);
}


static void
cb_down (GtkButton *button, Dialog *d)
{
   move_item (d, 1);
}


/*** reading ***/

typedef struct {
   GPtrArray *paths;
   GPtrArray *items;
   GString   *errors;
} LoadJob;


static void
load_job_free (gpointer p)
{
   LoadJob *job = p;
   g_ptr_array_unref (job->paths);
   if (job->items) g_ptr_array_unref (job->items);
   if (job->errors) g_string_free (job->errors, TRUE);
   g_free (job);
}


static void item_set_full (Item *it, GdkPixbuf *full);


static void
load_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   LoadJob *job = data;
   guint i;

   job->items = g_ptr_array_new_with_free_func (item_free);
   for (i = 0; i < job->paths->len && !g_cancellable_is_cancelled (cancel); i++) {
      const gchar *path = g_ptr_array_index (job->paths, i);
      GError *error = NULL;
      GdkPixbuf *p = gdk_pixbuf_new_from_file (path, &error);
      Item *it;

      it = g_new0 (Item, 1);
      it->path = g_strdup (path);
      g_ptr_array_add (job->items, it);
      /* not for GdkPixbuf: read later by gimv's loaders (load_done) */
      if (!p) {
         it->error = g_strdup (error ? error->message : "?");
         g_clear_error (&error);
         continue;
      }
      item_set_full (it, gdk_pixbuf_apply_embedded_orientation (p));
      g_object_unref (p);
   }
   g_task_return_boolean (task, TRUE);
}


/* the image of an item: its size and its preview */
static void
item_set_full (Item *it, GdkPixbuf *full)
{
   gdouble s;

   it->full = full;
   it->w = gdk_pixbuf_get_width (it->full);
   it->h = gdk_pixbuf_get_height (it->full);
   s = MIN (1.0, (gdouble) PREVIEW_SIZE / MAX (it->w, it->h));
   it->preview = s < 1.0
      ? gdk_pixbuf_scale_simple (it->full, MAX (1, (gint) (it->w * s)),
                                 MAX (1, (gint) (it->h * s)), GDK_INTERP_BILINEAR)
      : g_object_ref (it->full);
}


/* those GdkPixbuf could not read, by gimv's loaders; the others left out
   (and told) */
static void
load_by_gimv (LoadJob *job)
{
   guint i = 0;

   while (i < job->items->len) {
      Item *it = g_ptr_array_index (job->items, i);
      GimvImage *image;

      if (it->full) {
         i++;
         continue;
      }
      image = gimv_image_load_file (it->path, FALSE);
      if (image && GDK_IS_PIXBUF (image->image)) {
         item_set_full (it, gdk_pixbuf_copy (image->image));
         gimv_image_unref (image);
         i++;
         continue;
      }
      if (image) gimv_image_unref (image);
      {
         gchar *name = g_filename_display_basename (it->path);
         if (!job->errors) job->errors = g_string_new (NULL);
         g_string_append_printf (job->errors, "%s%s: %s", job->errors->len ? "\n" : "",
                                 name, it->error);
         g_free (name);
      }
      g_ptr_array_remove_index (job->items, i);
   }
}


static void
load_done (GObject *source, GAsyncResult *res, gpointer data)
{
   Dialog *d = data;
   LoadJob *job = g_task_get_task_data (G_TASK (res));

   if (!d->closed) {
      load_by_gimv (job);
      d->items = g_ptr_array_ref (job->items);
      d->overlap = g_new0 (gint, MAX (1, n_items (d)));
      d->found = g_new0 (gint, MAX (1, n_items (d)));
      fill_list (d);
      fill_seams (d);
      if (n_items (d) > 0) {
         gchar *name = g_filename_display_basename (item_at (d, 0)->path), *dot, *text;
         if ((dot = strrchr (name, '.')) && dot != name) *dot = '\0';
         text = g_strdup_printf (_("Saved next to the first image as %s_join"), name);
         gtk_label_set_text (GTK_LABEL (d->name_label), text);
         g_free (text);
         g_free (name);
      }
      if (n_items (d) < 2) {
         set_status (d, job->errors ? job->errors->str : _("Two images or more are needed."),
                     TRUE);
      } else {
         set_status (d, job->errors ? job->errors->str : NULL, job->errors != NULL);
         detect_start (d);
      }
      update_all (d);
   }
   dialog_unref (d);
}


/*** saving ***/

typedef struct {
   GPtrArray *items;
   Place     *places;
   gint       cw, ch;
   gint       quality;
   gboolean   metadata;
   gboolean   comment;
   gchar     *src;
   gchar     *dest;
   gchar     *error;
   gchar     *warning;
} SaveJob;


static void
save_job_free (gpointer p)
{
   SaveJob *job = p;
   g_ptr_array_unref (job->items);
   g_free (job->places);
   g_free (job->src);
   g_free (job->dest);
   g_free (job->error);
   g_free (job->warning);
   g_free (job);
}


static void
save_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   SaveJob *job = data;
   gboolean alpha = FALSE;
   GdkPixbuf *canvas;
   guint i;

   for (i = 0; i < job->items->len; i++)
      alpha |= gdk_pixbuf_get_has_alpha (((Item *) g_ptr_array_index (job->items, i))->full);
   canvas = gdk_pixbuf_new (GDK_COLORSPACE_RGB, alpha, 8, job->cw, job->ch);
   if (!canvas) {
      job->error = g_strdup (_("Not enough memory for the image."));
      g_task_return_boolean (task, TRUE);
      return;
   }
   gdk_pixbuf_fill (canvas, alpha ? 0x00000000 : 0xffffffff);

   for (i = 0; i < job->items->len; i++) {
      Item *it = g_ptr_array_index (job->items, i);
      Place *p = &job->places[i];
      GdkPixbuf *src = it->full;
      gint x0 = MAX (0, p->x), y0 = MAX (0, p->y);
      gint x1 = MIN (job->cw, p->x + p->w), y1 = MIN (job->ch, p->y + p->h);

      if (x1 <= x0 || y1 <= y0) continue;
      if (gdk_pixbuf_get_has_alpha (src) != alpha)
         src = gdk_pixbuf_add_alpha (src, FALSE, 0, 0, 0);
      else
         g_object_ref (src);
      if (p->w == it->w && p->h == it->h)
         gdk_pixbuf_copy_area (src, x0 - p->x, y0 - p->y, x1 - x0, y1 - y0, canvas, x0, y0);
      else
         gdk_pixbuf_scale (src, canvas, x0, y0, x1 - x0, y1 - y0, p->x, p->y,
                           (gdouble) p->w / it->w, (gdouble) p->h / it->h,
                           GDK_INTERP_HYPER);
      g_object_unref (src);
   }

   gimv_crop_save_pixbuf (canvas, job->src, "_join", job->quality, job->metadata,
                          &job->dest, &job->error, &job->warning);
   g_object_unref (canvas);
   g_task_return_boolean (task, TRUE);
}


static void
refresh_folder (const gchar *path)
{
   gchar *dir = g_path_get_dirname (path), *dir_slash = g_strconcat (dir, "/", NULL);
   GimvThumbView *tv = gimv_thumb_view_find_opened_dir (dir_slash);

   if (!tv) tv = gimv_thumb_view_find_opened_dir (dir);
   if (tv) gimv_thumb_view_refresh_list (tv);
   g_free (dir_slash);
   g_free (dir);
}


static void
save_done (GObject *source, GAsyncResult *res, gpointer data)
{
   Dialog *d = data;
   SaveJob *job = g_task_get_task_data (G_TASK (res));

   if (!job->error) {
      if (job->comment) gimv_resize_copy_comment (job->src, job->dest);
      refresh_folder (job->dest);
   }
   if (!d->closed) {
      gchar *text, *name;
      d->saving = FALSE;
      gtk_widget_set_sensitive (d->save_button, TRUE);
      if (job->error) {
         text = g_strdup_printf (_("Could not save: %s"), job->error);
         set_status (d, text, TRUE);
      } else {
         name = g_filename_display_basename (job->dest);
         if (job->warning)
            text = g_strdup_printf (_("Saved as %s (the information of the file "
                                      "could not be copied: %s)"), name, job->warning);
         else
            text = g_strdup_printf (_("Saved as %s"), name);
         g_free (name);
         set_status (d, text, job->warning != NULL);
         g_free (d->saved);
         d->saved = g_strdup (job->dest);
         gtk_widget_set_visible (d->show_button, TRUE);
         gtk_button_set_label (GTK_BUTTON (d->close_button), _("_Close"));
      }
      g_free (text);
   }
   dialog_unref (d);
}


static void
remember (Dialog *d)
{
   if (d->closed) return;
   conf.join_mode     = mode_of (d);
   conf.join_cols     = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (d->cols_spin));
   conf.join_fit      = fit_of (d);
   conf.join_auto     = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->auto_check));
   conf.join_quality  = (gint) gtk_range_get_value (GTK_RANGE (d->quality_scale));
   conf.join_metadata = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->metadata_check));
   conf.join_comment  = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->comment_check));
}


static void
cb_save (GtkButton *button, Dialog *d)
{
   SaveJob *job;
   GTask *task;

   if (d->saving || n_items (d) < 2) return;
   remember (d);
   job = g_new0 (SaveJob, 1);
   job->items = g_ptr_array_ref (d->items);
   job->places = layout (d, &job->cw, &job->ch);
   job->quality = conf.join_quality;
   job->metadata = conf.join_metadata;
   job->comment = conf.join_comment;
   job->src = g_strdup (item_at (d, 0)->path);

   d->saving = TRUE;
   gtk_widget_set_sensitive (d->save_button, FALSE);
   set_status (d, _("Saving..."), FALSE);
   task = g_task_new (NULL, NULL, save_done, dialog_ref (d));
   g_task_set_task_data (task, job, save_job_free);
   g_task_run_in_thread (task, save_thread);
   g_object_unref (task);
}


static void
cb_show (GtkButton *button, Dialog *d)
{
   GimvThumbWin *tw = NULL;
   GList *list;

   if (!d->saved) return;
   if (d->parent && GIMV_IS_THUMB_WIN (d->parent)) tw = GIMV_THUMB_WIN (d->parent);
   if (!tw && (list = gimv_thumb_win_get_list ())) tw = list->data;
   if (tw) gimv_thumb_view_open_folder_of (tw, d->saved);
   gtk_window_destroy (GTK_WINDOW (d->window));
}


static void
cb_close (GtkButton *button, Dialog *d)
{
   gtk_window_destroy (GTK_WINDOW (d->window));
}


static void
cb_destroy (GtkWidget *window, Dialog *d)
{
   /* the settings were kept as they were changed: the widgets are gone */
   d->closed = TRUE;
   if (d->detect_cancel) g_cancellable_cancel (d->detect_cancel);
   dialog_unref (d);
}


static GtkWidget *
label_new (const gchar *text)
{
   GtkWidget *label = gtk_label_new_with_mnemonic (text);
   gtk_label_set_xalign (GTK_LABEL (label), 1.0);
   return label;
}


void
gimv_join_dialog_open (GList *paths, GtkWindow *parent)
{
   Dialog *d;
   GtkWidget *window, *vbox, *hbox, *left, *right, *grid, *label, *button, *scroll, *box;
   const gchar *modes[] = { N_("One below another"), N_("Side by side"), N_("In a grid"), NULL };
   const gchar *fits[] = { N_("To the smallest"), N_("To the largest"), N_("Not changed"), NULL };
   const gchar *shown[4];
   GList *node;
   LoadJob *job;
   GTask *task;
   gint row = 0;
   guint i;

   d = g_new0 (Dialog, 1);
   d->ref = 1;
   d->parent = parent;
   d->seam = 0;
   d->paths = g_ptr_array_new_with_free_func (g_free);
   for (node = paths; node; node = g_list_next (node))
      if (node->data) g_ptr_array_add (d->paths, g_strdup (node->data));
   if (d->paths->len < 2) {
      dialog_unref (d);
      return;
   }

   window = d->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (window), _("Join Images"));
   if (parent) gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   gtk_window_set_default_size (GTK_WINDOW (window), 980, 680);
   g_signal_connect (window, "destroy", G_CALLBACK (cb_destroy), d);
   {
      GtkEventController *keys = gtk_shortcut_controller_new ();
      gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
         gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                           gtk_named_action_new ("window.close")));
      gtk_widget_add_controller (window, keys);
   }

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (window), vbox);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
   gtk_widget_set_vexpand (hbox, TRUE);
   gtk_box_append (GTK_BOX (vbox), hbox);

   /* the left: the images and the settings */
   left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_size_request (left, 320, -1);
   gtk_box_append (GTK_BOX (hbox), left);

   label = gtk_label_new_with_mnemonic (_("_Images (in this order):"));
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (left), label);
   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
   gtk_widget_set_size_request (scroll, -1, 150);
   gtk_widget_set_hexpand (scroll, TRUE);
   gtk_widget_add_css_class (scroll, "frame");
   d->list = gtk_list_box_new ();
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->list);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), d->list);
   gtk_box_append (GTK_BOX (box), scroll);
   {
      GtkWidget *bbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      button = gtk_button_new_from_icon_name ("go-up-symbolic");
      gtk_widget_set_tooltip_text (button, _("Up"));
      g_signal_connect (button, "clicked", G_CALLBACK (cb_up), d);
      gtk_box_append (GTK_BOX (bbox), button);
      button = gtk_button_new_from_icon_name ("go-down-symbolic");
      gtk_widget_set_tooltip_text (button, _("Down"));
      g_signal_connect (button, "clicked", G_CALLBACK (cb_down), d);
      gtk_box_append (GTK_BOX (bbox), button);
      gtk_box_append (GTK_BOX (box), bbox);
   }
   gtk_box_append (GTK_BOX (left), box);

   grid = gtk_grid_new ();
   gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
   gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
   gtk_box_append (GTK_BOX (left), grid);

   label = label_new (_("_Arrange:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   for (i = 0; modes[i]; i++) shown[i] = _(modes[i]);
   shown[i] = NULL;
   d->mode_drop = gtk_drop_down_new_from_strings (shown);
   gtk_drop_down_set_selected (GTK_DROP_DOWN (d->mode_drop), CLAMP (conf.join_mode, 0, 2));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->mode_drop);
   gtk_grid_attach (GTK_GRID (grid), d->mode_drop, 1, row++, 1, 1);

   d->cols_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   label = gtk_label_new_with_mnemonic (_("Colu_mns:"));
   gtk_box_append (GTK_BOX (d->cols_box), label);
   d->cols_spin = gtk_spin_button_new_with_range (1, 50, 1);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->cols_spin), MAX (1, conf.join_cols));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->cols_spin);
   gtk_box_append (GTK_BOX (d->cols_box), d->cols_spin);
   gtk_grid_attach (GTK_GRID (grid), d->cols_box, 1, row++, 1, 1);

   label = label_new (_("Si_ze:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   for (i = 0; fits[i]; i++) shown[i] = _(fits[i]);
   shown[i] = NULL;
   d->fit_drop = gtk_drop_down_new_from_strings (shown);
   gtk_drop_down_set_selected (GTK_DROP_DOWN (d->fit_drop), CLAMP (conf.join_fit, 0, 2));
   gtk_widget_set_tooltip_text (d->fit_drop,
      _("The width made the same (the height side by side)"));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->fit_drop);
   gtk_grid_attach (GTK_GRID (grid), d->fit_drop, 1, row++, 1, 1);

   d->auto_check = gtk_check_button_new_with_mnemonic (_("_Look for overlaps (pieces of one photo)"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (d->auto_check), conf.join_auto);
   gtk_grid_attach (GTK_GRID (grid), d->auto_check, 0, row++, 2, 1);

   label = label_new (_("_Quality:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   d->quality_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
   gtk_scale_set_draw_value (GTK_SCALE (d->quality_scale), TRUE);
   gtk_scale_set_value_pos (GTK_SCALE (d->quality_scale), GTK_POS_RIGHT);
   gtk_scale_set_digits (GTK_SCALE (d->quality_scale), 0);
   gtk_range_set_value (GTK_RANGE (d->quality_scale),
                        conf.join_quality > 0 ? conf.join_quality : 90);
   gtk_widget_set_hexpand (d->quality_scale, TRUE);
   gtk_widget_set_tooltip_text (d->quality_scale, _("Of JPEG and WebP"));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->quality_scale);
   gtk_grid_attach (GTK_GRID (grid), d->quality_scale, 1, row++, 1, 1);

   d->metadata_check = gtk_check_button_new_with_mnemonic
      (_("_Keep the information of the first file (EXIF: the date, the camera, ...)"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (d->metadata_check), conf.join_metadata);
   gtk_grid_attach (GTK_GRID (grid), d->metadata_check, 0, row++, 2, 1);
#ifndef ENABLE_GEXIV2
   gtk_widget_set_visible (d->metadata_check, FALSE);
#endif
   d->comment_check = gtk_check_button_new_with_mnemonic
      (_("Copy the comment and the _tags given by hand of the first one"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (d->comment_check), conf.join_comment);
   gtk_grid_attach (GTK_GRID (grid), d->comment_check, 0, row++, 2, 1);

   d->name_label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (d->name_label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (d->name_label), PANGO_ELLIPSIZE_MIDDLE);
   gtk_widget_add_css_class (d->name_label, "dim-label");
   gtk_box_append (GTK_BOX (left), d->name_label);

   /* the right: the new image, the seam */
   right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_hexpand (right, TRUE);
   gtk_box_append (GTK_BOX (hbox), right);

   d->preview = gtk_drawing_area_new ();
   gtk_widget_set_vexpand (d->preview, TRUE);
   gtk_widget_set_size_request (d->preview, 300, 200);
   gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (d->preview), draw_preview, d, NULL);
   gtk_box_append (GTK_BOX (right), d->preview);
   d->size_label = gtk_label_new (NULL);
   gtk_box_append (GTK_BOX (right), d->size_label);

   d->seam_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   label = gtk_label_new_with_mnemonic (_("S_eam:"));
   gtk_box_append (GTK_BOX (box), label);
   d->seam_drop = gtk_drop_down_new (NULL, NULL);
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->seam_drop);
   gtk_box_append (GTK_BOX (box), d->seam_drop);
   d->auto_label = gtk_label_new (NULL);
   gtk_widget_add_css_class (d->auto_label, "dim-label");
   gtk_widget_set_hexpand (d->auto_label, TRUE);
   gtk_label_set_xalign (GTK_LABEL (d->auto_label), 0.0);
   gtk_box_append (GTK_BOX (box), d->auto_label);
   d->diff_check = gtk_check_button_new_with_mnemonic (_("As the _difference"));
   gtk_widget_set_tooltip_text (d->diff_check,
      _("The overlap shown as the difference of the two images: dark where they are "
        "the same"));
   gtk_box_append (GTK_BOX (box), d->diff_check);
   gtk_box_append (GTK_BOX (d->seam_box), box);

   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   label = gtk_label_new_with_mnemonic (_("_Overlap:"));
   gtk_box_append (GTK_BOX (box), label);
   d->overlap_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
   gtk_scale_set_draw_value (GTK_SCALE (d->overlap_scale), FALSE);
   gtk_widget_set_hexpand (d->overlap_scale, TRUE);
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->overlap_scale);
   gtk_box_append (GTK_BOX (box), d->overlap_scale);
   d->overlap_spin = gtk_spin_button_new_with_range (0, 1, 1);
   gtk_box_append (GTK_BOX (box), d->overlap_spin);
   gtk_box_append (GTK_BOX (box), gtk_label_new ("px"));
   gtk_box_append (GTK_BOX (d->seam_box), box);
   gtk_box_append (GTK_BOX (right), d->seam_box);

   d->detail = gtk_drawing_area_new ();
   gtk_widget_set_size_request (d->detail, 300, 200);
   gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (d->detail), draw_detail, d, NULL);
   gtk_box_append (GTK_BOX (right), d->detail);
   d->legend = gtk_label_new (_("Between the red triangles the two images overlap; the second "
                                "one begins at the first ones.  They match when the picture "
                                "goes on without a step there (with \"As the difference\": "
                                "when the band between the triangles is black)."));
   gtk_label_set_wrap (GTK_LABEL (d->legend), TRUE);
   gtk_label_set_xalign (GTK_LABEL (d->legend), 0.0);
   gtk_widget_add_css_class (d->legend, "dim-label");
   gtk_box_append (GTK_BOX (right), d->legend);

   /* the status, the buttons */
   d->status = gtk_label_new (_("Reading the images..."));
   gtk_label_set_xalign (GTK_LABEL (d->status), 0.0);
   gtk_label_set_wrap (GTK_LABEL (d->status), TRUE);
   gtk_label_set_max_width_chars (GTK_LABEL (d->status), 80);
   gtk_label_set_selectable (GTK_LABEL (d->status), TRUE);
   gtk_box_append (GTK_BOX (vbox), d->status);

   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_set_halign (box, GTK_ALIGN_END);
   d->show_button = gtk_button_new_with_mnemonic (_("S_how the Saved Image"));
   gtk_widget_set_visible (d->show_button, FALSE);
   g_signal_connect (d->show_button, "clicked", G_CALLBACK (cb_show), d);
   gtk_box_append (GTK_BOX (box), d->show_button);
   d->close_button = gtk_button_new_with_mnemonic (_("_Cancel"));
   g_signal_connect (d->close_button, "clicked", G_CALLBACK (cb_close), d);
   gtk_box_append (GTK_BOX (box), d->close_button);
   d->save_button = gtk_button_new_with_mnemonic (_("_Save"));
   gtk_widget_add_css_class (d->save_button, "suggested-action");
   gtk_widget_set_sensitive (d->save_button, FALSE);
   g_signal_connect (d->save_button, "clicked", G_CALLBACK (cb_save), d);
   gtk_box_append (GTK_BOX (box), d->save_button);
   gtk_box_append (GTK_BOX (vbox), box);
   gtk_window_set_default_widget (GTK_WINDOW (window), d->save_button);

   g_signal_connect (d->mode_drop, "notify::selected", G_CALLBACK (cb_layout_changed), d);
   g_signal_connect (d->fit_drop, "notify::selected", G_CALLBACK (cb_layout_changed), d);
   g_signal_connect (d->cols_spin, "notify::value", G_CALLBACK (cb_layout_changed), d);
   g_signal_connect (d->auto_check, "toggled", G_CALLBACK (cb_auto_toggled), d);
   g_signal_connect (d->seam_drop, "notify::selected", G_CALLBACK (cb_seam_changed), d);
   g_signal_connect (d->overlap_scale, "value-changed", G_CALLBACK (cb_overlap_scale), d);
   g_signal_connect (d->overlap_spin, "value-changed", G_CALLBACK (cb_overlap_spin), d);
   g_signal_connect (d->diff_check, "toggled", G_CALLBACK (cb_diff_toggled), d);
   g_signal_connect_swapped (d->quality_scale, "value-changed", G_CALLBACK (remember), d);
   g_signal_connect_swapped (d->metadata_check, "toggled", G_CALLBACK (remember), d);
   g_signal_connect_swapped (d->comment_check, "toggled", G_CALLBACK (remember), d);

   gtk_widget_set_visible (d->cols_box, conf.join_mode == JOIN_GRID);
   gtk_widget_set_visible (d->seam_box, FALSE);
   gtk_widget_set_visible (d->detail, FALSE);
   gtk_widget_set_visible (d->legend, FALSE);
   gtk_window_present (GTK_WINDOW (window));

   job = g_new0 (LoadJob, 1);
   job->paths = g_ptr_array_ref (d->paths);
   task = g_task_new (NULL, NULL, load_done, dialog_ref (d));
   g_task_set_task_data (task, job, load_job_free);
   g_task_run_in_thread (task, load_thread);
   g_object_unref (task);
}
