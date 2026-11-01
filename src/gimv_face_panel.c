/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the faces of the image shown, in a narrow panel
 * next to an image view (the preview, an image window).
 *
 * Each face found in the image is a small image with its name (or the
 * person it looks like, or "no name").  The pointer on one marks the face
 * on the image; a click gives a menu: name it, the guess is right, not
 * this person (asked again first), ignore.  The panel follows the image
 * shown; it is turned on and off with the menu of the image view
 * ("Edit Faces") or its key.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>
#include <gio/gio.h>

#include "gimv_face_panel.h"
#include "gimv_image_info.h"
#include "gimv_image_view.h"
#include "gimv_index.h"
#include "gimv_index_faces.h"
#include "gimv_index_faces_win.h"
#include "gimv_thumb_win.h"

#define PANEL_KEY  "gimv-face-panel"
#define FACE_SIZE  96

typedef struct {
   gint64   id;
   gdouble  x, y, w, h;        /* in [0, 1] of the image as shown upright */
   gint64   person, guess;
   gchar   *name;              /* of person, or of guess */
   gchar   *tags;              /* of person: "a, b" */
   gboolean ignored;
} PanelFace;

typedef struct {
   GimvImageView *iv;
   GtkWidget     *box;         /* the panel */
   GtkWidget     *list;
   GtkWidget     *message;
   GtkWidget     *popover;
   GPtrArray     *faces;       /* PanelFace */
   PanelFace     *menu_face;   /* the face of the menu */
   guint          watch_id;
   gboolean       busy;

   /* a face marked by hand: dragged on the image */
   GtkWidget     *add_button;
   GtkGesture    *drag;        /* on the drawing area while marking */
   GtkEventController *keys;
   gdouble        drag_x, drag_y;      /* where the drag began (drawing area) */
   gdouble        mark_x, mark_y, mark_w, mark_h;   /* [0, 1] upright */
   gboolean       adding;      /* gimv-faces works on one */
} FacePanel;


static void panel_reload (FacePanel *fp);


static void
panel_face_free (PanelFace *f)
{
   g_free (f->name);
   g_free (f->tags);
   g_free (f);
}


static gchar *
face_label (PanelFace *f)
{
   if (f->ignored)
      return g_strdup (_("(ignored)"));
   if (f->person)
      return g_strdup (f->name ? f->name : "");
   if (f->guess && f->name)
      return g_strdup_printf (_("%s?"), f->name);
   return g_strdup (_("(no name)"));
}


/******************************************************************************
 *  reading the faces of the image
 ******************************************************************************/

/* the size of the image as its EXIF orientation shows it (as gimv-faces
 * read it), for its pixels as [0, 1] */
static gboolean
upright_size (GimvImageView *iv, gint *w, gint *h)
{
   gint o = gimv_image_view_get_exif_orientation (iv), t;

   if (!iv->info) return FALSE;
   gimv_image_info_get_image_size (iv->info, w, h);
   if (*w <= 0 || *h <= 0) return FALSE;
   if (o >= 5) {
      t = *w; *w = *h; *h = t;
   }
   return TRUE;
}


