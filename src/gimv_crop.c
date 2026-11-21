/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: cropping the image shown, saved as a new file.
 *
 * A rectangle on the image (the whole image at first): dragged inside it is
 * moved, dragged at a side or a corner it is resized, dragged outside it a
 * new one is drawn.  A bar over the image: "Detect the Edges" (the border
 * of the color of the edges of the image taken off: a small object scanned
 * on a large white sheet), the settings of it, the ratio, the size, Save
 * (Enter) and Stop (Esc).
 *
 * The image is loaded in its full size, turned as it is shown, in a thread;
 * the edges are found and the file is written in threads too.  It is saved
 * next to the image as "name_crop.ext" (never over another file:
 * name_crop-1.ext, ...), in its format when gdk-pixbuf writes it (else
 * JPEG, or PNG with alpha); the information of the file (EXIF, ...) and the
 * comment can be kept.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_crop.h"
#include "gimv_file_comment.h"

#include <math.h>
#include <string.h>

#include "gimageview.h"
#include "gimv_image.h"
#include "gimv_image_info.h"
#include "gimv_resize_dialog.h"
#include "gimv_save_dialog.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"
#include "prefs.h"


#define HANDLE_RANGE 8.0     /* [px of the screen] around a side */

enum {
   SIDE_L = 1 << 0,
   SIDE_R = 1 << 1,
   SIDE_T = 1 << 2,
   SIDE_B = 1 << 3,
   GRAB_MOVE = 1 << 4,
   GRAB_NEW  = 1 << 5,
};

static const struct { const gchar *label; gint a, b; } ratios[] = {
   { N_("Free"),      0,  0 },
   { N_("Original"), -1, -1 },
   { "1:1",           1,  1 },
   { "4:3",           4,  3 },
   { "3:2",           3,  2 },
   { "16:9",         16,  9 },
};


typedef struct Crop_Tag {
   gint            ref;
   gboolean        active;
   GimvImageView  *iv;          /* not a reference */
   gchar          *path;

   GtkWidget      *bar;         /* in a scrolled window: never wider than the view */
   GtkWidget      *bar_outer;
   GtkWidget      *detect_button;
   GtkWidget      *aspect_drop;
   GtkWidget      *size_label;
   GtkWidget      *message;
   GtkWidget      *save_button;
   GtkWidget      *tol_spin;
   GtkWidget      *margin_spin;
   GtkWidget      *quality_scale;
   GtkWidget      *metadata_check;
   GtkWidget      *comment_check;

   GtkEventController *drag;
   GtkEventController *motion;
   GtkEventController *keys;

   GdkPixbuf      *pixbuf;      /* the image shown, full size, turned */
   gint            W, H;
   GCancellable   *cancel;
   gboolean        busy;        /* finding the edges or saving */

   gdouble         x, y, w, h;  /* [px] of the image as it is shown */

   gint            grab;
   gdouble         sx, sy, sw, sh;        /* the rectangle when it began */
   gdouble         start_x, start_y;      /* where it began (the image) */
} Crop;


static Crop *
crop_ref (Crop *c)
{
   c->ref++;
   return c;
}


static void
crop_unref (Crop *c)
{
   if (--c->ref > 0) return;
   g_clear_object (&c->pixbuf);
   g_clear_object (&c->cancel);
   g_free (c->path);
   g_free (c);
}


/* the view is gone or the image changed: nothing of the view is touched */
static void
crop_data_free (gpointer data)
{
   Crop *c = data;
   c->active = FALSE;
   c->iv = NULL;
   if (c->cancel) g_cancellable_cancel (c->cancel);
   crop_unref (c);
}


static Crop *
crop_get (GimvImageView *iv)
{
   return g_object_get_data (G_OBJECT (iv), "gimv-crop");
}


gboolean
gimv_crop_is_active (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);
   return crop_get (iv) != NULL;
}


static void
set_message (Crop *c, const gchar *text)
{
   if (!c->message) return;
   gtk_label_set_text (GTK_LABEL (c->message), text ? text : "");
   gtk_widget_set_visible (c->message, text && *text);
}


/*** the rectangle ***/

/* w / h of the ratio chosen, 0: free */
static gdouble
ratio_of (Crop *c)
{
   guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (c->aspect_drop));
   gdouble r;

   if (i >= G_N_ELEMENTS (ratios) || ratios[i].a == 0) return 0;
   if (ratios[i].a < 0)
      return c->W > 0 && c->H > 0 ? (gdouble) c->W / c->H : 0;
   r = (gdouble) ratios[i].a / ratios[i].b;
   /* the way the rectangle stands now */
   if (c->h > c->w) r = 1 / r;
   return r;
}


static void
update (Crop *c)
{
   gchar *text;
   gint w = (gint) floor (c->w + 0.5), h = (gint) floor (c->h + 0.5);

   if (!c->iv) return;
   if (c->W > 0 && c->H > 0)
      gimv_image_view_set_crop_mark (c->iv, TRUE, c->x / c->W, c->y / c->H,
                                     c->w / c->W, c->h / c->H);
   text = c->W > 0 ? g_strdup_printf (_("%d × %d px"), w, h) : g_strdup ("");
   gtk_label_set_text (GTK_LABEL (c->size_label), text);
   g_free (text);
   gtk_widget_set_sensitive (c->save_button, c->pixbuf && !c->busy && w >= 1 && h >= 1);
   gtk_widget_set_sensitive (c->detect_button, c->pixbuf && !c->busy);
}


/* the rectangle in the image */
static void
clamp_rect (Crop *c)
{
   c->w = CLAMP (c->w, 1, c->W);
   c->h = CLAMP (c->h, 1, c->H);
   c->x = CLAMP (c->x, 0, c->W - c->w);
   c->y = CLAMP (c->y, 0, c->H - c->h);
}


/* the rectangle made of the ratio chosen, around its middle */
static void
apply_ratio (Crop *c)
{
   gdouble r = ratio_of (c), cx, cy;

   if (r <= 0 || c->W <= 0) return;
   cx = c->x + c->w / 2;
   cy = c->y + c->h / 2;
   if (c->w / c->h > r) c->w = c->h * r; else c->h = c->w / r;
   if (c->w > c->W) { c->w = c->W; c->h = c->w / r; }
   if (c->h > c->H) { c->h = c->H; c->w = c->h * r; }
   c->x = cx - c->w / 2;
   c->y = cy - c->h / 2;
   clamp_rect (c);
}


/* a corner (or the start of a new one) at ax, ay, the other one toward
   px, py, in the ratio r (0: free) */
