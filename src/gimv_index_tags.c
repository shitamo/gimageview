/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: editing the tags of images.
 *
 * The tags a person puts are kept in the gimv comment file of each image
 * (the line "X-IMG-Tags: a, b").  This dialog writes the comment files
 * first and then asks the indexer to read them again, so the comment file
 * stays the record and the database follows it.
 *
 * The names of the people ("person/NAME", written by the indexer from the
 * names given to the faces) are not in the entry: they are shown apart and
 * always kept, to be changed where the faces are named.
 *
 * The dialog is laid out as the search window: an entry to add tags
 * (with the tags in use below it to pick from), and the tags the images
 * have as chips, each with a button to take it away.  With several images
 * a tag only some of them have shows how many ("Kyoto 2/5"): a click puts
 * it on all of them, the button takes it away from all of them, and left
 * alone it stays as it is on each image.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>

#include "gimv_wrap_box.h"
#include "gimv_comment.h"
#include "gimv_image_info.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_index_tags.h"

/* a line of the comment file is read into a buffer of this size */
#define LINE_MAX_BYTES 4096

/* the names of people in the comment files */
#define NAME_TAG "person/"


static gboolean
is_name (const gchar *tag)
{
   return g_str_has_prefix (tag, NAME_TAG);
}


/* "a, b ,, c" -> [a, b, c], without duplicates */
static GPtrArray *
split_tags (const gchar *text)
{
   GPtrArray *tags = g_ptr_array_new_with_free_func (g_free);
   gchar **v;
   gint i;

   if (!text) return tags;
   /* a line break (a text pasted) is a space: one tag goes on */
   {
      gchar *one = gimv_comment_one_line (text);
      v = g_strsplit (one, ",", -1);
      g_free (one);
   }
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

/* the suggestions shown under the entry */
#define SUGGEST_MAX 12

typedef struct {
   gchar *name;
   guint  n;          /* images having it; == n_images: all of them */
} Item;

typedef struct {
   GtkWidget *window;
   GtkWidget *entry;
   GtkWidget *chips;      /* GtkFlowBox */
   GtkWidget *none_label;
   GtkWidget *suggest;    /* GtkListBox */
   GtkWidget *suggest_heading;
   GtkWidget *error;
   GList     *infos;      /* GimvImageInfo, referenced */
   guint      n_images;
   GPtrArray *items;      /* Item: the tags now (not the names) */
   GPtrArray *known;      /* the tags in use (the index), most used first */
} TagsDialog;


static void
item_free (Item *it)
{
   g_free (it->name);
   g_free (it);
}


static Item *
find_item (TagsDialog *td, const gchar *name)
{
   guint i;

   for (i = 0; i < td->items->len; i++) {
      Item *it = g_ptr_array_index (td->items, i);
      if (!strcmp (it->name, name)) return it;
   }
   return NULL;
}


static void
tags_dialog_free (TagsDialog *td)
{
   g_list_free_full (td->infos, (GDestroyNotify) gimv_image_info_unref);
   g_ptr_array_unref (td->items);
   g_ptr_array_unref (td->known);
   g_free (td);
}


static void rebuild (TagsDialog *td);


static void
show_error (TagsDialog *td, const gchar *text)
{
   gtk_label_set_text (GTK_LABEL (td->error), text ? text : "");
   gtk_widget_set_visible (td->error, text != NULL);
}


/* the tags typed ("a, b"): onto every image.  FALSE (nothing added) for a
 * name of a person */
static gboolean
add_typed (TagsDialog *td, const gchar *text)
{
   GPtrArray *tags = split_tags (text);
   guint i;

   for (i = 0; i < tags->len; i++) {
      if (is_name (g_ptr_array_index (tags, i))) {
         show_error (td, _("The names of people (person/...) are given to the faces, "
                           "in the faces window or the faces panel (P) of the image."));
         g_ptr_array_unref (tags);
         return FALSE;
      }
   }
   for (i = 0; i < tags->len; i++) {
      const gchar *t = g_ptr_array_index (tags, i);
      Item *it = find_item (td, t);
      if (!it) {
         it = g_new0 (Item, 1);
         it->name = g_strdup (t);
         g_ptr_array_add (td->items, it);
      }
      it->n = td->n_images;
   }
   g_ptr_array_unref (tags);
   show_error (td, NULL);
   return TRUE;
}


static gboolean
apply_tags (TagsDialog *td)
{
   GList *node;
   guint i;

   /* typed and not added yet */
   {
      const gchar *text = gtk_editable_get_text (GTK_EDITABLE (td->entry));
      if (text && *text && !add_typed (td, text)) return FALSE;
   }

   /* check the length of every line before writing anything */
   for (node = td->infos; node; node = g_list_next (node)) {
      GimvImageInfo *info = node->data;
      GPtrArray *tags = read_tags (gimv_image_info_get_path (info));
      GPtrArray *result = g_ptr_array_new_with_free_func (g_free);
      gchar *value;
      gsize len;

      for (i = 0; i < tags->len; i++) {
         const gchar *t = g_ptr_array_index (tags, i);
         /* the names stay; a tag stays while it is in the dialog */
         if (is_name (t) || find_item (td, t))
            g_ptr_array_add (result, g_strdup (t));
      }
      for (i = 0; i < td->items->len; i++) {
         Item *it = g_ptr_array_index (td->items, i);
         if (it->n == td->n_images && !has_tag (result, it->name))
            g_ptr_array_add (result, g_strdup (it->name));
      }

      value = join_tags (result);
      len = strlen (GIMV_INDEX_TAGS_KEY) + 2 + strlen (value) + 1;
      g_ptr_array_unref (tags);
      g_ptr_array_unref (result);

      if (len >= LINE_MAX_BYTES) {
         show_error (td, _("Too many tags: a comment line holds up to 4096 bytes."));
         g_free (value);
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
   return TRUE;
}


static void
cb_chip_remove (GtkButton *button, TagsDialog *td)
{
   const gchar *name = g_object_get_data (G_OBJECT (button), "tag");
   Item *it = name ? find_item (td, name) : NULL;

   if (it) g_ptr_array_remove (td->items, it);
   rebuild (td);
}


/* a tag only some of the images have: onto all of them */
static void
cb_chip_all (GtkButton *button, TagsDialog *td)
{
   const gchar *name = g_object_get_data (G_OBJECT (button), "tag");
   Item *it = name ? find_item (td, name) : NULL;

   if (it) it->n = td->n_images;
   rebuild (td);
}


static void
cb_suggest_activated (GtkListBox *list, GtkListBoxRow *row, TagsDialog *td)
{
   const gchar *name = g_object_get_data (G_OBJECT (row), "tag");

   if (!name) return;
   add_typed (td, name);
   gtk_editable_set_text (GTK_EDITABLE (td->entry), "");
   rebuild (td);
   gtk_widget_grab_focus (td->entry);
}


static void
rebuild_chips (TagsDialog *td)
{
   guint i;

   gimv_wrap_box_clear (GIMV_WRAP_BOX (td->chips));
   gtk_widget_set_visible (td->none_label, td->items->len == 0);

   for (i = 0; i < td->items->len; i++) {
      Item *it = g_ptr_array_index (td->items, i);
      gboolean all = it->n == td->n_images;
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *label_button = gtk_button_new ();
      GtkWidget *x = gtk_button_new_from_icon_name ("window-close-symbolic");
      gchar *text = all ? g_strdup (it->name)
                        : g_strdup_printf ("%s %u/%u", it->name, it->n, td->n_images);
      GtkWidget *label = gtk_label_new (text);

      gtk_widget_add_css_class (box, "linked");
      gtk_button_set_child (GTK_BUTTON (label_button), label);
      g_object_set_data_full (G_OBJECT (label_button), "tag", g_strdup (it->name), g_free);
      if (all) {
         gtk_widget_set_can_target (label_button, FALSE);
      } else {
         gtk_widget_add_css_class (label, "dim-label");
         gtk_widget_set_tooltip_text (label_button,
                                      _("Only some of the images have it: click to put it on all of them"));
         g_signal_connect (label_button, "clicked", G_CALLBACK (cb_chip_all), td);
      }
      gtk_widget_set_tooltip_text (x, all ? _("Take the tag away")
                                          : _("Take the tag away from all of them"));
      g_object_set_data_full (G_OBJECT (x), "tag", g_strdup (it->name), g_free);
      g_signal_connect (x, "clicked", G_CALLBACK (cb_chip_remove), td);
      gtk_box_append (GTK_BOX (box), label_button);
      gtk_box_append (GTK_BOX (box), x);
      gimv_wrap_box_append (GIMV_WRAP_BOX (td->chips), box);
      g_free (text);
   }
}


/* the tags in use matching what is typed (the most used first; all of
 * them once typed), not those on every image already */
static void
rebuild_suggest (TagsDialog *td)
{
   const gchar *typed = gtk_editable_get_text (GTK_EDITABLE (td->entry));
   gchar *last = NULL, *fold = NULL;
   GtkWidget *child;
   guint i, shown = 0;

   /* the part after the last comma */
   if (typed && *typed) {
      const gchar *p = strrchr (typed, ',');
      last = g_strstrip (g_strdup (p ? p + 1 : typed));
      if (*last) fold = g_utf8_casefold (last, -1);
   }

   while ((child = gtk_widget_get_first_child (td->suggest)))
      gtk_list_box_remove (GTK_LIST_BOX (td->suggest), child);

   for (i = 0; i < td->known->len && shown < SUGGEST_MAX; i++) {
      Item *k = g_ptr_array_index (td->known, i);
      Item *it = find_item (td, k->name);
      GtkWidget *label, *row;
      gchar *text;

      if (it && it->n == td->n_images) continue;
      if (fold) {
         gchar *kf = g_utf8_casefold (k->name, -1);
         gboolean match = strstr (kf, fold) != NULL;
         g_free (kf);
         if (!match) continue;
      }
      text = g_strdup_printf ("%s (%'u)", k->name, k->n);
      label = gtk_label_new (text);
      g_free (text);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_margin_start (label, 8);
      gtk_widget_set_margin_top (label, 3);
      gtk_widget_set_margin_bottom (label, 3);
      gtk_list_box_append (GTK_LIST_BOX (td->suggest), label);
      row = gtk_widget_get_parent (label);
      g_object_set_data_full (G_OBJECT (row), "tag", g_strdup (k->name), g_free);
      shown++;
   }
   gtk_widget_set_visible (td->suggest, shown > 0);
   gtk_widget_set_visible (td->suggest_heading, shown > 0);
   /* not squeezed to a row when the tags above take more lines (the
      window keeps its height): a few rows at least, the window grows */
   {
      GtkWidget *scroll = gtk_widget_get_ancestor (td->suggest, GTK_TYPE_SCROLLED_WINDOW);
      gint nat = 0;
      if (scroll) {
         if (shown) gtk_widget_measure (td->suggest, GTK_ORIENTATION_VERTICAL, -1,
                                        NULL, &nat, NULL, NULL);
         gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroll),
                                                     shown ? MIN (nat, 125) : -1);
      }
   }
   g_free (fold);
   g_free (last);
}


static void
rebuild (TagsDialog *td)
{
   rebuild_chips (td);
   rebuild_suggest (td);
}


static void
cb_entry_changed (GtkEditable *editable, TagsDialog *td)
{
   rebuild_suggest (td);
}


/* Enter: the tags typed go into the chips; with nothing typed, OK */
static void
cb_entry_activate (GtkEntry *entry, TagsDialog *td)
{
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry));

   if (text && *text) {
      if (add_typed (td, text)) {
         gtk_editable_set_text (GTK_EDITABLE (entry), "");
         rebuild (td);
      }
      return;
   }
   if (apply_tags (td))
      gtk_window_destroy (GTK_WINDOW (td->window));
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