/* -1: not looked at for faces (not in the folders, or not yet) */
static gint
read_faces (FacePanel *fp)
{
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;
   const gchar *path;
   gchar *dir, *name;
   gint w, h, scanned = -1;

   g_ptr_array_set_size (fp->faces, 0);
   if (!db || !fp->iv->info) return -1;
   path = gimv_image_info_get_path (fp->iv->info);
   if (!path || path[0] != '/') return -1;

   dir = g_path_get_dirname (path);
   name = g_path_get_basename (path);

   if (sqlite3_prepare_v2 (db, "SELECT s.n FROM face_scans s JOIN files f ON f.id = s.file_id "
                               "JOIN dirs d ON d.id = f.dir_id WHERE d.path = ?1 AND f.name = ?2",
                           -1, &st, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text (st, 1, dir, -1, SQLITE_STATIC);
      sqlite3_bind_text (st, 2, name, -1, SQLITE_STATIC);
      if (sqlite3_step (st) == SQLITE_ROW) scanned = sqlite3_column_int (st, 0);
      sqlite3_finalize (st);
   }

   /* the faces marked by hand too (an image where none was found) */
   if (upright_size (fp->iv, &w, &h)
       && sqlite3_prepare_v2 (db,
             "SELECT fa.id, fa.x, fa.y, fa.w, fa.h, fa.person_id, fa.guess, "
             "  coalesce(p.name, g.name), fa.ignored, "
             "  (SELECT group_concat(tag, ', ') FROM person_tags WHERE person_id = fa.person_id) "
             "FROM faces fa JOIN files f ON f.id = fa.file_id JOIN dirs d ON d.id = f.dir_id "
             "LEFT JOIN persons p ON p.id = fa.person_id LEFT JOIN persons g ON g.id = fa.guess "
             "WHERE d.path = ?1 AND f.name = ?2 ORDER BY fa.x",
             -1, &st, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text (st, 1, dir, -1, SQLITE_STATIC);
      sqlite3_bind_text (st, 2, name, -1, SQLITE_STATIC);
      while (sqlite3_step (st) == SQLITE_ROW) {
         PanelFace *f = g_new0 (PanelFace, 1);
         f->id = sqlite3_column_int64 (st, 0);
         f->x = sqlite3_column_double (st, 1) / w;
         f->y = sqlite3_column_double (st, 2) / h;
         f->w = sqlite3_column_double (st, 3) / w;
         f->h = sqlite3_column_double (st, 4) / h;
         f->person = sqlite3_column_int64 (st, 5);
         f->guess = sqlite3_column_int64 (st, 6);
         f->name = g_strdup ((const gchar *) sqlite3_column_text (st, 7));
         f->ignored = sqlite3_column_int (st, 8) != 0;
         f->tags = g_strdup ((const gchar *) sqlite3_column_text (st, 9));
         g_ptr_array_add (fp->faces, f);
      }
      sqlite3_finalize (st);
   }
   g_free (dir);
   g_free (name);
   return scanned;
}


/******************************************************************************
 *  what to do with a face (the menu)
 ******************************************************************************/

static GtkWindow *
panel_window (FacePanel *fp)
{
   GtkRoot *root = gtk_widget_get_root (fp->box);
   return GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL;
}


static void
cb_edited (GObject *source, GAsyncResult *res, gpointer data)
{
   FacePanel *fp = data;
   GError *error = NULL;

   gimv_index_faces_edit_finish (res, NULL, &error);
   if (error) {
      g_warning ("faces: %s", error->message);
      g_error_free (error);
   }
   /* the box was held (see edit ()): fp is there still; the panel may
    * have been taken out of its window meanwhile */
   GtkWidget *box = fp->box;
   fp->busy = FALSE;
   if (gtk_widget_get_parent (box)) panel_reload (fp);
   g_object_unref (box);
}


static void
edit (FacePanel *fp, GimvIndexFacesEdit what, gint64 face, const gchar *name, gint64 person)
{
   GArray *ids;

   if (fp->busy) return;
   ids = g_array_new (FALSE, FALSE, sizeof (gint64));
   g_array_append_val (ids, face);
   fp->busy = TRUE;
   g_object_ref (fp->box);
   gimv_index_faces_edit_async (what, ids, name, person, cb_edited, fp);
   g_array_unref (ids);
}


/* the face a dialog asks about (one dialog at a time: they are modal) */
static struct {
   GWeakRef box;      /* the panel box: gone, nothing is done */
   gint64   face;
   gint64   person;
} ask;


static FacePanel *
asked_panel (void)
{
   GtkWidget *box = g_weak_ref_get (&ask.box);
   FacePanel *fp = NULL;

   if (box) {
      fp = g_object_get_data (G_OBJECT (box), "gimv-face-panel-data");
      g_object_unref (box);
   }
   return fp;
}


static void
named (const gchar *name, gpointer data)
{
   FacePanel *fp = asked_panel ();
   if (fp) edit (fp, GIMV_INDEX_FACES_SET_NAME, ask.face, name, 0);
}


static void
tags_given (const gchar *tags, gpointer data)
{
   FacePanel *fp = asked_panel ();
   if (fp) edit (fp, GIMV_INDEX_FACES_PERSON_TAGS, 0, tags, ask.person);
}


static void
rejected (gpointer data)
{
   FacePanel *fp = asked_panel ();
   if (fp) edit (fp, GIMV_INDEX_FACES_REJECT, ask.face, NULL, ask.person);
}


static gpointer
face_ask (FacePanel *fp, PanelFace *f)
{
   g_weak_ref_set (&ask.box, fp->box);
   ask.face = f->id;
   ask.person = f->person ? f->person : f->guess;
   return NULL;
}


static void
cb_action (GSimpleAction *action, GVariant *param, gpointer data)
{
   FacePanel *fp = data;
   const gchar *what = g_action_get_name (G_ACTION (action));
   PanelFace *f = fp->menu_face;

   if (f && fp->iv && !strcmp (what, "copy")) {
      gimv_image_view_copy_face_to_clipboard (fp->iv, f->x, f->y, f->w, f->h);
      return;
   }
   if (!f || fp->busy) return;

   if (!strcmp (what, "name")) {
      gimv_index_faces_ask_name (panel_window (fp), named, face_ask (fp, f));
   } else if (!strcmp (what, "tags")) {
      if (f->person)
         gimv_index_faces_ask_person_tags (panel_window (fp), f->name, f->person, tags_given,
                                           face_ask (fp, f));
   } else if (!strcmp (what, "confirm")) {
      edit (fp, GIMV_INDEX_FACES_CONFIRM, f->id, NULL, 0);
   } else if (!strcmp (what, "reject")) {
      gimv_index_faces_confirm_reject (panel_window (fp), f->name, 1, rejected,
                                       face_ask (fp, f));
   } else if (!strcmp (what, "ignore")) {
      edit (fp, f->ignored ? GIMV_INDEX_FACES_UNIGNORE : GIMV_INDEX_FACES_IGNORE,
            f->id, NULL, 0);
   } else if (!strcmp (what, "person")) {
      gint64 p = f->person ? f->person : f->guess;
      gchar *key = g_strdup_printf ("p:%" G_GINT64_FORMAT, p);
      GimvThumbWin *tw = gimv_thumb_win_get_list () ? gimv_thumb_win_get_list ()->data : NULL;
      if (p && f->name) gimv_index_faces_win_show (tw, key, f->name);
      g_free (key);
   }
}


/* the popover of a face, unparented (and so freed: fp->popover is a weak
 * pointer) */
static void
drop_popover (FacePanel *fp)
{
   if (fp->popover) {
      GtkWidget *popover = fp->popover;
      g_object_remove_weak_pointer (G_OBJECT (popover), (gpointer *) &fp->popover);
      fp->popover = NULL;
      if (gtk_widget_get_parent (popover))
         gtk_widget_unparent (popover);
   }
}


static void
cb_row_destroy (GtkWidget *row, FacePanel *fp)
{
   if (fp->popover && gtk_widget_get_parent (fp->popover) == row)
      drop_popover (fp);
}


static void
show_menu (FacePanel *fp, PanelFace *f, GtkWidget *row)
{
   GMenu *menu = g_menu_new ();
   GSimpleActionGroup *group = g_simple_action_group_new ();
   static const gchar *names[] = { "name", "tags", "confirm", "reject", "ignore", "person",
                                   "copy" };
   guint i;

   fp->menu_face = f;
   for (i = 0; i < G_N_ELEMENTS (names); i++) {
      GSimpleAction *a = g_simple_action_new (names[i], NULL);
      g_signal_connect (a, "activate", G_CALLBACK (cb_action), fp);
      g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (a));
      g_object_unref (a);
   }

   g_menu_append (menu, f->person ? _("Change the Name...") : _("Name..."), "face.name");
   if (f->person && !f->ignored)
      g_menu_append (menu, _("Tags of the Person..."), "face.tags");
   if (!f->person && f->guess && !f->ignored)
      g_menu_append (menu, _("This Is Right"), "face.confirm");
   if ((f->person || f->guess) && !f->ignored)
      g_menu_append (menu, _("Not This Person..."), "face.reject");
   g_menu_append (menu, f->ignored ? _("Do Not Ignore") : _("Ignore"), "face.ignore");
   if ((f->person || f->guess) && !f->ignored)
      g_menu_append (menu, _("All the Faces of This Person..."), "face.person");
   {
      /* apart: it changes nothing */
      GMenu *section = g_menu_new ();
      g_menu_append (section, _("Copy This Face to the Clipboard"), "face.copy");
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);
   }

   drop_popover (fp);
   fp->popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
   g_object_add_weak_pointer (G_OBJECT (fp->popover), (gpointer *) &fp->popover);
   gtk_widget_insert_action_group (fp->box, "face", G_ACTION_GROUP (group));
   gtk_widget_set_parent (fp->popover, row);
   /* the row goes (the list made again, the window closed): the popover
    * with it (a row does not unparent it by itself) */
   g_signal_connect (row, "destroy", G_CALLBACK (cb_row_destroy), fp);
   /* towards the image: the panel is at the right edge of the window */
   gtk_popover_set_position (GTK_POPOVER (fp->popover), GTK_POS_LEFT);
   gtk_popover_popup (GTK_POPOVER (fp->popover));
   g_object_unref (menu);
   g_object_unref (group);
}