static void
rect_from_anchor (Crop *c, gdouble ax, gdouble ay, gdouble px, gdouble py, gdouble r)
{
   gdouble w = fabs (px - ax), h = fabs (py - ay);
   gboolean right = px >= ax, down = py >= ay;
   gdouble room_w = right ? c->W - ax : ax, room_h = down ? c->H - ay : ay;

   if (r > 0) {
      if (h <= 0 || w / h > r) h = w / r; else w = h * r;
      if (w > room_w) { w = room_w; h = w / r; }
      if (h > room_h) { h = room_h; w = h * r; }
   } else {
      w = MIN (w, room_w);
      h = MIN (h, room_h);
   }
   c->x = right ? ax : ax - w;
   c->y = down ? ay : ay - h;
   c->w = w;
   c->h = h;
}


/* a side dragged to p: the other side stays; in a ratio the other way is
   kept around its middle */
static void
rect_from_side (Crop *c, gint side, gdouble px, gdouble py, gdouble r)
{
   gdouble l = c->sx, t = c->sy, rr = c->sx + c->sw, b = c->sy + c->sh;

   if (side & SIDE_L) l = MIN (px, rr - 1);
   if (side & SIDE_R) rr = MAX (px, l + 1);
   if (side & SIDE_T) t = MIN (py, b - 1);
   if (side & SIDE_B) b = MAX (py, t + 1);

   if (r > 0 && (side & (SIDE_L | SIDE_R))) {
      gdouble w = rr - l, h = w / r, cy = c->sy + c->sh / 2;
      if (h > c->H) { h = c->H; w = h * r; if (side & SIDE_L) l = rr - w; else rr = l + w; }
      t = cy - h / 2;
      b = t + h;
      if (t < 0) { t = 0; b = h; }
      if (b > c->H) { b = c->H; t = b - h; }
   } else if (r > 0) {
      gdouble h = b - t, w = h * r, cx = c->sx + c->sw / 2;
      if (w > c->W) { w = c->W; h = w / r; if (side & SIDE_T) t = b - h; else b = t + h; }
      l = cx - w / 2;
      rr = l + w;
      if (l < 0) { l = 0; rr = w; }
      if (rr > c->W) { rr = c->W; l = rr - w; }
   }
   c->x = l;
   c->y = t;
   c->w = rr - l;
   c->h = b - t;
}


/* a point of the drawing area -> [px] of the image */
static gboolean
to_image (Crop *c, gdouble wx, gdouble wy, gdouble *ix, gdouble *iy)
{
   gdouble fx, fy;

   if (!c->iv || c->W <= 0 || !gimv_image_view_point_to_shown (c->iv, wx, wy, &fx, &fy))
      return FALSE;
   *ix = fx * c->W;
   *iy = fy * c->H;
   return TRUE;
}


/* what a press at wx, wy takes: a side, a corner, the rectangle, a new
   one (on the image), or nothing */
static gint
hit (Crop *c, gdouble wx, gdouble wy)
{
   gdouble x0, y0, x1, y1, ix, iy;
   gint side = 0;

   if (!c->iv || c->W <= 0
       || !gimv_image_view_shown_to_point (c->iv, c->x / c->W, c->y / c->H, &x0, &y0)
       || !gimv_image_view_shown_to_point (c->iv, (c->x + c->w) / c->W,
                                           (c->y + c->h) / c->H, &x1, &y1))
      return 0;

   if (wy >= y0 - HANDLE_RANGE && wy <= y1 + HANDLE_RANGE) {
      if (fabs (wx - x0) <= HANDLE_RANGE) side |= SIDE_L;
      else if (fabs (wx - x1) <= HANDLE_RANGE) side |= SIDE_R;
   }
   if (wx >= x0 - HANDLE_RANGE && wx <= x1 + HANDLE_RANGE) {
      if (fabs (wy - y0) <= HANDLE_RANGE) side |= SIDE_T;
      else if (fabs (wy - y1) <= HANDLE_RANGE) side |= SIDE_B;
   }
   if (side) return side;
   /* inside it it is moved; the whole image (at first) has nowhere to
      go: a new one is drawn */
   if (wx > x0 && wx < x1 && wy > y0 && wy < y1
       && (c->w < c->W - 0.5 || c->h < c->H - 0.5))
      return GRAB_MOVE;
   if (to_image (c, wx, wy, &ix, &iy) && ix >= 0 && iy >= 0 && ix <= c->W && iy <= c->H)
      return GRAB_NEW;
   return 0;
}


static const gchar *
cursor_of (gint grab)
{
   switch (grab) {
   case SIDE_L | SIDE_T: return "nw-resize";
   case SIDE_R | SIDE_T: return "ne-resize";
   case SIDE_L | SIDE_B: return "sw-resize";
   case SIDE_R | SIDE_B: return "se-resize";
   case SIDE_L:          return "w-resize";
   case SIDE_R:          return "e-resize";
   case SIDE_T:          return "n-resize";
   case SIDE_B:          return "s-resize";
   case GRAB_MOVE:       return "move";
   case GRAB_NEW:        return "crosshair";
   default:              return NULL;
   }
}


static void
cb_motion (GtkEventControllerMotion *ctrl, gdouble x, gdouble y, Crop *c)
{
   if (!c->iv || !c->iv->draw_area || c->grab) return;
   gtk_widget_set_cursor_from_name (c->iv->draw_area, cursor_of (hit (c, x, y)));
}


static void
cb_drag_begin (GtkGestureDrag *drag, gdouble x, gdouble y, Crop *c)
{
   gint grab = c->busy ? 0 : hit (c, x, y);

   if (!grab || !to_image (c, x, y, &c->start_x, &c->start_y)) {
      /* not on the image: the view's own (scrolling) */
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
      return;
   }
   gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
   /* Enter and Esc to the view (the press is not the view's own) */
   if (c->iv && c->iv->draw_area) gtk_widget_grab_focus (c->iv->draw_area);
   c->grab = grab;
   c->sx = c->x;
   c->sy = c->y;
   c->sw = c->w;
   c->sh = c->h;
}


static void
cb_drag_update (GtkGestureDrag *drag, gdouble dx, gdouble dy, Crop *c)
{
   gdouble x0, y0, px, py, r;

   if (!c->grab || !gtk_gesture_drag_get_start_point (drag, &x0, &y0)
       || !to_image (c, x0 + dx, y0 + dy, &px, &py))
      return;
   px = CLAMP (px, 0, c->W);
   py = CLAMP (py, 0, c->H);
   r = ratio_of (c);

   if (c->grab == GRAB_MOVE) {
      c->x = c->sx + (px - c->start_x);
      c->y = c->sy + (py - c->start_y);
      c->w = c->sw;
      c->h = c->sh;
   } else if (c->grab == GRAB_NEW) {
      gdouble sx = CLAMP (c->start_x, 0, c->W), sy = CLAMP (c->start_y, 0, c->H);
      /* the ratio stands the way it is drawn */
      if (r > 0 && r != 1) {
         guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (c->aspect_drop));
         if (ratios[i].a > 0) {
            r = (gdouble) ratios[i].a / ratios[i].b;
            if (fabs (py - sy) > fabs (px - sx)) r = 1 / r;
         }
      }
      rect_from_anchor (c, sx, sy, px, py, r);
   } else if ((c->grab & (SIDE_L | SIDE_R)) && (c->grab & (SIDE_T | SIDE_B))) {
      gdouble ax = (c->grab & SIDE_L) ? c->sx + c->sw : c->sx;
      gdouble ay = (c->grab & SIDE_T) ? c->sy + c->sh : c->sy;
      rect_from_anchor (c, ax, ay, px, py, r);
   } else {
      rect_from_side (c, c->grab, px, py, r);
   }
   clamp_rect (c);
   update (c);
}


