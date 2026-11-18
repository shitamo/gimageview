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
 * [File Names to UTF-8...]: the files of a folder whose names are not in
 * UTF-8 (Shift_JIS of the MS-DOS days, EUC-JP of UNIX) renamed to UTF-8.
 * Each name is shown as its bytes, the charset (guessed, can be chosen) and
 * the new name; a name that would be the name of a file there already (or
 * of another file renamed) is not renamed: nothing is overwritten.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE   /* renameat2 () */
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "gimageview.h"
#include "charset.h"
#include "fileutil.h"
#include "gimv_image_info.h"
#include "gimv_thumb.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"
#include "gtkutils.h"
#include "prefs.h"

/* the charsets offered: as shown, as iconv knows them */
static const gchar *charsets[][2] = {
   { "Shift_JIS",   "CP932" },
   { "EUC-JP",      "EUC-JP" },
   { "ISO-2022-JP", "ISO-2022-JP" },
   { "ISO-8859-1",  "ISO-8859-1" },     /* Latin-1: any bytes read */
};

typedef struct NamesDialog_Tag NamesDialog;

typedef struct {
   NamesDialog *nd;
   GimvThumb   *thumb;
   gchar       *path;        /* the file as it is */
   gchar       *base;        /* its name (bytes) */
   gchar       *new_name;    /* in the charset chosen; NULL: not readable */
   gboolean     ok;          /* can be renamed */
   gboolean     wanted;      /* checked by the user */
   GtkWidget   *check, *charset, *new_label, *state;
} NameRow;

struct NamesDialog_Tag {
   GimvThumbView *tv;        /* weak */
   gchar         *dir;
   GtkWidget     *window, *convert;
   GPtrArray     *rows;
   gboolean       updating;
};


static void
name_row_free (NameRow *r)
{
   g_object_unref (r->thumb);
   g_free (r->path);
   g_free (r->base);
   g_free (r->new_name);
   g_free (r);
}


/* the new names again: what is readable, and that no two files get one */
static void
update_rows (NamesDialog *nd)
{
   GHashTable *taken = g_hash_table_new (g_str_hash, g_str_equal);
   guint i, n = 0;

   nd->updating = TRUE;
   for (i = 0; i < nd->rows->len; i++) {
      NameRow *r = g_ptr_array_index (nd->rows, i);
      guint cs = gtk_drop_down_get_selected (GTK_DROP_DOWN (r->charset));
      const gchar *state = "";
      gchar *text;
      gsize written;

      g_free (r->new_name);
      r->new_name = NULL;
      if (cs < G_N_ELEMENTS (charsets)) {
         text = g_convert (r->base, -1, "UTF-8", charsets[cs][1], NULL, &written, NULL);
         if (text && strlen (text) == written && !strchr (text, '/'))
            r->new_name = text;
         else
            g_free (text);
      }

      r->ok = FALSE;
      if (!r->new_name) {
         state = _("Not readable in this charset");
      } else {
         gchar *dest = g_build_filename (nd->dir, r->new_name, NULL);
         struct stat st;
         if (!lstat (dest, &st))
            state = _("A file of this name is there");
         else if (r->wanted && g_hash_table_contains (taken, r->new_name))
            state = _("Another file gets this name");
         else
            r->ok = TRUE;
         g_free (dest);
      }
      if (r->ok && r->wanted) {
         g_hash_table_add (taken, r->new_name);
         n++;
      }
      /* checked: what will be renamed */
      gtk_check_button_set_active (GTK_CHECK_BUTTON (r->check), r->ok && r->wanted);

      gtk_label_set_text (GTK_LABEL (r->new_label), r->new_name ? r->new_name : "—");
      gtk_label_set_text (GTK_LABEL (r->state), state);
      gtk_widget_set_sensitive (r->check, r->ok);
   }
   g_hash_table_destroy (taken);
   nd->updating = FALSE;

   {
      gchar *label = g_strdup_printf (ngettext ("_Rename %u File", "_Rename %u Files", n), n);
      gtk_button_set_label (GTK_BUTTON (nd->convert), label);
      g_free (label);
   }
   gtk_widget_set_sensitive (nd->convert, n > 0);
}


static void
cb_row_changed (GObject *object, GParamSpec *pspec, NamesDialog *nd)
{
   guint i;

   if (nd->updating) return;
   for (i = 0; i < nd->rows->len; i++) {
      NameRow *r = g_ptr_array_index (nd->rows, i);
      if (object == G_OBJECT (r->check))
         r->wanted = gtk_check_button_get_active (GTK_CHECK_BUTTON (r->check));
   }
   update_rows (nd);
}