/******************************************************************************
 *  the list
 ******************************************************************************/

static PanelFace *
row_face (FacePanel *fp, GtkWidget *row)
{
   guint i = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "face-index"));
   return i < fp->faces->len ? g_ptr_array_index (fp->faces, i) : NULL;
}


static void
cb_enter (GtkEventControllerMotion *ctrl, gdouble x, gdouble y, FacePanel *fp)
{
   GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (ctrl));
   PanelFace *f = row_face (fp, row);
   gchar *label;

   if (!f) return;
   label = face_label (f);
   gimv_image_view_set_face_mark (fp->iv, TRUE, f->x, f->y, f->w, f->h, label);
   g_free (label);
}


static void
cb_leave (GtkEventControllerMotion *ctrl, FacePanel *fp)
{
   gimv_image_view_set_face_mark (fp->iv, FALSE, 0, 0, 0, 0, NULL);
}


static void
cb_pressed (GtkGestureClick *gesture, gint n, gdouble x, gdouble y, FacePanel *fp)
{
   GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
   PanelFace *f = row_face (fp, row);
   if (f) show_menu (fp, f, row);
}


static void
panel_clear (FacePanel *fp)
{
   GtkWidget *child;

   drop_popover (fp);
   fp->menu_face = NULL;
   while ((child = gtk_widget_get_first_child (fp->list)))
      gtk_box_remove (GTK_BOX (fp->list), child);
}


