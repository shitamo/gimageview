/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: bookmarking images (the popover with the note).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "intl.h"
#include <gtk/gtk.h>

#include "gimv_bookmark.h"
#include "gimv_bookmark_edit.h"
#include "gimv_image_info.h"

typedef struct {
   GtkWidget *popover;
   GtkWidget *entry;
   GPtrArray *paths;
   gchar     *note;       /* the note when it opened */
   gboolean   removed;
} Edit;


static void
edit_free (Edit *e)
{
   g_ptr_array_unref (e->paths);
   g_free (e->note);
   g_free (e);
}


/* the note is kept however the popover closes (Enter, Esc, a click
 * elsewhere): a bookmark is there already, the note is only added */
static void
save_note (Edit *e)
{
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (e->entry));
   guint i;

   if (e->removed || !g_strcmp0 (text, e->note ? e->note : "")) return;
   for (i = 0; i < e->paths->len; i++)
      gimv_bookmark_set_note (g_ptr_array_index (e->paths, i), text);
   g_free (e->note);
   e->note = g_strdup (text);
}


static gboolean
idle_unparent (gpointer data)
{
   GtkWidget *popover = data;

   gtk_widget_unparent (popover);
   g_object_unref (popover);
   return G_SOURCE_REMOVE;
}


static void
cb_closed (GtkPopover *popover, Edit *e)
{
   save_note (e);
   /* not in its own signal */
   g_idle_add (idle_unparent, g_object_ref (popover));
}


static void
cb_activate (GtkEntry *entry, Edit *e)
{
   save_note (e);
   gtk_popover_popdown (GTK_POPOVER (e->popover));
}


static void
cb_remove (GtkButton *button, Edit *e)
{
   guint i;

   for (i = 0; i < e->paths->len; i++)
      gimv_bookmark_remove (g_ptr_array_index (e->paths, i));
   e->removed = TRUE;
   gtk_popover_popdown (GTK_POPOVER (e->popover));
}


void
gimv_bookmark_edit (GtkWidget *parent, const GdkRectangle *rect, GList *paths)
{
   GtkWidget *box, *label, *hbox, *button;
   GimvBookmark *bm = NULL;
   Edit *e;
   GList *node;
   guint added = 0;
   gchar *title;

   g_return_if_fail (GTK_IS_WIDGET (parent));
   if (!paths) return;

   e = g_new0 (Edit, 1);
   e->paths = g_ptr_array_new_with_free_func (g_free);
   for (node = paths; node; node = g_list_next (node)) {
      const gchar *path = node->data;
      if (!path || !*path) continue;
      /* the full path (an image given on the command line) */
      g_ptr_array_add (e->paths, g_canonicalize_filename (path, NULL));
   }
   if (!e->paths->len) {
      edit_free (e);
      return;
   }
   for (guint i = 0; i < e->paths->len; i++)
      if (gimv_bookmark_add (g_ptr_array_index (e->paths, i))) added++;

   if (e->paths->len == 1) {
      bm = gimv_bookmark_get (g_ptr_array_index (e->paths, 0));
      title = g_strdup (added ? _("Bookmarked") : _("Bookmark"));
   } else {
      title = g_strdup_printf (ngettext ("%'u image bookmarked", "%'u images bookmarked",
                                         e->paths->len), e->paths->len);
   }
   e->note = g_strdup (bm && bm->note ? bm->note : "");
   gimv_bookmark_free (bm);

   e->popover = gtk_popover_new ();
   gtk_widget_set_parent (e->popover, parent);
   if (rect) gtk_popover_set_pointing_to (GTK_POPOVER (e->popover), rect);
   g_object_set_data_full (G_OBJECT (e->popover), "gimv-bookmark-edit", e,
                           (GDestroyNotify) edit_free);

   box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   label = gtk_label_new (title);
   gtk_widget_add_css_class (label, "heading");
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (box), label);
   g_free (title);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   e->entry = gtk_entry_new ();
   gtk_entry_set_placeholder_text (GTK_ENTRY (e->entry), _("Note (may be left empty)"));
   gtk_editable_set_text (GTK_EDITABLE (e->entry), e->note);
   gtk_editable_set_width_chars (GTK_EDITABLE (e->entry), 32);
   gtk_widget_set_hexpand (e->entry, TRUE);
   g_signal_connect (e->entry, "activate", G_CALLBACK (cb_activate), e);
   gtk_box_append (GTK_BOX (hbox), e->entry);

   button = gtk_button_new_with_mnemonic (_("_Remove"));
   gtk_widget_set_tooltip_text (button, _("Remove the bookmark"));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_remove), e);
   gtk_box_append (GTK_BOX (hbox), button);
   gtk_box_append (GTK_BOX (box), hbox);

   gtk_popover_set_child (GTK_POPOVER (e->popover), box);
   g_signal_connect (e->popover, "closed", G_CALLBACK (cb_closed), e);
   gtk_popover_popup (GTK_POPOVER (e->popover));
   gtk_widget_grab_focus (e->entry);
}


/* the paths to bookmark of a list of GimvImageInfo: images only (not
 * folders, archives, files inside an archive) */
GList *
gimv_bookmark_paths_of_infos (GList *infos)
{
   GList *paths = NULL, *node;

   for (node = infos; node; node = g_list_next (node)) {
      GimvImageInfo *info = node->data;
      if (!info || gimv_image_info_is_dir (info) || gimv_image_info_is_archive (info)
          || (info->flags & GIMV_IMAGE_INFO_ARCHIVE_MEMBER_FLAG)
          || (info->flags & GIMV_IMAGE_INFO_MRL_FLAG))
         continue;
      paths = g_list_prepend (paths, g_strdup (gimv_image_info_get_path (info)));
   }
   return g_list_reverse (paths);
}