static void
cb_cancel (GtkWidget *widget, NamesDialog *nd)
{
   gtk_window_destroy (GTK_WINDOW (nd->window));
}


static void
cb_destroy (GtkWidget *widget, NamesDialog *nd)
{
   if (nd->tv) g_object_remove_weak_pointer (G_OBJECT (nd->tv), (gpointer *) &nd->tv);
   g_ptr_array_unref (nd->rows);
   g_free (nd->dir);
   g_free (nd);
}


/* src to dest; never over a file there */
static gboolean
rename_no_replace (const gchar *src, const gchar *dest)
{
#ifdef RENAME_NOREPLACE
   if (renameat2 (AT_FDCWD, src, AT_FDCWD, dest, RENAME_NOREPLACE) == 0)
      return TRUE;
   if (errno != EINVAL && errno != ENOSYS) return FALSE;
   /* a file system that cannot: as below */
#endif
   {
      struct stat st;
      if (!lstat (dest, &st)) {
         errno = EEXIST;
         return FALSE;
      }
      return rename (src, dest) == 0;
   }
}


static void
cb_convert (GtkWidget *widget, NamesDialog *nd)
{
   GString *failed = g_string_new (NULL);
   guint i, done = 0;

   update_rows (nd);
   for (i = 0; i < nd->rows->len; i++) {
      NameRow *r = g_ptr_array_index (nd->rows, i);
      gchar *dest;

      if (!r->ok || !r->wanted) continue;
      dest = g_build_filename (nd->dir, r->new_name, NULL);
      if (rename_no_replace (r->path, dest)) {
         gimv_thumb_view_rename_extras (r->thumb, dest);
         done++;
      } else {
         gchar *bytes = charset_escape_bytes (r->base, -1);
         g_string_append_printf (failed, "%s → %s: %s\n", bytes, r->new_name,
                                 g_strerror (errno));
         g_free (bytes);
      }
      g_free (dest);
   }

   if (nd->tv && done) gimv_thumb_view_refresh_list (nd->tv);
   if (failed->len) {
      gchar *msg = g_strdup_printf (_("These files could not be renamed:\n\n%s"), failed->str);
      gtkutil_message_dialog (_("Error!"), msg,
                              GTK_WINDOW (gtk_window_get_transient_for (GTK_WINDOW (nd->window))));
      g_free (msg);
   }
   g_string_free (failed, TRUE);
   gtk_window_destroy (GTK_WINDOW (nd->window));
}


static GtkWidget *
cell_label (const gchar *text, gboolean bold)
{
   GtkWidget *label = gtk_label_new (text);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   if (bold) gtk_widget_add_css_class (label, "heading");
   return label;
}