static void
cb_drag_end (GtkGestureDrag *drag, gdouble dx, gdouble dy, Crop *c)
{
   if (!c->grab) return;
   cb_drag_update (drag, dx, dy, c);
   /* a click, not a drag: the rectangle as it was */
   if (c->grab == GRAB_NEW && (c->w < 2 || c->h < 2)) {
      c->x = c->sx;
      c->y = c->sy;
      c->w = c->sw;
      c->h = c->sh;
      update (c);
   }
   c->grab = 0;
}


static void
cb_ratio_changed (GObject *drop, GParamSpec *pspec, Crop *c)
{
   conf.crop_aspect = gtk_drop_down_get_selected (GTK_DROP_DOWN (c->aspect_drop));
   apply_ratio (c);
   update (c);
   if (c->iv && c->iv->draw_area) gtk_widget_grab_focus (c->iv->draw_area);
}


/*** the image in its full size, turned as it is shown ***/

typedef struct {
   gchar     *path;
   gboolean   flip;
   gint       rotate;
   GdkPixbuf *pixbuf;
} LoadJob;


static void
load_job_free (gpointer p)
{
   LoadJob *job = p;
   g_free (job->path);
   g_clear_object (&job->pixbuf);
   g_free (job);
}


static GdkPixbuf *
turn (GdkPixbuf *pixbuf, gboolean flip, gint rotate)
{
   GdkPixbuf *tmp;

   if (flip) {
      tmp = gdk_pixbuf_flip (pixbuf, TRUE);
      g_object_unref (pixbuf);
      pixbuf = tmp;
   }
   switch (rotate) {
   case GIMV_IMAGE_VIEW_ROTATE_90:
      tmp = gdk_pixbuf_rotate_simple (pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE); break;
   case GIMV_IMAGE_VIEW_ROTATE_180:
      tmp = gdk_pixbuf_rotate_simple (pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);       break;
   case GIMV_IMAGE_VIEW_ROTATE_270:
      tmp = gdk_pixbuf_rotate_simple (pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);        break;
   default:
      return pixbuf;
   }
   g_object_unref (pixbuf);
   return tmp;
}


static void
load_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   LoadJob *job = data;
   GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file (job->path, NULL);

   if (pixbuf && !g_cancellable_is_cancelled (cancel))
      job->pixbuf = turn (pixbuf, job->flip, job->rotate);
   else if (pixbuf)
      g_object_unref (pixbuf);
   g_task_return_boolean (task, TRUE);
}


/* an image gdk-pixbuf does not read: the one of the view (the main
   thread) */
static GdkPixbuf *
pixbuf_of_view (GimvImageView *iv)
{
   GimvImage *image = gimv_image_view_get_full_image (iv);
   GdkPixbuf *pixbuf = NULL, *tmp;

   if (!image) return NULL;
   if (gimv_image_depth (image) == 8 && gimv_image_get_pixels (image)) {
      tmp = gdk_pixbuf_new_from_data (gimv_image_get_pixels (image), GDK_COLORSPACE_RGB,
                                      gimv_image_has_alpha (image), 8,
                                      gimv_image_width (image), gimv_image_height (image),
                                      gimv_image_rowstride (image), NULL, NULL);
      pixbuf = gdk_pixbuf_copy (tmp);
      g_object_unref (tmp);
   }
   gimv_image_unref (image);
   return pixbuf;
}


static void
load_done (GObject *source, GAsyncResult *res, gpointer data)
{
   Crop *c = data;
   LoadJob *job = g_task_get_task_data (G_TASK (res));

   if (c->active && c->iv) {
      c->pixbuf = job->pixbuf ? g_object_ref (job->pixbuf) : pixbuf_of_view (c->iv);
      if (!c->pixbuf) {
         set_message (c, _("The image could not be read."));
      } else {
         gint W = gdk_pixbuf_get_width (c->pixbuf), H = gdk_pixbuf_get_height (c->pixbuf);
         if (c->W > 0 && (c->W != W || c->H != H)) {
            /* the size thought was not right: the same part */
            gdouble sx = (gdouble) W / c->W, sy = (gdouble) H / c->H;
            c->x *= sx; c->w *= sx; c->y *= sy; c->h *= sy;
         } else if (c->W <= 0) {
            c->x = c->y = 0;
            c->w = W;
            c->h = H;
         }
         c->W = W;
         c->H = H;
         clamp_rect (c);
         apply_ratio (c);
         set_message (c, NULL);
      }
      update (c);
   }
   crop_unref (c);
}


/*** the edges ***/

typedef struct {
   GdkPixbuf *pixbuf;
   gint       tolerance, margin;
   gboolean   found;
   gint       x, y, w, h;
} DetectJob;


static void
detect_job_free (gpointer p)
{
   DetectJob *job = p;
   g_object_unref (job->pixbuf);
   g_free (job);
}


/* the range of what is there in count[0..n): the parts in the middle
   (the outer 3 % left out: the shadows of the edges of a scanner), each
   long enough, then grown out over what goes on from them */
static gboolean
range_of (const gint *count, gint n, gint noise, gint *from, gint *to)
{
   gint lo = n * 3 / 100, hi = n - n * 3 / 100, min_len = MAX (2, n / 200);
   gint i, start = -1, a = -1, b = -1;

   for (i = lo; i <= hi; i++) {
      gboolean on = i < hi && count[i] > noise;
      if (on && start < 0) start = i;
      if (!on && start >= 0) {
         if (i - start >= min_len) {
            if (a < 0) a = start;
            b = i;
         }
         start = -1;
      }
   }
   if (a < 0) return FALSE;
   while (a > 0 && count[a - 1] > noise) a--;
   while (b < n && count[b] > noise) b++;
   *from = a;
   *to = b;
   return TRUE;
}


