/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the notice icon of the thumbnail window.
 *
 * Instead of dialogs, the things to confirm or to know about are gathered
 * behind an icon in the status bar, which is shown only while there are
 * some.  For now: images (or folders) with comment files that another
 * program moved or renamed below the indexed directories, where the user
 * chooses whether the comment files (and so the tags) follow them.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>

#include "gimv_index.h"
#include "gimv_index_notice.h"
#include "gimv_thumb_win.h"

/* gimv moves the comment file itself right after the image: look again a
 * little later before asking */
#define CHECK_DELAY_MS 1500

typedef struct {
   gchar   *old;
   gchar   *new;
   gboolean dir;
} Move;

static GPtrArray *pending = NULL;   /* Move, waiting for an answer */
static GPtrArray *notes = NULL;     /* gchar *, things to know */
static gchar     *missing_note = NULL;   /* in notes, replaced while the log grows */


static void
move_free (Move *m)
{
   g_free (m->old);
   g_free (m->new);
   g_free (m);
}


static gchar *
comment_path (const gchar *path, gboolean dir)
{
   /* the same place as gimv_comment_get_path () */
   return dir ? g_strconcat (g_getenv ("HOME"), "/.gimv/comment", path, NULL)
              : g_strconcat (g_getenv ("HOME"), "/.gimv/comment", path, ".txt", NULL);
}


/******************************************************************************
 *  the icon in every thumbnail window
 ******************************************************************************/

static void update_buttons (void);


static gboolean
move_comment_file (const gchar *from, const gchar *to)
{
   gchar *dir = g_path_get_dirname (to);
   gboolean ok;

   g_mkdir_with_parents (dir, 0700);
   g_free (dir);
   ok = g_rename (from, to) == 0;
   if (!ok)
      g_warning ("cannot move %s to %s: %s", from, to, g_strerror (errno));
   return ok;
}


/* the comment files below a directory, one by one (the target directory
 * may exist already) */
static void
move_comment_tree (const gchar *from, const gchar *to, const gchar *img_to)
{
   GDir *d = g_dir_open (from, 0, NULL);
   const gchar *name;

   if (!d) return;
   while ((name = g_dir_read_name (d))) {
      gchar *f = g_build_filename (from, name, NULL);
      gchar *t = g_build_filename (to, name, NULL);

      if (g_file_test (f, G_FILE_TEST_IS_DIR)) {
         gchar *img = g_build_filename (img_to, name, NULL);
         move_comment_tree (f, t, img);
         g_free (img);
      } else if (g_str_has_suffix (name, ".txt") && !g_file_test (t, G_FILE_TEST_EXISTS)) {
         if (move_comment_file (f, t)) {
            gchar *img_name = g_strndup (name, strlen (name) - 4);
            gchar *img = g_build_filename (img_to, img_name, NULL);
            gimv_index_tags_changed (img);
            g_free (img);
            g_free (img_name);
         }
      }
      g_free (f);
      g_free (t);
   }
   g_dir_close (d);
   g_rmdir (from);   /* when empty */
}


static void
answer_moves (gboolean move)
{
   guint i;

   if (!pending) return;

   for (i = 0; move && i < pending->len; i++) {
      Move *m = g_ptr_array_index (pending, i);
      gchar *from = comment_path (m->old, m->dir);
      gchar *to   = comment_path (m->new, m->dir);

      if (m->dir) {
         move_comment_tree (from, to, m->new);
      } else if (g_file_test (from, G_FILE_TEST_IS_REGULAR)
                 && !g_file_test (to, G_FILE_TEST_EXISTS)) {
         if (move_comment_file (from, to))
            gimv_index_tags_changed (m->new);
      }
      g_free (from);
      g_free (to);
   }
   /* not moved: the comment files stay at the old place (a later log of
    * missing files will show them) */
   g_ptr_array_set_size (pending, 0);
   update_buttons ();
}


static void
cb_move_clicked (GtkButton *button, gpointer data)
{
   GtkWidget *popover = gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_POPOVER);
   if (popover) gtk_popover_popdown (GTK_POPOVER (popover));
   answer_moves (GPOINTER_TO_INT (data));
}