static void
panel_reload (FacePanel *fp)
{
   gint scanned;
   guint i;

   if (!fp->box || !fp->iv || !gtk_widget_get_visible (fp->box)) return;
   panel_clear (fp);
   gimv_image_view_set_face_mark (fp->iv, FALSE, 0, 0, 0, 0, NULL);
   scanned = read_faces (fp);

   if (scanned < 0)
      gtk_label_set_text (GTK_LABEL (fp->message),
                          gimv_index_faces_is_configured ()
                          ? _("Not looked at for faces (not in the folders for faces, "
                              "or not yet).")
                          : _("No folders are given for faces (the Index page of the "
                              "preferences)."));
   else if (!fp->faces->len)
      gtk_label_set_text (GTK_LABEL (fp->message),
                          _("No faces found. A face can be added with the + button."));
   if (scanned < 0 && fp->faces->len)
      scanned = 0;
   gtk_widget_set_visible (fp->message, (scanned < 0 || !fp->faces->len) && !fp->drag);
   if (fp->add_button)
      gtk_widget_set_sensitive (fp->add_button, !fp->adding && fp->iv->info
                                && gimv_index_faces_path_is_for_faces (
                                      gimv_image_info_get_path (fp->iv->info)));

   for (i = 0; i < fp->faces->len; i++) {
      PanelFace *f = g_ptr_array_index (fp->faces, i);
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      GtkWidget *pic;
      GtkWidget *label;
      GdkTexture *texture = gimv_index_faces_get_texture (f->id);
      GtkEventController *motion = gtk_event_controller_motion_new ();
      GtkGesture *click = gtk_gesture_click_new ();
      gchar *text = face_label (f);

      /* a GtkImage keeps its size (a GtkPicture grows with the panel) */
      pic = texture ? gtk_image_new_from_paintable (GDK_PAINTABLE (texture))
                    : gtk_image_new_from_icon_name ("avatar-default-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (pic), FACE_SIZE);
      if (texture) g_object_unref (texture);
      gtk_widget_set_halign (pic, GTK_ALIGN_CENTER);
      label = gtk_label_new (text);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (label), 12);
      /* a guess, no name, ignored: dimmer than a name given */
      if (!f->person) gtk_widget_add_css_class (label, "dim-label");
      g_free (text);
      gtk_box_append (GTK_BOX (row), pic);
      gtk_box_append (GTK_BOX (row), label);
      if (f->person && f->tags && *f->tags && !f->ignored) {
         GtkWidget *tl = gtk_label_new (f->tags);
         gtk_label_set_ellipsize (GTK_LABEL (tl), PANGO_ELLIPSIZE_END);
         gtk_label_set_max_width_chars (GTK_LABEL (tl), 12);
         gtk_widget_add_css_class (tl, "dim-label");
         gtk_widget_add_css_class (tl, "caption");
         gtk_box_append (GTK_BOX (row), tl);
      }
      gtk_widget_set_margin_bottom (row, 6);
      g_object_set_data (G_OBJECT (row), "face-index", GUINT_TO_POINTER (i));

      g_signal_connect (motion, "enter", G_CALLBACK (cb_enter), fp);
      g_signal_connect (motion, "leave", G_CALLBACK (cb_leave), fp);
      gtk_widget_add_controller (row, motion);
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
      g_signal_connect (click, "pressed", G_CALLBACK (cb_pressed), fp);
      gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (click));
      gtk_widget_set_cursor_from_name (row, "pointer");
      gtk_box_append (GTK_BOX (fp->list), row);
   }
}