static void
detect_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   DetectJob *job = data;
   gint W = gdk_pixbuf_get_width (job->pixbuf), H = gdk_pixbuf_get_height (job->pixbuf);
   gint nc = gdk_pixbuf_get_n_channels (job->pixbuf);
   gint rs = gdk_pixbuf_get_rowstride (job->pixbuf);
   const guchar *pixels = gdk_pixbuf_read_pixels (job->pixbuf);
   guint *hist = g_new0 (guint, 4 * 256);
   guint total = 0;
   gint bg[4] = { 0, 0, 0, 255 };
   gint band = MAX (1, MIN (W, H) / 50);
   guchar *mask;
   gint *cols, *rows;
   gint x, y, k, iter, x0 = 0, x1 = W, y0 = 0, y1 = H;

   /* the color of the border: the median of the band around the edges */
   for (y = 0; y < H; y++) {
      const guchar *p = pixels + (gsize) y * rs;
      gboolean edge_row = y < band || y >= H - band;
      for (x = 0; x < W; x++) {
         if (!edge_row && x == band) x = W - band;
         for (k = 0; k < nc; k++) hist[k * 256 + p[x * nc + k]]++;
         total++;
      }
   }
   for (k = 0; k < nc; k++) {
      guint sum = 0;
      gint v;
      for (v = 0; v < 256; v++) {
         sum += hist[k * 256 + v];
         if (sum * 2 >= total) break;
      }
      bg[k] = v;
   }
   g_free (hist);

   /* what is not of that color */
   mask = g_malloc ((gsize) W * H);
   for (y = 0; y < H; y++) {
      const guchar *p = pixels + (gsize) y * rs;
      guchar *m = mask + (gsize) y * W;
      for (x = 0; x < W; x++, p += nc) {
         gint d = 0;
         for (k = 0; k < nc; k++) d = MAX (d, ABS (p[k] - bg[k]));
         m[x] = d > job->tolerance;
      }
      if (g_cancellable_is_cancelled (cancel)) goto out;
   }

   /* the columns with something in them over the rows found, and the rows
      over the columns found, twice */
   cols = g_new (gint, W);
   rows = g_new (gint, H);
   job->found = TRUE;
   for (iter = 0; iter < 2 && job->found; iter++) {
      memset (cols, 0, sizeof (gint) * W);
      for (y = y0; y < y1; y++) {
         const guchar *m = mask + (gsize) y * W;
         for (x = 0; x < W; x++) cols[x] += m[x];
      }
      if (!range_of (cols, W, MAX (2, (y1 - y0) / 250), &x0, &x1)) {
         job->found = FALSE;
         break;
      }
      memset (rows, 0, sizeof (gint) * H);
      for (y = 0; y < H; y++) {
         const guchar *m = mask + (gsize) y * W;
         for (x = x0; x < x1; x++) rows[y] += m[x];
      }
      if (!range_of (rows, H, MAX (2, (x1 - x0) / 250), &y0, &y1))
         job->found = FALSE;
   }
   g_free (cols);
   g_free (rows);

   if (job->found) {
      job->x = MAX (0, x0 - job->margin);
      job->y = MAX (0, y0 - job->margin);
      job->w = MIN (W, x1 + job->margin) - job->x;
      job->h = MIN (H, y1 + job->margin) - job->y;
   }

out:
   g_free (mask);
   g_task_return_boolean (task, TRUE);
}


static void
remember (Crop *c)
{
   if (!c->tol_spin) return;   /* the bar is gone */
   conf.crop_tolerance = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (c->tol_spin));
   conf.crop_margin    = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (c->margin_spin));
   conf.crop_quality   = (gint) gtk_range_get_value (GTK_RANGE (c->quality_scale));
   conf.crop_metadata  = gtk_check_button_get_active (GTK_CHECK_BUTTON (c->metadata_check));
   conf.crop_comment   = gtk_check_button_get_active (GTK_CHECK_BUTTON (c->comment_check));
}


static void
detect_done (GObject *source, GAsyncResult *res, gpointer data)
{
   Crop *c = data;
   DetectJob *job = g_task_get_task_data (G_TASK (res));

   if (c->active && c->iv) {
      c->busy = FALSE;
      if (job->found) {
         c->x = job->x;
         c->y = job->y;
         c->w = job->w;
         c->h = job->h;
         /* the ratio would cut what was found off: free */
         gtk_drop_down_set_selected (GTK_DROP_DOWN (c->aspect_drop), 0);
         if (job->x == 0 && job->y == 0 && job->w == c->W && job->h == c->H)
            set_message (c, _("There is no border to take off."));
         else
            set_message (c, NULL);
      } else {
         set_message (c, _("No edges were found."));
      }
      update (c);
   }
   crop_unref (c);
}


static void
cb_detect (GtkButton *button, Crop *c)
{
   DetectJob *job;
   GTask *task;

   if (!c->pixbuf || c->busy) return;
   remember (c);
   job = g_new0 (DetectJob, 1);
   job->pixbuf = g_object_ref (c->pixbuf);
   job->tolerance = conf.crop_tolerance;
   job->margin = conf.crop_margin;
   c->busy = TRUE;
   set_message (c, _("Finding the edges..."));
   update (c);

   task = g_task_new (NULL, c->cancel, detect_done, crop_ref (c));
   g_task_set_task_data (task, job, detect_job_free);
   g_task_run_in_thread (task, detect_thread);
   g_object_unref (task);
}


/*** saving ***/

typedef struct {
   GdkPixbuf *pixbuf;     /* the part */
   gchar     *src;
   gchar     *dest;
   gint       quality;
   gboolean   metadata;
   gboolean   comment;
   gchar     *error;
   gchar     *warning;
} SaveJob;


static void
save_job_free (gpointer p)
{
   SaveJob *job = p;
   g_object_unref (job->pixbuf);
   g_free (job->src);
   g_free (job->dest);
   g_free (job->error);
   g_free (job->warning);
   g_free (job);
}


static gboolean
writable (const gchar *name)
{
   GSList *list = gdk_pixbuf_get_formats (), *node;
   gboolean ret = FALSE;

   for (node = list; node && !ret; node = g_slist_next (node)) {
      gchar *n = gdk_pixbuf_format_get_name (node->data);
      ret = !strcmp (n, name) && gdk_pixbuf_format_is_writable (node->data);
      g_free (n);
   }
   g_slist_free (list);
   return ret;
}


/* the format of the image if gdk-pixbuf writes it, else JPEG (PNG with
   alpha); ext: the extension of the new name, NULL: the one it has */
