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
#include <sys/stat.h>

#include "gimv_wrap_box.h"
#include "gimv_comment.h"
#include "gimv_image.h"
#include "gimv_image_info.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_index_tags.h"
#include "prefs.h"

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


/* the tags of the image from added to those of the image to (a copy of it
 * deleted); TRUE: to has new tags */
gboolean
gimv_index_tags_merge (const gchar *from, const gchar *to)
{
   GPtrArray *a, *b;
   gboolean changed = FALSE;
   guint i;

   g_return_val_if_fail (from && to, FALSE);
   a = read_tags (to);
   b = read_tags (from);
   for (i = 0; i < b->len; i++) {
      const gchar *t = g_ptr_array_index (b, i);
      if (!has_tag (a, t)) {
         g_ptr_array_add (a, g_strdup (t));
         changed = TRUE;
      }
   }
   if (changed) {
      GimvImageInfo *info = gimv_image_info_get (to);
      if (info) {
         gchar *value = join_tags (a);
         gimv_comment_set_value (info, GIMV_INDEX_TAGS_KEY, value);
         gimv_index_tags_changed (to);
         g_free (value);
         gimv_image_info_unref (info);
      } else {
         changed = FALSE;
      }
   }
   g_ptr_array_unref (a);
   g_ptr_array_unref (b);
   return changed;
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
   GtkWidget *content;    /* what is edited: insensitive while writing */
   GtkWidget *progress;   /* GtkProgressBar, while writing */
   GtkWidget *cancel;     /* Cancel; Stop while writing; Close after a stop */
   GtkWidget *ok;
   GPtrArray *paths;      /* the images */
   guint      n_images;
   /* writing: in steps on the main loop, not to stop the screen */
   guint      apply_id;
   guint      apply_pos;
   gboolean   writing;    /* FALSE: still checking (nothing written yet) */
   gboolean   stop;
   GPtrArray *values;     /* the new line of each image; NULL: as it is */
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
   if (td->apply_id) g_source_remove (td->apply_id);
   g_ptr_array_unref (td->paths);
   if (td->values) g_ptr_array_unref (td->values);
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


/* the new tags line of the image at path ("" for none); NULL when it is
 * as it was */
static gchar *
new_value (TagsDialog *td, const gchar *path)
{
   GPtrArray *tags = read_tags (path);
   GPtrArray *result = g_ptr_array_new_with_free_func (g_free);
   gchar *old = join_tags (tags), *value;
   guint i;

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
   g_ptr_array_unref (tags);
   g_ptr_array_unref (result);
   if (!strcmp (old, value)) {
      g_free (value);
      value = NULL;
   }
   g_free (old);
   return value;
}


static void
set_busy (TagsDialog *td, gboolean busy)
{
   gtk_widget_set_sensitive (td->content, !busy);
   gtk_widget_set_sensitive (td->ok, !busy);
   gtk_widget_set_visible (td->progress, busy);
   gtk_button_set_label (GTK_BUTTON (td->cancel), busy ? _("_Stop") : _("_Cancel"));
}


static void
apply_progress (TagsDialog *td)
{
   gchar *text = g_strdup_printf (td->writing ? _("Writing: %'u / %'u")
                                              : _("Checking: %'u / %'u"),
                                  td->apply_pos, td->n_images);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (td->progress), text);
   gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (td->progress),
                                  td->n_images ? (gdouble) td->apply_pos / td->n_images : 1.0);
   g_free (text);
}


/* first every new line is made and checked (nothing written when one is
 * too long), then the comment files are written, and the index told; a
 * few at a time */