/******************************************************************************
 *  a face marked by hand: the + button, then a drag on the image
 ******************************************************************************/

static void mark_stop (FacePanel *fp);

static void
mark_message (FacePanel *fp, const gchar *text)
{
   if (!fp->message) return;
   gtk_label_set_text (GTK_LABEL (fp->message), text);
   gtk_widget_set_visible (fp->message, text != NULL);
}


static void
cb_added (GObject *source, GAsyncResult *res, gpointer data)
{
   GtkWidget *box = data;   /* held till now: fp lives as long as it */
   FacePanel *fp = g_object_get_data (G_OBJECT (box), "gimv-face-panel-data");
   GError *error = NULL;
   gboolean exists = FALSE, found = FALSE;
   gint64 face = gimv_index_faces_add_finish (res, &exists, &found, &error);

   if (fp && fp->box && fp->iv) {
      fp->adding = FALSE;
      panel_reload (fp);
      if (!face)
         mark_message (fp, error ? error->message : "?");
      else if (exists)
         mark_message (fp, _("A face is there already."));
      else if (!found)
         mark_message (fp, _("Added as marked (not found as a face there: it may be "
                             "guessed less well)."));
   }
   g_clear_error (&error);
   g_object_unref (box);
}


static void
cb_drag_begin (GtkGestureDrag *drag, gdouble x, gdouble y, FacePanel *fp)
{
   gdouble fx, fy;

   /* only on the image */
   if (!gimv_image_view_point_to_face (fp->iv, x, y, &fx, &fy)
       || fx < 0 || fy < 0 || fx > 1 || fy > 1) {
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
      return;
   }
   /* not the view's own (scrolling, the next image) */
   gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
   fp->drag_x = x;
   fp->drag_y = y;
   fp->mark_w = fp->mark_h = 0;
}