static gboolean
cb_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
        TagsDialog *td)
{
   if (keyval == GDK_KEY_Escape) {
      gtk_window_destroy (GTK_WINDOW (td->window));
      return TRUE;
   }
   return FALSE;
}


/* the tags put by people in the index, the most used first */
static GPtrArray *
known_tags (void)
{
   GPtrArray *known = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
#ifdef ENABLE_INDEX
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;

   if (db && sqlite3_prepare_v2 (db,
                                 "SELECT t.name, count(*) AS n FROM file_tags ft "
                                 "JOIN tags t ON t.id = ft.tag_id WHERE ft.auto = 0 "
                                 "AND NOT (t.name >= 'person/' AND t.name < 'person0') "
                                 "GROUP BY t.id ORDER BY n DESC, t.name", -1, &st, NULL) == SQLITE_OK)
   {
      while (sqlite3_step (st) == SQLITE_ROW) {
         Item *k = g_new0 (Item, 1);
         k->name = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
         k->n = sqlite3_column_int (st, 1);
         g_ptr_array_add (known, k);
      }
      sqlite3_finalize (st);
   }
#endif
   return known;
}


static GtkWidget *
heading (const gchar *text)
{
   GtkWidget *label = gtk_label_new (text);

   gtk_widget_add_css_class (label, "heading");
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_set_margin_top (label, 6);
   return label;
}