static gboolean
idle_apply (gpointer data)
{
   TagsDialog *td = data;
   gint64 until = g_get_monotonic_time () + 30 * 1000;

   while (td->apply_pos < td->n_images && g_get_monotonic_time () < until) {
      const gchar *path = g_ptr_array_index (td->paths, td->apply_pos);

      if (td->stop) break;
      if (!td->writing) {
         gchar *value = new_value (td, path);
         if (value && strlen (GIMV_INDEX_TAGS_KEY) + 2 + strlen (value) + 1 >= LINE_MAX_BYTES) {
            gchar *name = g_path_get_basename (path), *msg;
            msg = g_strdup_printf (_("Too many tags for %s: a comment line holds up to 4096 bytes."),
                                   name);
            show_error (td, msg);
            g_free (msg);
            g_free (name);
            g_free (value);
            td->apply_id = 0;
            set_busy (td, FALSE);
            return G_SOURCE_REMOVE;
         }
         g_ptr_array_add (td->values, value);
      } else {
         const gchar *value = g_ptr_array_index (td->values, td->apply_pos);
         if (value) {
            GimvImageInfo *info = gimv_image_info_get (path);
            if (info) {
               gimv_comment_set_value (info, GIMV_INDEX_TAGS_KEY, value);
               gimv_index_tags_changed (path);
               gimv_image_info_unref (info);
            }
         }
      }
      td->apply_pos++;
   }

   if (td->stop) {
      td->apply_id = 0;
      if (!td->writing) {
         /* nothing written yet: back to editing */
         set_busy (td, FALSE);
      } else {
         gchar *msg = g_strdup_printf (_("Stopped: %'u of %'u images done, the others "
                                         "are as they were."),
                                       td->apply_pos, td->n_images);
         show_error (td, msg);
         g_free (msg);
         gtk_widget_set_visible (td->progress, FALSE);
         gtk_button_set_label (GTK_BUTTON (td->cancel), _("_Close"));
      }
      return G_SOURCE_REMOVE;
   }
   if (td->apply_pos < td->n_images) {
      apply_progress (td);
      return G_SOURCE_CONTINUE;
   }
   if (!td->writing) {
      td->writing = TRUE;
      td->apply_pos = 0;
      apply_progress (td);
      return G_SOURCE_CONTINUE;
   }
   td->apply_id = 0;
   gtk_window_destroy (GTK_WINDOW (td->window));
   return G_SOURCE_REMOVE;
}


static void
apply_tags (TagsDialog *td)
{
   /* typed and not added yet */
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (td->entry));

   if (td->apply_id) return;
   if (text && *text && !add_typed (td, text)) return;
   show_error (td, NULL);
   if (td->values) g_ptr_array_unref (td->values);
   td->values = g_ptr_array_new_with_free_func (g_free);
   td->apply_pos = 0;
   td->writing = FALSE;
   td->stop = FALSE;
   set_busy (td, TRUE);
   apply_progress (td);
   td->apply_id = g_idle_add (idle_apply, td);
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


/* "Travel/Kyoto/Temple": the parents dim, the last part as it is
 * (markup) */
static gchar *
tag_markup (const gchar *name)
{
   const gchar *slash = strrchr (name, '/');
   gchar *up, *ret;

   if (!slash || slash == name || !slash[1]) return g_markup_escape_text (name, -1);
   up = g_strndup (name, slash + 1 - name);
   ret = g_markup_printf_escaped ("<span alpha=\"55%%\">%s</span>%s", up, slash + 1);
   g_free (up);
   return ret;
}


/* a parent of a tag in use: what is typed goes up to it ("Travel/"),
 * the tags below it shown to pick from */