static void
cb_drag_update (GtkGestureDrag *drag, gdouble dx, gdouble dy, FacePanel *fp)
{
   gdouble x0, y0, x1, y1;

   if (!gimv_image_view_point_to_face (fp->iv, fp->drag_x, fp->drag_y, &x0, &y0)
       || !gimv_image_view_point_to_face (fp->iv, fp->drag_x + dx, fp->drag_y + dy, &x1, &y1))
      return;
   fp->mark_x = CLAMP (MIN (x0, x1), 0, 1);
   fp->mark_y = CLAMP (MIN (y0, y1), 0, 1);
   fp->mark_w = CLAMP (MAX (x0, x1), 0, 1) - fp->mark_x;
   fp->mark_h = CLAMP (MAX (y0, y1), 0, 1) - fp->mark_y;
   gimv_image_view_set_face_mark (fp->iv, TRUE, fp->mark_x, fp->mark_y,
                                  fp->mark_w, fp->mark_h, NULL);
}


/* the drag taken off the drawing area after its signal, not in it */
static gboolean
idle_mark_stop (gpointer data)
{
   GtkWidget *box = data;
   FacePanel *fp = g_object_get_data (G_OBJECT (box), "gimv-face-panel-data");
   if (fp && fp->box) mark_stop (fp);
   g_object_unref (box);
   return G_SOURCE_REMOVE;
}


static void
cb_drag_end (GtkGestureDrag *drag, gdouble dx, gdouble dy, FacePanel *fp)
{
   gint w, h, px, py, pw, ph;

   cb_drag_update (drag, dx, dy, fp);
   if (!upright_size (fp->iv, &w, &h) || !fp->iv->info) {
      g_idle_add (idle_mark_stop, g_object_ref (fp->box));
      return;
   }
   px = (gint) (fp->mark_x * w);
   py = (gint) (fp->mark_y * h);
   pw = (gint) (fp->mark_w * w);
   ph = (gint) (fp->mark_h * h);
   if (pw < 6 || ph < 6) {
      /* a click, not a drag: still marking */
      gimv_image_view_set_face_mark (fp->iv, FALSE, 0, 0, 0, 0, NULL);
      return;
   }
   g_idle_add (idle_mark_stop, g_object_ref (fp->box));
   gimv_image_view_set_face_mark (fp->iv, TRUE, fp->mark_x, fp->mark_y,
                                  fp->mark_w, fp->mark_h, _("adding..."));
   fp->adding = TRUE;
   if (fp->add_button) gtk_widget_set_sensitive (fp->add_button, FALSE);
   mark_message (fp, _("Adding the face..."));
   gimv_index_faces_add_async (gimv_image_info_get_path (fp->iv->info), px, py, pw, ph,
                               cb_added, g_object_ref (fp->box));
}


static gboolean
cb_mark_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
             FacePanel *fp)
{
   if (keyval == GDK_KEY_Escape) {
      mark_stop (fp);
      mark_message (fp, NULL);
      gimv_image_view_set_face_mark (fp->iv, FALSE, 0, 0, 0, 0, NULL);
      return TRUE;
   }
   return FALSE;
}


static void
mark_stop (FacePanel *fp)
{
   if (fp->drag && fp->iv && fp->iv->draw_area)
      gtk_widget_remove_controller (fp->iv->draw_area, GTK_EVENT_CONTROLLER (fp->drag));
   if (fp->keys && fp->iv)
      gtk_widget_remove_controller (GTK_WIDGET (fp->iv), fp->keys);
   fp->drag = NULL;
   fp->keys = NULL;
   if (fp->iv && fp->iv->draw_area)
      gtk_widget_set_cursor (fp->iv->draw_area, NULL);
   if (fp->add_button)
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (fp->add_button), FALSE);
}