static const gchar *
format_for (const gchar *src, gboolean alpha, const gchar **ext)
{
   static const gchar *keep[] = { "jpeg", "png", "webp", "tiff" };
   GdkPixbufFormat *format = gdk_pixbuf_get_file_info (src, NULL, NULL);
   gchar *name = format ? gdk_pixbuf_format_get_name (format) : NULL;
   const gchar *ret = NULL;
   guint i;

   *ext = NULL;
   for (i = 0; name && i < G_N_ELEMENTS (keep) && !ret; i++)
      if (!strcmp (name, keep[i]) && writable (name)) ret = keep[i];
   g_free (name);
   /* a format of gimv's own loaders (MAG, Pi, ...: pictures of a few
      colours, drawn by dots): PNG, not JPEG */
   if (!ret && !format) {
      ret = "png";
      *ext = "png";
   }
   if (!ret) {
      ret = alpha ? "png" : "jpeg";
      *ext = alpha ? "png" : "jpg";
   }
   return ret;
}


/* name<suffix>.ext next to the image, a name not taken */
static gchar *
dest_for (const gchar *src, const gchar *suffix, const gchar *new_ext)
{
   gchar *dir = g_path_get_dirname (src), *base = g_path_get_basename (src);
   gchar *dot = strrchr (base, '.'), *stem, *path;
   const gchar *ext;
   gint i;

   stem = dot && dot != base ? g_strndup (base, dot - base) : g_strdup (base);
   ext = new_ext ? new_ext : (dot && dot != base ? dot + 1 : "jpg");
   path = g_strdup_printf ("%s/%s%s.%s", dir, stem, suffix, ext);
   for (i = 1; g_file_test (path, G_FILE_TEST_EXISTS); i++) {
      g_free (path);
      path = g_strdup_printf ("%s/%s%s-%d.%s", dir, stem, suffix, i, ext);
   }
   g_free (stem);
   g_free (base);
   g_free (dir);
   return path;
}


/* the comments written in the files (MAG, Pi, PNG, ...) as one text:
   "name: text" (a comment: the text alone); of several files, those not
   the same, each under the name of its file; NULL: none */
static gchar *
comment_of_files (const gchar * const *sources)
{
   GPtrArray *texts = g_ptr_array_new_with_free_func (g_free);
   GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
   GString *all;
   guint i, j;

   for (i = 0; sources && sources[i]; i++) {
      GPtrArray *list = gimv_file_comment_read (sources[i]);
      GString *one = g_string_new (NULL);
      gboolean seen = FALSE;

      for (j = 0; list && j < list->len; j++) {
         GimvFileComment *c = g_ptr_array_index (list, j);
         gchar *t;
         if (!c->text || !*c->text) continue;
         t = g_strchomp (g_strdup (c->text));
         if (one->len) g_string_append_c (one, '\n');
         if (!strcmp (c->name, "Comment") || !strcmp (c->name, _("Comment")))
            g_string_append (one, t);
         else
            g_string_append_printf (one, "%s: %s", c->name, t);
         g_free (t);
      }
      if (list) g_ptr_array_unref (list);
      for (j = 0; j < texts->len; j++)
         if (!strcmp (g_ptr_array_index (texts, j), one->str)) seen = TRUE;
      if (one->len && !seen) {
         g_ptr_array_add (texts, g_string_free (one, FALSE));
         g_ptr_array_add (names, g_filename_display_basename (sources[i]));
      } else {
         g_string_free (one, TRUE);
      }
   }

   all = g_string_new (NULL);
   for (i = 0; i < texts->len; i++) {
      if (texts->len > 1)
         g_string_append_printf (all, "%s[%s]\n", i ? "\n" : "",
                                 (gchar *) g_ptr_array_index (names, i));
      g_string_append (all, g_ptr_array_index (texts, i));
   }
   g_ptr_array_unref (texts);
   g_ptr_array_unref (names);
   if (!all->len) {
      g_string_free (all, TRUE);
      return NULL;
   }
   return g_string_free (all, FALSE);
}


static guint32
png_crc (const guchar *p, gsize len)
{
   static guint32 table[256];
   guint32 c = 0xffffffff;
   gsize i;

   if (!table[1]) {
      for (i = 0; i < 256; i++) {
         guint32 v = i;
         gint k;
         for (k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320 ^ (v >> 1) : v >> 1;
         table[i] = v;
      }
   }
   for (i = 0; i < len; i++) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
   return c ^ 0xffffffff;
}


/* the PNG in *buf with a "Comment" after its IHDR: tEXt (Latin-1) when
   the text can be, else iTXt (UTF-8).  Written here, not by GdkPixbuf:
   some versions of it store a text not in Latin-1 broken (tEXt of the
   wrong bytes). */
static void
png_add_comment (gchar **buf, gsize *len, const gchar *text)
{
   static const guchar sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
   const gsize at = 8 + 4 + 4 + 13 + 4;        /* after IHDR */
   gchar *latin1 = g_convert (text, -1, "ISO-8859-1", "UTF-8", NULL, NULL, NULL);
   GByteArray *chunk = g_byte_array_new (), *out;
   guchar n[4];
   guint32 crc, size;

   if (*len < at || memcmp (*buf, sig, 8) || memcmp (*buf + 12, "IHDR", 4)) {
      g_free (latin1);
      g_byte_array_unref (chunk);
      return;
   }
   if (latin1) {
      g_byte_array_append (chunk, (const guint8 *) "tEXtComment", 12);
      g_byte_array_append (chunk, (const guint8 *) latin1, strlen (latin1));
   } else {
      /* keyword, 0, no compression, method 0, no language, no
         translated keyword */
      g_byte_array_append (chunk, (const guint8 *) "iTXtComment\0\0\0\0", 16);
      g_byte_array_append (chunk, (const guint8 *) text, strlen (text));
   }
   g_free (latin1);
   size = chunk->len - 4;
   crc = png_crc (chunk->data, chunk->len);

   out = g_byte_array_sized_new (*len + chunk->len + 8);
   g_byte_array_append (out, (const guint8 *) *buf, at);
   n[0] = size >> 24; n[1] = size >> 16; n[2] = size >> 8; n[3] = size;
   g_byte_array_append (out, n, 4);
   g_byte_array_append (out, chunk->data, chunk->len);
   n[0] = crc >> 24; n[1] = crc >> 16; n[2] = crc >> 8; n[3] = crc;
   g_byte_array_append (out, n, 4);
   g_byte_array_append (out, (const guint8 *) *buf + at, *len - at);
   g_byte_array_unref (chunk);

   g_free (*buf);
   *len = out->len;
   *buf = (gchar *) g_byte_array_free (out, FALSE);
}


/* pixbuf saved next to the image src as name<suffix>.ext (any thread);
   the comments written in the files sources (NULL-terminated) go into a
   PNG ("Comment");
   FALSE: not saved (*error); *warning: saved, the information not copied */
gboolean
gimv_crop_save_pixbuf (GdkPixbuf *pixbuf, const gchar *src, const gchar *suffix,
                       gint quality, gboolean metadata, const gchar * const *sources,
                       gchar **dest, gchar **error_text, gchar **warning)
{
   const gchar *ext, *writer;
   gchar *buf = NULL;
   gsize len = 0;
   GError *error = NULL;
   gboolean ok;

   writer = format_for (src, gdk_pixbuf_get_has_alpha (pixbuf), &ext);
   *dest = dest_for (src, suffix, ext);

   if (!strcmp (writer, "jpeg")
       && (gdk_pixbuf_get_width (pixbuf) > 65500 || gdk_pixbuf_get_height (pixbuf) > 65500))
   {
      *error_text = g_strdup (_("JPEG can not be larger than 65500 px."));
      return FALSE;
   }
   if (!strcmp (writer, "jpeg") || !strcmp (writer, "webp")) {
      gchar q[8];
      GdkPixbuf *p = pixbuf;
      g_snprintf (q, sizeof q, "%d", quality);
      if (!strcmp (writer, "jpeg") && gdk_pixbuf_get_has_alpha (p))
         p = gdk_pixbuf_composite_color_simple (p, gdk_pixbuf_get_width (p),
                                                gdk_pixbuf_get_height (p),
                                                GDK_INTERP_NEAREST, 255, 8,
                                                0xffffff, 0xffffff);
      else
         g_object_ref (p);
      ok = gdk_pixbuf_save_to_buffer (p, &buf, &len, writer, &error, "quality", q, NULL);
      g_object_unref (p);
   } else if (!strcmp (writer, "png")) {
      gchar *comment = comment_of_files (sources);
      ok = gdk_pixbuf_save_to_buffer (pixbuf, &buf, &len, writer, &error, NULL);
      if (ok && comment) png_add_comment (&buf, &len, comment);
      g_free (comment);
   } else {
      ok = gdk_pixbuf_save_to_buffer (pixbuf, &buf, &len, writer, &error, NULL);
   }
   if (ok) ok = g_file_set_contents (*dest, buf, len, &error);
   g_free (buf);
   if (!ok) {
      *error_text = g_strdup (error ? error->message : "?");
      g_clear_error (&error);
      return FALSE;
   }

#ifdef ENABLE_GEXIV2
   /* not of the formats of gimv's own loaders (MAG, Pi, ...): they have no
      EXIF, and Exiv2 can not open them (told as "not copied") */
   if (metadata && strcmp (writer, "webp")
       && gdk_pixbuf_get_file_info (src, NULL, NULL))
   {
      gchar *e = NULL;
      if (!gimv_save_copy_metadata (src, *dest, gdk_pixbuf_get_width (pixbuf),
                                    gdk_pixbuf_get_height (pixbuf), FALSE, &e))
         *warning = e;
   }
#endif
   return TRUE;
}


static void
save_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   SaveJob *job = data;
   const gchar *sources[2] = { job->src, NULL };

   gimv_crop_save_pixbuf (job->pixbuf, job->src, "_crop", job->quality, job->metadata,
                          sources, &job->dest, &job->error, &job->warning);
   g_task_return_boolean (task, TRUE);
}