void
gimv_index_edit_tags (GtkWindow *parent, GList *infos)
{
   TagsDialog *td;
   GtkWidget *vbox, *label, *hbox, *button, *scroll;
   GPtrArray *names;
   GList *node;
   gchar *text;
   guint i;

   if (!infos) return;

   td = g_new0 (TagsDialog, 1);
   for (node = infos; node; node = g_list_next (node))
      td->infos = g_list_append (td->infos, gimv_image_info_ref (node->data));
   td->n_images = g_list_length (td->infos);
   td->items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
   td->known = known_tags ();

   /* the tags of the images, with how many have each; the names apart */
   names = g_ptr_array_new_with_free_func (g_free);
   for (node = td->infos; node; node = g_list_next (node)) {
      GPtrArray *tags = read_tags (gimv_image_info_get_path (node->data));
      for (i = 0; i < tags->len; i++) {
         const gchar *t = g_ptr_array_index (tags, i);
         if (is_name (t)) {
            if (t[strlen (NAME_TAG)] && !has_tag (names, t + strlen (NAME_TAG)))
               g_ptr_array_add (names, g_strdup (t + strlen (NAME_TAG)));
         } else {
            Item *it = find_item (td, t);
            if (!it) {
               it = g_new0 (Item, 1);
               it->name = g_strdup (t);
               g_ptr_array_add (td->items, it);
            }
            it->n++;
         }
      }
      g_ptr_array_unref (tags);
   }

   td->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (td->window), _("Edit Tags"));
   gtk_window_set_modal (GTK_WINDOW (td->window), TRUE);
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (td->window), parent);
   gtk_window_set_default_size (GTK_WINDOW (td->window), 440, -1);
   g_signal_connect (td->window, "destroy", G_CALLBACK (cb_destroy), td);
   {
      GtkEventController *keys = gtk_event_controller_key_new ();
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_key), td);
      gtk_widget_add_controller (td->window, keys);
   }

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (td->window), vbox);

   if (td->n_images == 1) {
      gchar *name = g_path_get_basename (gimv_image_info_get_path (td->infos->data));
      text = g_strdup_printf (_("Tags of %s"), name);
      g_free (name);
   } else {
      text = g_strdup_printf (_("Tags of %u images"), td->n_images);
   }
   label = gtk_label_new (text);
   g_free (text);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_box_append (GTK_BOX (vbox), label);

   /* adding */
   td->entry = gtk_entry_new ();
   gtk_entry_set_placeholder_text (GTK_ENTRY (td->entry),
                                   _("A tag to add, Enter (several: separated by commas)"));
   g_signal_connect (td->entry, "changed", G_CALLBACK (cb_entry_changed), td);
   g_signal_connect (td->entry, "activate", G_CALLBACK (cb_entry_activate), td);
   gtk_box_append (GTK_BOX (vbox), td->entry);

   /* the tags now */
   gtk_box_append (GTK_BOX (vbox), heading (_("Tags")));
   td->chips = gimv_wrap_box_new (6, 6);
   gtk_box_append (GTK_BOX (vbox), td->chips);
   td->none_label = gtk_label_new (_("No tags"));
   gtk_widget_add_css_class (td->none_label, "dim-label");
   gtk_label_set_xalign (GTK_LABEL (td->none_label), 0.0);
   gtk_box_append (GTK_BOX (vbox), td->none_label);

   /* the names: shown, not taken away here */
   if (names->len) {
      GtkWidget *flow = gimv_wrap_box_new (12, 4);

      gtk_box_append (GTK_BOX (vbox), heading (_("People")));
      for (i = 0; i < names->len; i++) {
         GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
         gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("avatar-default-symbolic"));
         gtk_box_append (GTK_BOX (box), gtk_label_new (g_ptr_array_index (names, i)));
         gimv_wrap_box_append (GIMV_WRAP_BOX (flow), box);
      }
      gtk_box_append (GTK_BOX (vbox), flow);
      label = gtk_label_new (_("Names are given to the faces (the faces window, or the "
                               "faces panel (P) of the image); they are kept here."));
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_widget_add_css_class (label, "dim-label");
      gtk_box_append (GTK_BOX (vbox), label);
   }
   g_ptr_array_unref (names);

   /* the tags in use, to pick from */
   td->suggest = gtk_list_box_new ();
   gtk_list_box_set_selection_mode (GTK_LIST_BOX (td->suggest), GTK_SELECTION_NONE);
   gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (td->suggest), TRUE);
   gtk_widget_add_css_class (td->suggest, "boxed-list");
   gtk_widget_set_tooltip_text (td->suggest, _("Tags in use: click to add"));
   g_signal_connect (td->suggest, "row-activated", G_CALLBACK (cb_suggest_activated), td);
   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 200);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), td->suggest);
   td->suggest_heading = heading (_("Tags in Use"));
   gtk_box_append (GTK_BOX (vbox), td->suggest_heading);
   gtk_box_append (GTK_BOX (vbox), scroll);

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

   rebuild (td);
   gtk_window_present (GTK_WINDOW (td->window));
   gtk_widget_grab_focus (td->entry);
}