static void
cb_add_toggled (GtkToggleButton *button, FacePanel *fp)
{
   if (!gtk_toggle_button_get_active (button)) {
      if (fp->drag) {
         mark_stop (fp);
         mark_message (fp, NULL);
      }
      return;
   }
   if (fp->drag || !fp->iv || !fp->iv->draw_area) return;
   fp->drag = gtk_gesture_drag_new ();
   gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (fp->drag),
                                               GTK_PHASE_CAPTURE);
   gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (fp->drag), GDK_BUTTON_PRIMARY);
   g_signal_connect (fp->drag, "drag-begin", G_CALLBACK (cb_drag_begin), fp);
   g_signal_connect (fp->drag, "drag-update", G_CALLBACK (cb_drag_update), fp);
   g_signal_connect (fp->drag, "drag-end", G_CALLBACK (cb_drag_end), fp);
   gtk_widget_add_controller (fp->iv->draw_area, GTK_EVENT_CONTROLLER (fp->drag));
   fp->keys = gtk_event_controller_key_new ();
   gtk_event_controller_set_propagation_phase (fp->keys, GTK_PHASE_CAPTURE);
   g_signal_connect (fp->keys, "key-pressed", G_CALLBACK (cb_mark_key), fp);
   gtk_widget_add_controller (GTK_WIDGET (fp->iv), fp->keys);
   gtk_widget_set_cursor_from_name (fp->iv->draw_area, "crosshair");
   gimv_image_view_set_face_mark (fp->iv, FALSE, 0, 0, 0, 0, NULL);
   mark_message (fp, _("Drag over the face on the image (Esc: stop)."));
}


static void
cb_image_changed (GimvImageView *iv, FacePanel *fp)
{
   if (fp->drag) {
      mark_stop (fp);
      mark_message (fp, NULL);
   }
   panel_reload (fp);
}


static void
cb_index_changed (GimvIndexChange what, gpointer data)
{
   FacePanel *fp = data;
   /* the groups made again: the guesses may have changed */
   if ((what & GIMV_INDEX_CHANGED_FACES) && !fp->busy)
      panel_reload (fp);
}


static void
cb_done (GtkButton *button, FacePanel *fp)
{
   gimv_face_panel_set_visible (fp->iv, FALSE);
}


static void
cb_box_destroy (GtkWidget *widget, FacePanel *fp)
{
   if (fp->watch_id) gimv_index_remove_watch (fp->watch_id);
   fp->watch_id = 0;
   fp->add_button = NULL;   /* gone with the box */
   if (fp->drag || fp->keys) mark_stop (fp);
   /* not panel_clear (): the children of the box are gone already (a box
    * unparents them before its "destroy"), the rows and the popover too */
   drop_popover (fp);
   fp->menu_face = NULL;
   fp->list = NULL;
   fp->message = NULL;
   if (fp->iv) {   /* a weak pointer: the view may have gone before */
      g_object_remove_weak_pointer (G_OBJECT (fp->iv), (gpointer *) &fp->iv);
      g_signal_handlers_disconnect_by_data (fp->iv, fp);
      g_object_set_data (G_OBJECT (fp->iv), PANEL_KEY, NULL);
      fp->iv = NULL;
   }
   g_ptr_array_unref (fp->faces);
   fp->box = NULL;
   /* an edit still running keeps a reference of the box, so fp is freed
    * only now, with it */
   g_free (fp);
}


/******************************************************************************
 *  public
 ******************************************************************************/