static void
cb_suggest_parent (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                   TagsDialog *td)
{
   GtkWidget *part = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
   const gchar *upto = g_object_get_data (G_OBJECT (part), "upto");
   const gchar *typed = gtk_editable_get_text (GTK_EDITABLE (td->entry));
   const gchar *comma = typed ? strrchr (typed, ',') : NULL;
   gchar *text;

   /* the row is not activated (the tag not added) */
   gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
   if (!upto) return;
   /* the tags typed before the last comma stay */
   if (comma) {
      gchar *before = g_strndup (typed, comma + 1 - typed);
      text = g_strdup_printf ("%s %s/", before, upto);
      g_free (before);
   } else {
      text = g_strdup_printf ("%s/", upto);
   }
   gtk_editable_set_text (GTK_EDITABLE (td->entry), text);
   /* to go on typing the child's name after it */
   gtk_entry_grab_focus_without_selecting (GTK_ENTRY (td->entry));
   gtk_editable_set_position (GTK_EDITABLE (td->entry), -1);
   g_free (text);
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
      GtkWidget *label = gtk_label_new (NULL);
      gchar *text = tag_markup (it->name);

      if (!all) {
         gchar *t = g_strdup_printf ("%s %u/%u", text, it->n, td->n_images);
         g_free (text);
         text = t;
      }
      gtk_label_set_markup (GTK_LABEL (label), text);

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
         /* "Travel/": the tags below it; otherwise anywhere in the name
          * ("Kyoto" finds "Travel/Kyoto") */
         gboolean match = strchr (fold, '/') ? g_str_has_prefix (kf, fold)
                                             : strstr (kf, fold) != NULL;
         g_free (kf);
         if (!match) continue;
      }
      {
         /* "Travel / Kyoto / Temple (3)": the parents are buttons that
          * type up to them, the rest of the row adds the tag */
         GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
         gchar **parts = g_strsplit (k->name, "/", -1);
         guint np = g_strv_length (parts), j;
         GString *upto = g_string_new (NULL);

         gtk_widget_set_margin_start (box, 8);
         for (j = 0; j + 1 < np; j++) {
            GtkWidget *b, *sep;
            GtkGesture *click;
            if (j > 0) g_string_append_c (upto, '/');
            g_string_append (upto, parts[j]);
            if (!*parts[j]) continue;
            b = gtk_label_new (parts[j]);
            gtk_widget_add_css_class (b, "dim-label");
            gtk_widget_set_cursor_from_name (b, "pointer");
            {
               gchar *tip = g_strdup_printf (_("Click to pick from the tags below %s"),
                                             upto->str);
               gtk_widget_set_tooltip_text (b, tip);
               g_free (tip);
            }
            g_object_set_data_full (G_OBJECT (b), "upto", g_strdup (upto->str), g_free);
            click = gtk_gesture_click_new ();
            gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
            g_signal_connect (click, "pressed", G_CALLBACK (cb_suggest_parent), td);
            gtk_widget_add_controller (b, GTK_EVENT_CONTROLLER (click));
            gtk_box_append (GTK_BOX (box), b);
            sep = gtk_label_new ("/");
            gtk_widget_add_css_class (sep, "dim-label");
            gtk_widget_set_margin_start (sep, 2);
            gtk_widget_set_margin_end (sep, 2);
            gtk_box_append (GTK_BOX (box), sep);
         }
         text = g_strdup_printf ("%s (%'u)", np ? parts[np - 1] : k->name, k->n);
         label = gtk_label_new (text);
         g_free (text);
         gtk_label_set_xalign (GTK_LABEL (label), 0.0);
         gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
         gtk_widget_set_hexpand (label, TRUE);
         gtk_widget_set_margin_top (label, 3);
         gtk_widget_set_margin_bottom (label, 3);
         gtk_box_append (GTK_BOX (box), label);
         g_string_free (upto, TRUE);
         g_strfreev (parts);
         gtk_list_box_append (GTK_LIST_BOX (td->suggest), box);
         row = gtk_widget_get_parent (box);
      }
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
   apply_tags (td);
}


static void
cb_ok (GtkWidget *button, TagsDialog *td)
{
   apply_tags (td);
}


static void
cb_cancel (GtkWidget *button, TagsDialog *td)
{
   /* while writing: stop after the image being written */
   if (td->apply_id) {
      td->stop = TRUE;
      return;
   }
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
      cb_cancel (NULL, td);
      return TRUE;
   }
   return FALSE;
}


/* the tags put by people in the index with the keywords of other programs
 * (the same name, each image once), the most used first */
