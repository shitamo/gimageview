/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: editing the tags of images.
 *
 * The tags a person puts are kept in the gimv comment file of each image
 * (the line "X-IMG-Tags: a, b").  This dialog writes the comment files
 * first and then asks the indexer to read them again, so the comment file
 * stays the record and the database follows it.
 *
 * With several images the dialog shows the tags all of them have.  Tags
 * taken away there are taken away from every image, tags added are added
 * to every image, and the tags only some of them have stay as they are.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>

#include "gimv_comment.h"
#include "gimv_image_info.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_index_tags.h"

/* a line of the comment file is read into a buffer of this size */
#define LINE_MAX_BYTES 4096


/* "a, b ,, c" -> [a, b, c], without duplicates */
static GPtrArray *
split_tags (const gchar *text)
{
   GPtrArray *tags = g_ptr_array_new_with_free_func (g_free);
   gchar **v;
   gint i;

   if (!text) return tags;
   v = g_strsplit (text, ",", -1);
   for (i = 0; v[i]; i++) {
      gchar *t = g_strstrip (v[i]);
      if (*t && !g_ptr_array_find_with_equal_func (tags, t, g_str_equal, NULL))
         g_ptr_array_add (tags, g_strdup (t));
   }
   g_strfreev (v);
   return tags;
}


static gchar *
join_tags (GPtrArray *tags)
{
   GString *s = g_string_new (NULL);
   guint i;

   for (i = 0; i < tags->len; i++) {
      if (i) g_string_append (s, ", ");
      g_string_append (s, g_ptr_array_index (tags, i));
   }
   return g_string_free (s, FALSE);
}


static gboolean
has_tag (GPtrArray *tags, const gchar *tag)
{
   return g_ptr_array_find_with_equal_func (tags, tag, g_str_equal, NULL);
}


/* the tags in the comment file of the image (read from the file itself, so
 * no comment object with default values is made) */
static GPtrArray *
read_tags (const gchar *img_path)
{
   gchar *cfile = gimv_comment_find_file (img_path);
   gchar *text = NULL, **lines;
   GPtrArray *tags = NULL;
   gint i;

   if (cfile && g_file_get_contents (cfile, &text, NULL, NULL)) {
      lines = g_strsplit (text, "\n", -1);
      for (i = 0; lines[i] && *lines[i]; i++) {
         if (g_str_has_prefix (lines[i], GIMV_INDEX_TAGS_KEY ":")) {
            /* in the charset of the comment files (ISO-2022-JP, ...) */
            gchar *line = gimv_comment_text_to_internal (lines[i]);
            if (line && g_str_has_prefix (line, GIMV_INDEX_TAGS_KEY ":"))
               tags = split_tags (line + strlen (GIMV_INDEX_TAGS_KEY ":"));
            g_free (line);
            break;
         }
      }
      g_strfreev (lines);
      g_free (text);
   }
   g_free (cfile);
   return tags ? tags : g_ptr_array_new_with_free_func (g_free);
}


/******************************************************************************
 *  dialog
 ******************************************************************************/

typedef struct {
   GtkWidget *window;
   GtkWidget *entry;
   GtkWidget *error;
   GList     *infos;      /* GimvImageInfo, referenced */
   GPtrArray *common;     /* tags every image has */
} TagsDialog;


static void
tags_dialog_free (TagsDialog *td)
{
   g_list_free_full (td->infos, (GDestroyNotify) gimv_image_info_unref);
   g_ptr_array_unref (td->common);
   g_free (td);
}


static gboolean
apply_tags (TagsDialog *td)
{
   GPtrArray *wanted = split_tags (gtk_editable_get_text (GTK_EDITABLE (td->entry)));
   GList *node;
   guint i;

   /* check the length of every line before writing anything */
   for (node = td->infos; node; node = g_list_next (node)) {
      GimvImageInfo *info = node->data;
      GPtrArray *tags = read_tags (gimv_image_info_get_path (info));
      GPtrArray *result = g_ptr_array_new_with_free_func (g_free);
      gchar *value;
      gsize len;

      for (i = 0; i < tags->len; i++) {
         const gchar *t = g_ptr_array_index (tags, i);
         /* taken away: every image had it and it is not wanted now */
         if (has_tag (td->common, t) && !has_tag (wanted, t)) continue;
         g_ptr_array_add (result, g_strdup (t));
      }
      for (i = 0; i < wanted->len; i++) {
         const gchar *t = g_ptr_array_index (wanted, i);
         if (!has_tag (result, t))
            g_ptr_array_add (result, g_strdup (t));
      }

      value = join_tags (result);
      len = strlen (GIMV_INDEX_TAGS_KEY) + 2 + strlen (value) + 1;
      g_ptr_array_unref (tags);
      g_ptr_array_unref (result);

      if (len >= LINE_MAX_BYTES) {
         gtk_label_set_text (GTK_LABEL (td->error),
                             _("Too many tags: a comment line holds up to 4096 bytes."));
         gtk_widget_set_visible (td->error, TRUE);
         g_free (value);
         g_ptr_array_unref (wanted);
         return FALSE;
      }
      g_object_set_data_full (G_OBJECT (td->window),
                              gimv_image_info_get_path (info), value, g_free);
   }

   /* the comment files first, then the index */
   for (node = td->infos; node; node = g_list_next (node)) {
      GimvImageInfo *info = node->data;
      const gchar *path = gimv_image_info_get_path (info);
      const gchar *value = g_object_get_data (G_OBJECT (td->window), path);

      gimv_comment_set_value (info, GIMV_INDEX_TAGS_KEY, value);
      gimv_index_tags_changed (path);
   }

   g_ptr_array_unref (wanted);
   return TRUE;
}