/* a note over the image for a while: saved, with a button to the file in
   the thumbnail window */
typedef struct {
   GimvImageView *iv;    /* weak */
   GtkWidget     *box;
   gchar         *path;
   guint          id;
} Toast;


static void
toast_free (Toast *t)
{
   if (t->id) g_source_remove (t->id);
   if (t->iv) g_object_remove_weak_pointer (G_OBJECT (t->iv), (gpointer *) &t->iv);
   if (t->iv && t->box) gimv_image_view_remove_overlay (t->iv, t->box);
   g_clear_object (&t->box);
   g_free (t->path);
   g_free (t);
}


static gboolean
toast_timeout (gpointer data)
{
   Toast *t = data;
   t->id = 0;
   toast_free (t);
   return G_SOURCE_REMOVE;
}


static void
cb_toast_show (GtkButton *button, Toast *t)
{
   GimvThumbWin *tw = NULL;
   GtkRoot *root;
   GList *list;

   if (t->iv) {
      root = gtk_widget_get_root (GTK_WIDGET (t->iv));
      if (root && GIMV_IS_THUMB_WIN (root)) tw = GIMV_THUMB_WIN (root);
   }
   if (!tw && (list = gimv_thumb_win_get_list ())) tw = list->data;
   if (tw) {
      gimv_thumb_view_open_folder_of (tw, t->path);
      gtk_window_present (GTK_WINDOW (tw));
   }
   toast_free (t);
}


static void
show_toast (GimvImageView *iv, const gchar *text, const gchar *path)
{
   Toast *t = g_new0 (Toast, 1);
   GtkWidget *label, *button;

   t->iv = iv;
   g_object_add_weak_pointer (G_OBJECT (iv), (gpointer *) &t->iv);
   t->path = g_strdup (path);
   t->box = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
   gtk_widget_add_css_class (t->box, "osd");
   gtk_widget_add_css_class (t->box, "toolbar");
   gtk_widget_set_halign (t->box, GTK_ALIGN_CENTER);
   gtk_widget_set_valign (t->box, GTK_ALIGN_END);
   gtk_widget_set_margin_bottom (t->box, 12);
   label = gtk_label_new (text);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
   gtk_label_set_max_width_chars (GTK_LABEL (label), 60);
   gtk_box_append (GTK_BOX (t->box), label);
   if (path) {
      button = gtk_button_new_with_mnemonic (_("_Show"));
      gtk_widget_set_tooltip_text (button, _("Choose it in the thumbnail window"));
      g_signal_connect (button, "clicked", G_CALLBACK (cb_toast_show), t);
      gtk_box_append (GTK_BOX (t->box), button);
   }
   gimv_image_view_add_overlay (iv, t->box);
   t->id = g_timeout_add_seconds (8, toast_timeout, t);
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
   Crop *c = data;
   SaveJob *job = g_task_get_task_data (G_TASK (res));

   if (!job->error) {
      if (job->comment) gimv_resize_copy_comment (job->src, job->dest);
      refresh_folder (job->dest);
   }

   if (c->active && c->iv) {
      c->busy = FALSE;
      if (job->error) {
         gchar *text = g_strdup_printf (_("Could not save: %s"), job->error);
         set_message (c, text);
         g_free (text);
         update (c);
      } else {
         GimvImageView *iv = c->iv;
         gchar *name = g_filename_display_basename (job->dest), *text;
         if (job->warning)
            text = g_strdup_printf (_("Saved as %s (the information of the file "
                                      "could not be copied: %s)"), name, job->warning);
         else
            text = g_strdup_printf (_("Saved as %s"), name);
         gimv_crop_stop (iv);
         show_toast (iv, text, job->dest);
         g_free (text);
         g_free (name);
      }
   }
   crop_unref (c);
}