static GPtrArray *
known_tags (void)
{
   GPtrArray *known = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
#ifdef ENABLE_INDEX
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;

   if (db && sqlite3_prepare_v2 (db,
                                 "SELECT name, count(DISTINCT id) AS n FROM ("
                                 " SELECT t.name AS name, ft.file_id AS id FROM file_tags ft"
                                 "  JOIN tags t ON t.id = ft.tag_id WHERE ft.auto = 0"
                                 "  AND NOT (t.name >= 'person/' AND t.name < 'person0')"
                                 " UNION ALL SELECT substr (t.name, 9), ft.file_id FROM file_tags ft"
                                 "  JOIN tags t ON t.id = ft.tag_id WHERE ft.auto = 1"
                                 "  AND t.name >= 'keyword/' AND t.name < 'keyword0')"
                                 " GROUP BY name ORDER BY n DESC, name", -1, &st, NULL) == SQLITE_OK)
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


/* the tags of the images (and the names apart), with how many have each;
 * in a worker thread for a folder */
typedef struct {
   GPtrArray *paths;      /* gchar* */
   GPtrArray *items;      /* Item */
   GPtrArray *names;      /* gchar* */
} Scan;


static void
scan_free (Scan *sc)
{
   if (sc->paths) g_ptr_array_unref (sc->paths);
   if (sc->items) g_ptr_array_unref (sc->items);
   if (sc->names) g_ptr_array_unref (sc->names);
   g_free (sc);
}


static Scan *
scan_new (void)
{
   Scan *sc = g_new0 (Scan, 1);
   sc->paths = g_ptr_array_new_with_free_func (g_free);
   sc->items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
   sc->names = g_ptr_array_new_with_free_func (g_free);
   return sc;
}


static void
scan_add (Scan *sc, GHashTable *items, const gchar *path)
{
   GPtrArray *tags = read_tags (path);
   guint i;

   g_ptr_array_add (sc->paths, g_strdup (path));
   for (i = 0; i < tags->len; i++) {
      const gchar *t = g_ptr_array_index (tags, i);
      if (is_name (t)) {
         if (t[strlen (NAME_TAG)] && !has_tag (sc->names, t + strlen (NAME_TAG)))
            g_ptr_array_add (sc->names, g_strdup (t + strlen (NAME_TAG)));
      } else {
         Item *it = g_hash_table_lookup (items, t);
         if (!it) {
            it = g_new0 (Item, 1);
            it->name = g_strdup (t);
            g_ptr_array_add (sc->items, it);
            g_hash_table_insert (items, it->name, it);
         }
         it->n++;
      }
   }
   g_ptr_array_unref (tags);
}


static void dialog_open (GtkWindow *parent, Scan *sc, const gchar *title);


/* the selected images */
void
gimv_index_edit_tags (GtkWindow *parent, GList *infos)
{
   Scan *sc;
   GHashTable *items;
   GList *node;
   gchar *title;

   if (!infos) return;
   sc = scan_new ();
   items = g_hash_table_new (g_str_hash, g_str_equal);
   for (node = infos; node; node = g_list_next (node))
      scan_add (sc, items, gimv_image_info_get_path (node->data));
   g_hash_table_unref (items);

   if (sc->paths->len == 1) {
      gchar *name = g_path_get_basename (g_ptr_array_index (sc->paths, 0));
      title = g_strdup_printf (_("Tags of %s"), name);
      g_free (name);
   } else {
      title = g_strdup_printf (_("Tags of %u images"), sc->paths->len);
   }
   dialog_open (parent, sc, title);
   g_free (title);
}


/******************************************************************************
 *  a folder and the folders in it
 ******************************************************************************/

typedef struct {
   GtkWindow    *parent;
   gchar        *dir;
   GCancellable *cancel;
   gint          found;      /* atomic: images found so far */
   gboolean      read_dot, follow_link;
   GtkWidget    *window;     /* "Looking for the images..." */
   GtkWidget    *label;
   guint         show_id, tick_id;
} DirScan;


static void
dir_scan_free (DirScan *ds)
{
   if (ds->show_id) g_source_remove (ds->show_id);
   if (ds->tick_id) g_source_remove (ds->tick_id);
   if (ds->window) gtk_window_destroy (GTK_WINDOW (ds->window));
   if (ds->parent) g_object_unref (ds->parent);
   g_object_unref (ds->cancel);
   g_free (ds->dir);
   g_free (ds);
}


static gint
cmp_path (gconstpointer a, gconstpointer b)
{
   return g_utf8_collate (*(const gchar * const *) a, *(const gchar * const *) b);
}


static void
walk (DirScan *ds, const gchar *dir, GPtrArray *files, GHashTable *seen)
{
   GDir *d = g_dir_open (dir, 0, NULL);
   const gchar *name;
   GPtrArray *dirs;
   struct stat st;
   guint i;

   if (!d) return;
   /* a folder once (links going round) */
   if (stat (dir, &st) == 0) {
      gchar *id = g_strdup_printf ("%lu:%lu", (gulong) st.st_dev, (gulong) st.st_ino);
      if (g_hash_table_contains (seen, id)) {
         g_free (id);
         g_dir_close (d);
         return;
      }
      g_hash_table_add (seen, id);
   }
   dirs = g_ptr_array_new_with_free_func (g_free);
   while ((name = g_dir_read_name (d))) {
      gchar *path;
      if (g_cancellable_is_cancelled (ds->cancel)) break;
      if (name[0] == '.' && !ds->read_dot) continue;
      path = g_build_filename (dir, name, NULL);
      if (lstat (path, &st) != 0) {
         g_free (path);
         continue;
      }
      if (S_ISLNK (st.st_mode) && stat (path, &st) != 0) {
         g_free (path);
         continue;
      }
      if (S_ISDIR (st.st_mode)) {
         gboolean link = FALSE;
         struct stat lst;
         if (lstat (path, &lst) == 0) link = S_ISLNK (lst.st_mode);
         if (!link || ds->follow_link) g_ptr_array_add (dirs, path);
         else g_free (path);
      } else if (S_ISREG (st.st_mode) && gimv_image_detect_type_by_ext (path)) {
         g_ptr_array_add (files, path);
         g_atomic_int_inc (&ds->found);
      } else {
         g_free (path);
      }
   }
   g_dir_close (d);
   g_ptr_array_sort (dirs, cmp_path);
   for (i = 0; i < dirs->len && !g_cancellable_is_cancelled (ds->cancel); i++)
      walk (ds, g_ptr_array_index (dirs, i), files, seen);
   g_ptr_array_unref (dirs);
}


static void
dir_scan_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   DirScan *ds = data;
   GPtrArray *files = g_ptr_array_new_with_free_func (g_free);
   GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   GHashTable *items = g_hash_table_new (g_str_hash, g_str_equal);
   Scan *sc = scan_new ();
   guint i;

   walk (ds, ds->dir, files, seen);
   g_ptr_array_sort (files, cmp_path);
   for (i = 0; i < files->len && !g_cancellable_is_cancelled (cancel); i++)
      scan_add (sc, items, g_ptr_array_index (files, i));
   g_hash_table_unref (items);
   g_hash_table_unref (seen);
   g_ptr_array_unref (files);
   if (g_cancellable_is_cancelled (cancel)) {
      scan_free (sc);
      g_task_return_pointer (task, NULL, NULL);
      return;
   }
   g_task_return_pointer (task, sc, (GDestroyNotify) scan_free);
}