static void
cb_ok (GtkWidget *button, TagsDialog *td)
{
   if (apply_tags (td))
      gtk_window_destroy (GTK_WINDOW (td->window));
}


static void
cb_cancel (GtkWidget *button, TagsDialog *td)
{
   gtk_window_destroy (GTK_WINDOW (td->window));
}


static void
cb_destroy (GtkWidget *window, TagsDialog *td)
{
   tags_dialog_free (td);
}


void
gimv_index_edit_tags (GtkWindow *parent, GList *infos)
{
   TagsDialog *td;
   GtkWidget *vbox, *label, *hbox, *button;
   GList *node;
   gchar *text;
   guint n;

   if (!infos) return;

   td = g_new0 (TagsDialog, 1);
   for (node = infos; node; node = g_list_next (node))
      td->infos = g_list_append (td->infos, gimv_image_info_ref (node->data));
   n = g_list_length (td->infos);

   /* the tags every image has */
   td->common = read_tags (gimv_image_info_get_path (td->infos->data));
   for (node = td->infos->next; node && td->common->len; node = g_list_next (node)) {
      GPtrArray *tags = read_tags (gimv_image_info_get_path (node->data));
      guint i = 0;
      while (i < td->common->len) {
         if (has_tag (tags, g_ptr_array_index (td->common, i)))
            i++;
         else
            g_ptr_array_remove_index (td->common, i);
      }
      g_ptr_array_unref (tags);
   }

   td->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (td->window), _("Edit Tags"));
   gtk_window_set_modal (GTK_WINDOW (td->window), TRUE);
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (td->window), parent);
   gtk_window_set_default_size (GTK_WINDOW (td->window), 420, -1);
   g_signal_connect (td->window, "destroy", G_CALLBACK (cb_destroy), td);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (td->window), vbox);

   if (n == 1) {
      gchar *name = g_path_get_basename (gimv_image_info_get_path (td->infos->data));
      text = g_strdup_printf (_("Tags of %s (separated by commas):"), name);
      g_free (name);
   } else {
      text = g_strdup_printf (_("Tags of %u images (separated by commas):"), n);
   }
   label = gtk_label_new (text);
   g_free (text);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_box_append (GTK_BOX (vbox), label);

   td->entry = gtk_entry_new ();
   text = join_tags (td->common);
   gtk_editable_set_text (GTK_EDITABLE (td->entry), text);
   g_free (text);
   gtk_entry_set_placeholder_text (GTK_ENTRY (td->entry), _("e.g. travel/Kyoto, cat"));
   gtk_entry_set_activates_default (GTK_ENTRY (td->entry), TRUE);
   gtk_box_append (GTK_BOX (vbox), td->entry);

   if (n > 1) {
      label = gtk_label_new (_("Only the tags all of them have are shown. "
                               "The tags only some of them have are kept."));
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_widget_add_css_class (label, "dim-label");
      gtk_box_append (GTK_BOX (vbox), label);
   }

   td->error = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (td->error), 0.0);
   gtk_label_set_wrap (GTK_LABEL (td->error), TRUE);
   gtk_widget_add_css_class (td->error, "error");
   gtk_widget_set_visible (td->error, FALSE);
   gtk_box_append (GTK_BOX (vbox), td->error);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign (hbox, GTK_ALIGN_END);
   gtk_widget_set_margin_top (hbox, 6);
   gtk_box_append (GTK_BOX (vbox), hbox);

   button = gtk_button_new_with_mnemonic (_("_Cancel"));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_cancel), td);
   gtk_box_append (GTK_BOX (hbox), button);

   button = gtk_button_new_with_mnemonic (_("_OK"));
   gtk_widget_add_css_class (button, "suggested-action");
   g_signal_connect (button, "clicked", G_CALLBACK (cb_ok), td);
   gtk_box_append (GTK_BOX (hbox), button);
   gtk_window_set_default_widget (GTK_WINDOW (td->window), button);

   gtk_window_present (GTK_WINDOW (td->window));
   gtk_widget_grab_focus (td->entry);
}