static void
cb_save (GtkButton *button, Crop *c)
{
   SaveJob *job;
   GTask *task;
   gint x, y, w, h;

   if (!c->pixbuf || c->busy) return;
   remember (c);
   x = (gint) floor (c->x + 0.5);
   y = (gint) floor (c->y + 0.5);
   w = MIN ((gint) floor (c->w + 0.5), c->W - x);
   h = MIN ((gint) floor (c->h + 0.5), c->H - y);
   if (w < 1 || h < 1) return;

   job = g_new0 (SaveJob, 1);
   job->pixbuf = gdk_pixbuf_new_subpixbuf (c->pixbuf, x, y, w, h);
   job->src = g_strdup (c->path);
   job->quality = conf.crop_quality;
   job->metadata = conf.crop_metadata;
   job->comment = conf.crop_comment;
   c->busy = TRUE;
   set_message (c, _("Saving..."));
   update (c);

   task = g_task_new (NULL, NULL, save_done, crop_ref (c));
   g_task_set_task_data (task, job, save_job_free);
   g_task_run_in_thread (task, save_thread);
   g_object_unref (task);
}


/*** keys, the bar ***/

static gboolean
cb_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
        Crop *c)
{
   if (keyval == GDK_KEY_Escape) {
      if (c->iv) gimv_crop_stop (c->iv);
      return TRUE;
   }
   if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
      /* a button or the list of the bar takes its own Enter */
      GtkRoot *root = c->iv ? gtk_widget_get_root (GTK_WIDGET (c->iv)) : NULL;
      GtkWidget *focus = root ? gtk_root_get_focus (root) : NULL;
      if (focus && c->bar && gtk_widget_is_ancestor (focus, c->bar)) return FALSE;
      cb_save (NULL, c);
      return TRUE;
   }
   return FALSE;
}


static void
cb_stop (GtkButton *button, Crop *c)
{
   if (c->iv) gimv_crop_stop (c->iv);
}


static GtkWidget *
settings_popover (Crop *c)
{
   GtkWidget *popover, *grid, *label;
   gint row = 0;

   grid = gtk_grid_new ();
   gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
   gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
   gtk_widget_set_margin_top (grid, 6);
   gtk_widget_set_margin_bottom (grid, 6);
   gtk_widget_set_margin_start (grid, 6);
   gtk_widget_set_margin_end (grid, 6);

   label = gtk_label_new_with_mnemonic (_("_Color tolerance:"));
   gtk_label_set_xalign (GTK_LABEL (label), 1.0);
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   c->tol_spin = gtk_spin_button_new_with_range (1, 200, 1);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (c->tol_spin),
                              conf.crop_tolerance > 0 ? conf.crop_tolerance : 40);
   gtk_widget_set_tooltip_text (c->tol_spin,
      _("How far (0 - 255) a color may be from the one of the edges and still be "
        "the border: larger for a noisy scan, smaller for a pale object"));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), c->tol_spin);
   gtk_grid_attach (GTK_GRID (grid), c->tol_spin, 1, row++, 1, 1);

   label = gtk_label_new_with_mnemonic (_("_Margin (px):"));
   gtk_label_set_xalign (GTK_LABEL (label), 1.0);
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   c->margin_spin = gtk_spin_button_new_with_range (0, 500, 1);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (c->margin_spin), MAX (0, conf.crop_margin));
   gtk_widget_set_tooltip_text (c->margin_spin, _("Left around what is found"));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), c->margin_spin);
   gtk_grid_attach (GTK_GRID (grid), c->margin_spin, 1, row++, 1, 1);

   label = gtk_label_new_with_mnemonic (_("_Quality:"));
   gtk_label_set_xalign (GTK_LABEL (label), 1.0);
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   c->quality_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
   gtk_scale_set_draw_value (GTK_SCALE (c->quality_scale), TRUE);
   gtk_scale_set_value_pos (GTK_SCALE (c->quality_scale), GTK_POS_RIGHT);
   gtk_scale_set_digits (GTK_SCALE (c->quality_scale), 0);
   gtk_range_set_value (GTK_RANGE (c->quality_scale),
                        conf.crop_quality > 0 ? conf.crop_quality : 90);
   gtk_widget_set_size_request (c->quality_scale, 160, -1);
   gtk_widget_set_tooltip_text (c->quality_scale, _("Of JPEG and WebP"));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), c->quality_scale);
   gtk_grid_attach (GTK_GRID (grid), c->quality_scale, 1, row++, 1, 1);

   c->metadata_check = gtk_check_button_new_with_mnemonic
      (_("_Keep the information of the file (EXIF: the date, the camera, ...)"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (c->metadata_check), conf.crop_metadata);
   gtk_grid_attach (GTK_GRID (grid), c->metadata_check, 0, row++, 2, 1);
#ifndef ENABLE_GEXIV2
   gtk_widget_set_visible (c->metadata_check, FALSE);
#endif
   c->comment_check = gtk_check_button_new_with_mnemonic
      (_("Copy the comment and the _tags given by hand"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (c->comment_check), conf.crop_comment);
   gtk_grid_attach (GTK_GRID (grid), c->comment_check, 0, row++, 2, 1);

   popover = gtk_popover_new ();
   gtk_popover_set_child (GTK_POPOVER (popover), grid);
   g_signal_connect_swapped (popover, "closed", G_CALLBACK (remember), c);
   return popover;
}


static void
build_bar (Crop *c)
{
   GtkWidget *bar, *button, *label;
   const gchar *shown[G_N_ELEMENTS (ratios) + 1];
   gchar *tip;
   guint i;

   bar = c->bar = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
   gtk_widget_add_css_class (bar, "toolbar");

   c->detect_button = gtk_button_new_with_mnemonic (_("_Detect the Edges"));
   gtk_widget_set_tooltip_text (c->detect_button,
      _("Choose what is inside the border of the color of the edges (a small "
        "thing scanned on a white sheet)"));
   g_signal_connect (c->detect_button, "clicked", G_CALLBACK (cb_detect), c);
   gtk_box_append (GTK_BOX (bar), c->detect_button);

   button = gtk_menu_button_new ();
   gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button), "open-menu-symbolic");
   gtk_widget_set_tooltip_text (button, _("Settings"));
   gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), settings_popover (c));
   gtk_box_append (GTK_BOX (bar), button);

   label = gtk_label_new_with_mnemonic (_("_Ratio:"));
   gtk_widget_set_margin_start (label, 6);
   gtk_box_append (GTK_BOX (bar), label);
   for (i = 0; i < G_N_ELEMENTS (ratios); i++) shown[i] = _(ratios[i].label);
   shown[i] = NULL;
   c->aspect_drop = gtk_drop_down_new_from_strings (shown);
   gtk_drop_down_set_selected (GTK_DROP_DOWN (c->aspect_drop),
                               CLAMP (conf.crop_aspect, 0, (gint) G_N_ELEMENTS (ratios) - 1));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), c->aspect_drop);
   g_signal_connect (c->aspect_drop, "notify::selected", G_CALLBACK (cb_ratio_changed), c);
   gtk_box_append (GTK_BOX (bar), c->aspect_drop);

   c->size_label = gtk_label_new (NULL);
   gtk_label_set_width_chars (GTK_LABEL (c->size_label), 14);
   gtk_box_append (GTK_BOX (bar), c->size_label);

   c->message = gtk_label_new (NULL);
   gtk_label_set_ellipsize (GTK_LABEL (c->message), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars (GTK_LABEL (c->message), 40);
   gtk_widget_set_visible (c->message, FALSE);
   gtk_box_append (GTK_BOX (bar), c->message);

   c->save_button = gtk_button_new_with_mnemonic (_("_Save"));
   gtk_widget_add_css_class (c->save_button, "suggested-action");
   {
      gchar *base = g_path_get_basename (c->path), *dot = strrchr (base, '.');
      if (dot && dot != base) *dot = '\0';
      tip = g_strdup_printf (_("Save the part as %s_crop next to the image (Enter)"), base);
      g_free (base);
   }
   gtk_widget_set_tooltip_text (c->save_button, tip);
   g_free (tip);
   g_signal_connect (c->save_button, "clicked", G_CALLBACK (cb_save), c);
   gtk_box_append (GTK_BOX (bar), c->save_button);

   button = gtk_button_new_with_mnemonic (_("S_top"));
   gtk_widget_set_tooltip_text (button, _("Stop cropping (Esc)"));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_stop), c);
   gtk_box_append (GTK_BOX (bar), button);

   /* a click on the bar leaves the focus on the image: Enter saves */
   for (button = gtk_widget_get_first_child (bar); button;
        button = gtk_widget_get_next_sibling (button))
      if (GTK_IS_BUTTON (button) || GTK_IS_MENU_BUTTON (button))
         gtk_widget_set_focus_on_click (button, FALSE);

   c->bar_outer = g_object_ref_sink (gtk_scrolled_window_new ());
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (c->bar_outer),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
   gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (c->bar_outer), TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (c->bar_outer), bar);
}