static gboolean
dir_scan_tick (gpointer data)
{
   DirScan *ds = data;
   gchar *text = g_strdup_printf (_("Looking for the images in %s and the folders in it: %'d"),
                                  ds->dir, g_atomic_int_get (&ds->found));
   gtk_label_set_text (GTK_LABEL (ds->label), text);
   g_free (text);
   return G_SOURCE_CONTINUE;
}


static void
cb_dir_scan_cancel (GtkWidget *widget, DirScan *ds)
{
   g_cancellable_cancel (ds->cancel);
}


static gboolean
cb_dir_scan_close (GtkWindow *window, DirScan *ds)
{
   g_cancellable_cancel (ds->cancel);
   return TRUE;   /* closed when the thread is over */
}


/* shown only when the looking takes a while */
static gboolean
dir_scan_show (gpointer data)
{
   DirScan *ds = data;
   GtkWidget *vbox, *button, *spinner;

   ds->show_id = 0;
   ds->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (ds->window), _("Edit Tags"));
   gtk_window_set_modal (GTK_WINDOW (ds->window), TRUE);
   gtk_window_set_default_size (GTK_WINDOW (ds->window), 380, -1);
   if (ds->parent) gtk_window_set_transient_for (GTK_WINDOW (ds->window), ds->parent);
   g_signal_connect (ds->window, "close-request", G_CALLBACK (cb_dir_scan_close), ds);
   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (ds->window), vbox);
   spinner = gtk_spinner_new ();
   gtk_spinner_start (GTK_SPINNER (spinner));
   gtk_box_append (GTK_BOX (vbox), spinner);
   ds->label = gtk_label_new (NULL);
   gtk_label_set_wrap (GTK_LABEL (ds->label), TRUE);
   gtk_label_set_wrap_mode (GTK_LABEL (ds->label), PANGO_WRAP_WORD_CHAR);
   gtk_box_append (GTK_BOX (vbox), ds->label);
   button = gtk_button_new_with_mnemonic (_("_Cancel"));
   gtk_widget_set_halign (button, GTK_ALIGN_END);
   g_signal_connect (button, "clicked", G_CALLBACK (cb_dir_scan_cancel), ds);
   gtk_box_append (GTK_BOX (vbox), button);
   dir_scan_tick (ds);
   ds->tick_id = g_timeout_add (200, dir_scan_tick, ds);
   gtk_window_present (GTK_WINDOW (ds->window));
   return G_SOURCE_REMOVE;
}