void
gimv_thumb_view_names_to_utf8 (GimvThumbView *tv)
{
   NamesDialog *nd;
   GtkWidget *vbox, *label, *scroll, *grid, *hbox, *button;
   const gchar *names[G_N_ELEMENTS (charsets) + 1];
   GList *node;
   guint i;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));

   if (tv->mode != GIMV_THUMB_VIEW_MODE_DIR || !gimv_thumb_view_get_path (tv)) {
      gtkutil_message_dialog (_("File Names to UTF-8"),
                              _("File names can be renamed in a folder only."),
                              GTK_WINDOW (tv->tw));
      return;
   }

   nd = g_new0 (NamesDialog, 1);
   nd->tv = tv;
   g_object_add_weak_pointer (G_OBJECT (tv), (gpointer *) &nd->tv);
   nd->dir = g_strdup (gimv_thumb_view_get_path (tv));
   nd->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) name_row_free);

   for (node = tv->thumblist; node; node = g_list_next (node)) {
      GimvThumb *thumb = node->data;
      const gchar *path;
      NameRow *r;

      if (!GIMV_IS_THUMB (thumb) || !thumb->info) continue;
      path = gimv_image_info_get_path (thumb->info);
      if (!path || g_utf8_validate (fileutil_basename (path), -1, NULL)) continue;
      r = g_new0 (NameRow, 1);
      r->nd = nd;
      r->thumb = g_object_ref (thumb);
      r->path = g_strdup (path);
      r->base = g_strdup (fileutil_basename (path));
      g_ptr_array_add (nd->rows, r);
   }

   if (!nd->rows->len) {
      gtkutil_message_dialog (_("File Names to UTF-8"),
                              _("All the file names here are in UTF-8."),
                              GTK_WINDOW (tv->tw));
      cb_destroy (NULL, nd);
      return;
   }

   nd->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (nd->window), _("File Names to UTF-8"));
   gtk_window_set_transient_for (GTK_WINDOW (nd->window), GTK_WINDOW (tv->tw));
   gtk_window_set_modal (GTK_WINDOW (nd->window), TRUE);
   gtk_window_set_default_size (GTK_WINDOW (nd->window), 760, 420);
   g_signal_connect (nd->window, "destroy", G_CALLBACK (cb_destroy), nd);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (nd->window), vbox);

   label = gtk_label_new (_("The names of these files are not in UTF-8: in the list they "
                            "are shown in italics in the charset guessed, or as %XX when "
                            "they could not be read.  Check the new names and choose the "
                            "charset where they are wrong.  A file whose new name is "
                            "taken already is not renamed."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (vbox), label);

   scroll = gtk_scrolled_window_new ();
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_box_append (GTK_BOX (vbox), scroll);
   grid = gtk_grid_new ();
   gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
   gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), grid);

   gtk_grid_attach (GTK_GRID (grid), cell_label (_("Name now (bytes)"), TRUE), 1, 0, 1, 1);
   gtk_grid_attach (GTK_GRID (grid), cell_label (_("Charset"), TRUE), 2, 0, 1, 1);
   gtk_grid_attach (GTK_GRID (grid), cell_label (_("New name"), TRUE), 3, 0, 1, 1);

   for (i = 0; i < G_N_ELEMENTS (charsets); i++) names[i] = charsets[i][0];
   names[i] = NULL;

   for (i = 0; i < nd->rows->len; i++) {
      NameRow *r = g_ptr_array_index (nd->rows, i);
      const gchar *guessed = NULL;
      gchar *bytes = charset_escape_bytes (r->base, -1), *text;
      guint j, sel = 0;

      /* the guess first; else the first charset that reads it */
      text = charset_guess_to_utf8 (r->base, -1, TRUE, &guessed);
      g_free (text);
      for (j = 0, sel = G_MAXUINT; j < G_N_ELEMENTS (charsets) && sel == G_MAXUINT; j++) {
         if (guessed && !strcmp (guessed, charsets[j][0])) sel = j;
      }
      for (j = 0; j < G_N_ELEMENTS (charsets) && sel == G_MAXUINT; j++) {
         gsize written;
         text = g_convert (r->base, -1, "UTF-8", charsets[j][1], NULL, &written, NULL);
         if (text && strlen (text) == written) sel = j;
         g_free (text);
      }
      if (sel == G_MAXUINT) sel = 0;
      r->wanted = TRUE;

      r->check = gtk_check_button_new ();
      gtk_check_button_set_active (GTK_CHECK_BUTTON (r->check), TRUE);
      r->charset = gtk_drop_down_new_from_strings (names);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (r->charset), sel);
      r->new_label = cell_label (NULL, FALSE);
      r->state = cell_label (NULL, FALSE);
      gtk_widget_add_css_class (r->state, "dim-label");

      gtk_grid_attach (GTK_GRID (grid), r->check, 0, i + 1, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), cell_label (bytes, FALSE), 1, i + 1, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), r->charset, 2, i + 1, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), r->new_label, 3, i + 1, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), r->state, 4, i + 1, 1, 1);
      g_free (bytes);

      g_signal_connect (r->check, "notify::active", G_CALLBACK (cb_row_changed), nd);
      g_signal_connect (r->charset, "notify::selected", G_CALLBACK (cb_row_changed), nd);
   }

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_set_halign (hbox, GTK_ALIGN_END);
   gtk_box_append (GTK_BOX (vbox), hbox);
   button = gtk_button_new_with_mnemonic (_("_Cancel"));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_cancel), nd);
   gtk_box_append (GTK_BOX (hbox), button);
   nd->convert = gtk_button_new_with_mnemonic ("");
   gtk_widget_add_css_class (nd->convert, "suggested-action");
   g_signal_connect (nd->convert, "clicked", G_CALLBACK (cb_convert), nd);
   gtk_box_append (GTK_BOX (hbox), nd->convert);

   update_rows (nd);
   gtk_window_present (GTK_WINDOW (nd->window));
}