void
gimv_crop_start (GimvImageView *iv)
{
   Crop *c;
   const gchar *path;
   LoadJob *job;
   GTask *task;
   gint w = 0, h = 0;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   if (crop_get (iv)) return;
   if (!iv->info || !iv->draw_area
       || gimv_image_info_is_movie (iv->info) || gimv_image_info_is_audio (iv->info)
       || gimv_image_info_is_in_archive (iv->info)
       || !(path = gimv_image_info_get_path (iv->info)))
   {
      show_toast (iv, _("This can not be cropped (only an image file)."), NULL);
      return;
   }

   c = g_new0 (Crop, 1);
   c->ref = 1;
   c->active = TRUE;
   c->iv = iv;
   c->path = g_strdup (path);
   c->cancel = g_cancellable_new ();
   g_object_set_data_full (G_OBJECT (iv), "gimv-crop", c, crop_data_free);

   /* the size as it is shown, till the image is read */
   gimv_image_info_get_image_size (iv->info, &w, &h);
   if (w > 0 && h > 0) {
      gint rot = gimv_image_view_get_orientation (iv);
      if (rot == GIMV_IMAGE_VIEW_ROTATE_90 || rot == GIMV_IMAGE_VIEW_ROTATE_270) {
         gint t = w; w = h; h = t;
      }
      c->W = w;
      c->H = h;
      c->w = w;
      c->h = h;
   }

   build_bar (c);
   gimv_image_view_add_bar (iv, c->bar_outer);

   c->drag = GTK_EVENT_CONTROLLER (gtk_gesture_drag_new ());
   gtk_event_controller_set_propagation_phase (c->drag, GTK_PHASE_CAPTURE);
   gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (c->drag), GDK_BUTTON_PRIMARY);
   g_signal_connect (c->drag, "drag-begin", G_CALLBACK (cb_drag_begin), c);
   g_signal_connect (c->drag, "drag-update", G_CALLBACK (cb_drag_update), c);
   g_signal_connect (c->drag, "drag-end", G_CALLBACK (cb_drag_end), c);
   gtk_widget_add_controller (iv->draw_area, c->drag);

   c->motion = gtk_event_controller_motion_new ();
   g_signal_connect (c->motion, "motion", G_CALLBACK (cb_motion), c);
   gtk_widget_add_controller (iv->draw_area, c->motion);

   c->keys = gtk_event_controller_key_new ();
   gtk_event_controller_set_propagation_phase (c->keys, GTK_PHASE_CAPTURE);
   g_signal_connect (c->keys, "key-pressed", G_CALLBACK (cb_key), c);
   gtk_widget_add_controller (GTK_WIDGET (iv), c->keys);

   apply_ratio (c);
   set_message (c, _("Reading the image..."));
   update (c);
   gtk_widget_grab_focus (iv->draw_area);

   job = g_new0 (LoadJob, 1);
   job->path = g_strdup (path);
   job->flip = gimv_image_view_get_flip (iv);
   job->rotate = gimv_image_view_get_orientation (iv);
   task = g_task_new (NULL, c->cancel, load_done, crop_ref (c));
   g_task_set_task_data (task, job, load_job_free);
   g_task_run_in_thread (task, load_thread);
   g_object_unref (task);
}


void
gimv_crop_stop (GimvImageView *iv)
{
   Crop *c;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   c = crop_get (iv);
   if (!c) return;

   remember (c);
   if (iv->draw_area) {
      if (c->drag) gtk_widget_remove_controller (iv->draw_area, c->drag);
      if (c->motion) gtk_widget_remove_controller (iv->draw_area, c->motion);
      gtk_widget_set_cursor (iv->draw_area, NULL);
   }
   if (c->keys) gtk_widget_remove_controller (GTK_WIDGET (iv), c->keys);
   c->drag = c->motion = c->keys = NULL;
   if (c->bar_outer) {
      gimv_image_view_remove_bar (iv, c->bar_outer);
      g_clear_object (&c->bar_outer);
      g_clear_object (&c->bar);
   }
   c->message = c->size_label = c->save_button = c->detect_button = NULL;
   c->aspect_drop = c->tol_spin = c->margin_spin = c->quality_scale = NULL;
   c->metadata_check = c->comment_check = NULL;
   gimv_image_view_set_crop_mark (iv, FALSE, 0, 0, 0, 0);

   /* crop_data_free (): inactive, the threads let go */
   g_object_set_data (G_OBJECT (iv), "gimv-crop", NULL);
}