static void
cb_dir_scanned (GObject *source, GAsyncResult *res, gpointer data)
{
   DirScan *ds = data;
   Scan *sc = g_task_propagate_pointer (G_TASK (res), NULL);

   if (sc && sc->paths->len == 0) {
      GtkAlertDialog *dialog = gtk_alert_dialog_new ("%s", _("No images in the folder."));
      gchar *detail = g_strdup_printf (_("%s and the folders in it have no images."), ds->dir);
      gtk_alert_dialog_set_detail (dialog, detail);
      gtk_alert_dialog_show (dialog, ds->parent);
      g_object_unref (dialog);
      g_free (detail);
      scan_free (sc);
   } else if (sc) {
      gchar *base = g_path_get_basename (ds->dir);
      gchar *title = g_strdup_printf (_("Tags of the %'u images in \"%s\" and the folders in it"),
                                      sc->paths->len, base);
      dialog_open (ds->parent, sc, title);
      g_free (title);
      g_free (base);
   }
   dir_scan_free (ds);
}


/* the images in dir and the folders in it */
void
gimv_index_edit_tags_dir (GtkWindow *parent, const gchar *dir)
{
   DirScan *ds;
   GTask *task;

   g_return_if_fail (dir && *dir);
   ds = g_new0 (DirScan, 1);
   ds->parent = parent ? g_object_ref (parent) : NULL;
   ds->dir = g_strdup (dir);
   ds->cancel = g_cancellable_new ();
   ds->read_dot = conf.read_dotfile;
   ds->follow_link = conf.recursive_follow_link;
   ds->show_id = g_timeout_add (300, dir_scan_show, ds);

   task = g_task_new (NULL, ds->cancel, cb_dir_scanned, ds);
   g_task_set_task_data (task, ds, NULL);
   g_task_run_in_thread (task, dir_scan_thread);
   g_object_unref (task);
}


static gboolean
cb_close_request (GtkWindow *window, TagsDialog *td)
{
   /* while writing: stopped, not closed half way */
   if (td->apply_id) {
      td->stop = TRUE;
      return TRUE;
   }
   return FALSE;
}


static void
dialog_open (GtkWindow *parent, Scan *sc, const gchar *title)
{
   TagsDialog *td;
   GtkWidget *vbox, *label, *hbox, *button, *scroll, *outer;
   GPtrArray *names;
   guint i;

   td = g_new0 (TagsDialog, 1);
   td->paths = g_steal_pointer (&sc->paths);
   td->n_images = td->paths->len;
   td->items = g_steal_pointer (&sc->items);
   names = g_steal_pointer (&sc->names);
   scan_free (sc);
   td->known = known_tags ();

   td->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (td->window), _("Edit Tags"));
   gtk_window_set_modal (GTK_WINDOW (td->window), TRUE);
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (td->window), parent);
   gtk_window_set_default_size (GTK_WINDOW (td->window), 440, -1);
   g_signal_connect (td->window, "destroy", G_CALLBACK (cb_destroy), td);
   g_signal_connect (td->window, "close-request", G_CALLBACK (cb_close_request), td);
   {
      GtkEventController *keys = gtk_event_controller_key_new ();
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_key), td);
      gtk_widget_add_controller (td->window, keys);
   }

   outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (outer, 12);
   gtk_widget_set_margin_end (outer, 12);
   gtk_widget_set_margin_top (outer, 12);
   gtk_widget_set_margin_bottom (outer, 12);
   gtk_window_set_child (GTK_WINDOW (td->window), outer);
   /* what is edited (insensitive while writing) */
   vbox = td->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_box_append (GTK_BOX (outer), vbox);

   label = gtk_label_new (title);
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
   gtk_box_append (GTK_BOX (outer), td->error);

   td->progress = gtk_progress_bar_new ();
   gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (td->progress), TRUE);
   gtk_widget_set_visible (td->progress, FALSE);
   gtk_box_append (GTK_BOX (outer), td->progress);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign (hbox, GTK_ALIGN_END);
   gtk_widget_set_margin_top (hbox, 6);
   gtk_box_append (GTK_BOX (outer), hbox);

   button = td->cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_cancel), td);
   gtk_box_append (GTK_BOX (hbox), button);

   button = td->ok = gtk_button_new_with_mnemonic (_("_OK"));
   gtk_widget_add_css_class (button, "suggested-action");
   g_signal_connect (button, "clicked", G_CALLBACK (cb_ok), td);
   gtk_box_append (GTK_BOX (hbox), button);

   rebuild (td);
   gtk_window_present (GTK_WINDOW (td->window));
   gtk_widget_grab_focus (td->entry);
}