static void
cb_dismiss_note (GtkButton *button, gpointer data)
{
   GtkWidget *popover = gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_POPOVER);
   if (popover) gtk_popover_popdown (GTK_POPOVER (popover));
   if (notes) g_ptr_array_set_size (notes, 0);
   missing_note = NULL;
   update_buttons ();
}


static GtkWidget *
label_new (const gchar *text, gboolean wrap)
{
   GtkWidget *label = gtk_label_new (text);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   if (wrap) {
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (label), 60);
   } else {
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
      gtk_label_set_max_width_chars (GTK_LABEL (label), 70);
   }
   return label;
}


/* the contents of the popover, made when it opens */
static void
fill_popover (GtkPopover *popover)
{
   GtkWidget *box, *label, *hbox, *button, *list, *scroll;
   guint i, n = pending ? pending->len : 0;

   box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (box, 6);
   gtk_widget_set_margin_end (box, 6);
   gtk_widget_set_margin_top (box, 6);
   gtk_widget_set_margin_bottom (box, 6);

   if (n) {
      gchar *text = g_strdup_printf (
         ngettext ("%u image or folder with comments (tags) was moved by another program.",
                   "%u images or folders with comments (tags) were moved by another program.",
                   n), n);
      gtk_box_append (GTK_BOX (box), label_new (text, TRUE));
      g_free (text);

      list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      for (i = 0; i < n && i < 200; i++) {
         Move *m = g_ptr_array_index (pending, i);
         gchar *text2 = g_strdup_printf ("%s%s → %s", m->dir ? "📁 " : "", m->old, m->new);
         gtk_box_append (GTK_BOX (list), label_new (text2, FALSE));
         g_free (text2);
      }
      if (n > 200) {
         gchar *more = g_strdup_printf (_("and %u more"), n - 200);
         gtk_box_append (GTK_BOX (list), label_new (more, FALSE));
         g_free (more);
      }
      scroll = gtk_scrolled_window_new ();
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                      GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 240);
      gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
      gtk_box_append (GTK_BOX (box), scroll);

      hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_halign (hbox, GTK_ALIGN_END);
      button = gtk_button_new_with_label (_("Leave the Comments"));
      g_signal_connect (button, "clicked", G_CALLBACK (cb_move_clicked), GINT_TO_POINTER (FALSE));
      gtk_box_append (GTK_BOX (hbox), button);
      button = gtk_button_new_with_label (_("Move the Comments Too"));
      gtk_widget_add_css_class (button, "suggested-action");
      g_signal_connect (button, "clicked", G_CALLBACK (cb_move_clicked), GINT_TO_POINTER (TRUE));
      gtk_box_append (GTK_BOX (hbox), button);
      gtk_box_append (GTK_BOX (box), hbox);
   }

   if (notes && notes->len) {
      if (n) gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
      for (i = 0; i < notes->len; i++)
         gtk_box_append (GTK_BOX (box), label_new (g_ptr_array_index (notes, i), TRUE));
      button = gtk_button_new_with_label (_("OK"));
      gtk_widget_set_halign (button, GTK_ALIGN_END);
      g_signal_connect (button, "clicked", G_CALLBACK (cb_dismiss_note), NULL);
      gtk_box_append (GTK_BOX (box), button);
   }

   gtk_popover_set_child (popover, box);
   (void) label;
}


static void
cb_popover_show (GtkPopover *popover, gpointer data)
{
   fill_popover (popover);
}


static guint
notice_count (void)
{
   return (pending ? pending->len : 0) + (notes ? notes->len : 0);
}


static void
update_button (GtkWidget *button)
{
   guint n = notice_count ();
   gchar *text;

   gtk_widget_set_visible (button, n > 0);
   if (!n) return;
   text = g_strdup_printf ("%u", n);
   gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (button), "count")), text);
   g_free (text);
}


static void
update_buttons (void)
{
   GList *node;

   for (node = gimv_thumb_win_get_list (); node; node = g_list_next (node)) {
      GimvThumbWin *tw = node->data;
      if (tw && tw->notice_button)
         update_button (tw->notice_button);
   }
}