GtkWidget *
gimv_face_panel_wrap (GimvImageView *iv)
{
   FacePanel *fp = g_new0 (FacePanel, 1);
   GtkWidget *hbox, *header, *title, *done, *scroll;

   fp->iv = iv;
   g_object_add_weak_pointer (G_OBJECT (iv), (gpointer *) &fp->iv);
   fp->faces = g_ptr_array_new_with_free_func ((GDestroyNotify) panel_face_free);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_set_hexpand (GTK_WIDGET (iv), TRUE);
   gtk_widget_set_vexpand (GTK_WIDGET (iv), TRUE);
   gtk_box_append (GTK_BOX (hbox), GTK_WIDGET (iv));

   fp->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_size_request (fp->box, FACE_SIZE + 40, -1);
   gtk_widget_set_hexpand (fp->box, FALSE);
   gtk_widget_set_margin_start (fp->box, 4);
   gtk_widget_set_margin_end (fp->box, 4);
   gtk_widget_set_margin_top (fp->box, 4);

   header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
   title = gtk_label_new (_("Faces"));
   gtk_widget_add_css_class (title, "heading");
   gtk_widget_set_hexpand (title, TRUE);
   gtk_label_set_xalign (GTK_LABEL (title), 0.0);
   done = gtk_button_new_with_mnemonic (_("_Done"));
   gtk_button_set_has_frame (GTK_BUTTON (done), FALSE);
   g_signal_connect (done, "clicked", G_CALLBACK (cb_done), fp);
   fp->add_button = gtk_toggle_button_new ();
   gtk_button_set_icon_name (GTK_BUTTON (fp->add_button), "list-add-symbolic");
   gtk_button_set_has_frame (GTK_BUTTON (fp->add_button), FALSE);
   gtk_widget_set_tooltip_text (fp->add_button,
                                _("Add a face not found: drag over it on the image"));
   g_signal_connect (fp->add_button, "toggled", G_CALLBACK (cb_add_toggled), fp);
   gtk_box_append (GTK_BOX (header), title);
   gtk_box_append (GTK_BOX (header), fp->add_button);
   gtk_box_append (GTK_BOX (header), done);
   gtk_box_append (GTK_BOX (fp->box), header);

   fp->message = gtk_label_new (NULL);
   gtk_label_set_wrap (GTK_LABEL (fp->message), TRUE);
   gtk_label_set_max_width_chars (GTK_LABEL (fp->message), 12);
   gtk_widget_add_css_class (fp->message, "dim-label");
   gtk_box_append (GTK_BOX (fp->box), fp->message);

   fp->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), fp->list);
   gtk_box_append (GTK_BOX (fp->box), scroll);

   gtk_widget_set_visible (fp->box, FALSE);
   gtk_box_append (GTK_BOX (hbox), fp->box);

   g_object_set_data (G_OBJECT (iv), PANEL_KEY, fp);
   g_object_set_data (G_OBJECT (fp->box), "gimv-face-panel-data", fp);
   g_signal_connect (iv, "image_changed", G_CALLBACK (cb_image_changed), fp);
   g_signal_connect (fp->box, "destroy", G_CALLBACK (cb_box_destroy), fp);
   fp->watch_id = gimv_index_add_watch (cb_index_changed, fp);
   return hbox;
}


gboolean
gimv_face_panel_get_visible (GimvImageView *iv)
{
   FacePanel *fp = iv ? g_object_get_data (G_OBJECT (iv), PANEL_KEY) : NULL;
   return fp && fp->box && gtk_widget_get_visible (fp->box);
}


void
gimv_face_panel_set_visible (GimvImageView *iv, gboolean visible)
{
   FacePanel *fp = iv ? g_object_get_data (G_OBJECT (iv), PANEL_KEY) : NULL;

   if (!fp || !fp->box) return;
   if (!visible && fp->drag) mark_stop (fp);
   gtk_widget_set_visible (fp->box, visible);
   if (visible)
      panel_reload (fp);
   else
      gimv_image_view_set_face_mark (iv, FALSE, 0, 0, 0, 0, NULL);
}


gboolean
gimv_face_panel_has (GimvImageView *iv)
{
   return iv && g_object_get_data (G_OBJECT (iv), PANEL_KEY) != NULL;
}