GtkWidget *
gimv_index_notice_button_new (void)
{
   GtkWidget *button = gtk_menu_button_new ();
   GtkWidget *popover = gtk_popover_new ();

   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
   GtkWidget *label = gtk_label_new (NULL);

   /* an icon and the number of notices */
   gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("dialog-information-symbolic"));
   gtk_box_append (GTK_BOX (box), label);
   gtk_menu_button_set_child (GTK_MENU_BUTTON (button), box);
   g_object_set_data (G_OBJECT (button), "count", label);
   gtk_widget_set_tooltip_text (button, _("Notices"));
   gtk_widget_set_name (button, "IndexNotice");
   gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), popover);
   g_signal_connect (popover, "show", G_CALLBACK (cb_popover_show), NULL);
   update_button (button);
   return button;
}


/******************************************************************************
 *  notices from the indexer
 ******************************************************************************/

static gboolean
cb_check_move (gpointer data)
{
   Move *m = data;
   gchar *from = comment_path (m->old, m->dir);
   gchar *to   = comment_path (m->new, m->dir);
   gboolean ask;

   /* gimv moved the comment file itself: nothing to ask */
   ask = g_file_test (from, m->dir ? G_FILE_TEST_IS_DIR : G_FILE_TEST_IS_REGULAR)
         && (m->dir || !g_file_test (to, G_FILE_TEST_EXISTS));
   g_free (from);
   g_free (to);

   if (ask) {
      if (!pending)
         pending = g_ptr_array_new_with_free_func ((GDestroyNotify) move_free);
      g_ptr_array_add (pending, m);
      update_buttons ();
   } else {
      move_free (m);
   }
   return G_SOURCE_REMOVE;
}


void
gimv_index_notice_moved (const gchar *old, const gchar *new, gboolean dir)
{
   Move *m;

   g_return_if_fail (old && new);
   m = g_new0 (Move, 1);
   m->old = g_strdup (old);
   m->new = g_strdup (new);
   m->dir = dir;
   g_timeout_add (CHECK_DELAY_MS, cb_check_move, m);
}


static void
add_note (gchar *text)
{
   if (!notes) notes = g_ptr_array_new_with_free_func (g_free);
   g_ptr_array_add (notes, text);
   update_buttons ();
}


/* the moves nobody answered: gimv quits */
GList *
gimv_index_notice_take_unanswered (void)
{
   GList *list = NULL;
   guint i;

   for (i = 0; pending && i < pending->len; i++) {
      Move *m = g_ptr_array_index (pending, i);
      if (!m->dir)
         list = g_list_append (list, g_strdup_printf ("%s\t%s", m->old, m->new));
   }
   return list;
}


void
gimv_index_notice_missing (const gchar *log, guint n)
{
   gchar *text = g_strdup_printf (
      ngettext ("%u image with comments (tags) is missing. Its comment file is kept; see %s",
                "%u images with comments (tags) are missing. Their comment files are kept; see %s",
                n), n, log);

   if (!notes) notes = g_ptr_array_new_with_free_func (g_free);
   if (missing_note && g_ptr_array_remove (notes, missing_note))
      missing_note = NULL;
   missing_note = text;
   g_ptr_array_add (notes, text);
   update_buttons ();
}


void
gimv_index_notice_logs_pruned (guint n)
{
   if (!notes) notes = g_ptr_array_new_with_free_func (g_free);
   g_ptr_array_add (notes, g_strdup_printf (
      ngettext ("%u old log of missing images was deleted.",
                "%u old logs of missing images were deleted.", n), n));
   update_buttons ();
}


void
gimv_index_notice_added_since_last (guint n)
{
   /* images that came while gimv was not running: no desktop notification,
    * just once here */
   add_note (g_strdup_printf (ngettext ("%u new image since GImageView ran last time.",
                                        "%u new images since GImageView ran last time.", n),
                              n));
}


void
gimv_index_notice_watch_limit (guint n_watched)
{
   add_note (g_strdup_printf (
      _("Only %u folders can be watched for changes (the system limit "
        "fs.inotify.max_user_watches). Changes in the other folders are "
        "found the next time GImageView starts."), n_watched));
}
