/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * [Two Pages | Edit Layouts...]: the layouts of pages side by side.  A row
 * a layout: its name, columns x rows, from right to left, the first page
 * alone; up, down, remove.  What is changed is used at once (the views
 * showing pages side by side show them again) and kept in the settings.
 */

#include "intl.h"
#include <gtk/gtk.h>

#include "gimv_page_layout.h"
#include "gimv_page_layout_dialog.h"

static GtkWidget *dialog_window = NULL;
static GtkWidget *dialog_list   = NULL;

static void rows_fill (void);


static GimvPageLayout *
row_layout (GtkWidget *w)
{
   GPtrArray *a = gimv_image_view_page_layouts ();
   gint i = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (w), "gimv-layout"));

   if (i < 0 || (guint) i >= a->len) return NULL;
   return g_ptr_array_index (a, i);
}


static void
cb_name (GtkEditable *entry, gpointer data)
{
   GimvPageLayout *l = row_layout (GTK_WIDGET (entry));
   const gchar *text = gtk_editable_get_text (entry);
   GimvPageLayout *clean;

   if (!l) return;
   /* as it is kept (no ';') */
   clean = gimv_page_layout_new (text, 1, 1, FALSE, FALSE);
   if (*clean->name && g_strcmp0 (clean->name, l->name)) {
      g_free (l->name);
      l->name = g_strdup (clean->name);
      gimv_image_view_page_layouts_changed (FALSE);
   }
   gimv_page_layout_free (clean);
}


static void
cb_spin (GtkSpinButton *spin, gpointer data)
{
   GimvPageLayout *l = row_layout (GTK_WIDGET (spin));
   gint v = gtk_spin_button_get_value_as_int (spin);
   gint *field;

   if (!l) return;
   field = GPOINTER_TO_INT (data) ? &l->rows : &l->cols;
   if (*field == v) return;
   *field = v;
   gimv_image_view_page_layouts_changed (TRUE);
}


static void
cb_check (GtkCheckButton *check, gpointer data)
{
   GimvPageLayout *l = row_layout (GTK_WIDGET (check));
   gboolean on = gtk_check_button_get_active (check);
   gboolean *field;

   if (!l) return;
   field = GPOINTER_TO_INT (data) ? &l->first_alone : &l->rtl;
   if (*field == on) return;
   *field = on;
   gimv_image_view_page_layouts_changed (TRUE);
}


/* up (-1), down (1) */
static void
cb_move (GtkButton *button, gpointer data)
{
   GPtrArray *a = gimv_image_view_page_layouts ();
   gint i = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "gimv-layout"));
   gint j = i + GPOINTER_TO_INT (data);
   gpointer tmp;

   if (i < 0 || j < 0 || (guint) i >= a->len || (guint) j >= a->len) return;
   tmp = a->pdata[i];
   a->pdata[i] = a->pdata[j];
   a->pdata[j] = tmp;
   gimv_image_view_page_layouts_changed (FALSE);
   rows_fill ();
}


/* a layout is only settings: no question (but not the last one) */
static void
cb_remove (GtkButton *button, gpointer data)
{
   GPtrArray *a = gimv_image_view_page_layouts ();
   gint i = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "gimv-layout"));

   if (a->len <= 1 || i < 0 || (guint) i >= a->len) return;
   g_ptr_array_remove_index (a, i);
   gimv_image_view_page_layouts_changed (TRUE);
   rows_fill ();
}


static void
cb_add (GtkButton *button, gpointer data)
{
   GPtrArray *a = gimv_image_view_page_layouts ();
   GimvPageLayout *last = a->len ? g_ptr_array_index (a, a->len - 1) : NULL;
   gchar *name = g_strdup_printf (_("Layout %u"), a->len + 1);

   g_ptr_array_add (a, gimv_page_layout_new (name, 3, 1,
                                             last ? last->rtl : TRUE,
                                             last ? last->first_alone : TRUE));
   g_free (name);
   gimv_image_view_page_layouts_changed (FALSE);
   rows_fill ();
}


static GtkWidget *
row_widget (gint i, GimvPageLayout *l, guint n)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   GtkWidget *w;

   gtk_widget_set_margin_top (box, 3);
   gtk_widget_set_margin_bottom (box, 3);
   gtk_widget_set_margin_start (box, 6);
   gtk_widget_set_margin_end (box, 6);

#define ROW(widget) \
   g_object_set_data (G_OBJECT (widget), "gimv-layout", GINT_TO_POINTER (i)); \
   gtk_box_append (GTK_BOX (box), widget)

   w = gtk_entry_new ();
   gtk_editable_set_text (GTK_EDITABLE (w), l->name);
   gtk_editable_set_width_chars (GTK_EDITABLE (w), 12);
   gtk_widget_set_hexpand (w, TRUE);
   gtk_widget_set_tooltip_text (w, _("The name (the menu, the status bar)"));
   g_signal_connect (w, "changed", G_CALLBACK (cb_name), NULL);
   ROW (w);

   w = gtk_spin_button_new_with_range (1, GIMV_PAGE_LAYOUT_MAX_COLS, 1);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (w), l->cols);
   gtk_widget_set_tooltip_text (w, _("Pages side by side"));
   g_signal_connect (w, "value-changed", G_CALLBACK (cb_spin), GINT_TO_POINTER (0));
   ROW (w);

   w = gtk_label_new ("×");
   gtk_box_append (GTK_BOX (box), w);

   w = gtk_spin_button_new_with_range (1, GIMV_PAGE_LAYOUT_MAX_ROWS, 1);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (w), l->rows);
   gtk_widget_set_tooltip_text (w, _("Rows"));
   g_signal_connect (w, "value-changed", G_CALLBACK (cb_spin), GINT_TO_POINTER (1));
   ROW (w);

   w = gtk_check_button_new_with_label (_("Right to left"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (w), l->rtl);
   gtk_widget_set_tooltip_text (w, _("The first page on the right (manga); off: on the left"));
   g_signal_connect (w, "toggled", G_CALLBACK (cb_check), GINT_TO_POINTER (0));
   ROW (w);

   w = gtk_check_button_new_with_label (_("First page alone"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (w), l->first_alone);
   gtk_widget_set_tooltip_text (w, _("The first page of the list (the cover) is shown alone"));
   g_signal_connect (w, "toggled", G_CALLBACK (cb_check), GINT_TO_POINTER (1));
   ROW (w);

   w = gtk_button_new_from_icon_name ("go-up-symbolic");
   gtk_widget_set_tooltip_text (w, _("Up"));
   gtk_widget_set_sensitive (w, i > 0);
   g_signal_connect (w, "clicked", G_CALLBACK (cb_move), GINT_TO_POINTER (-1));
   ROW (w);

   w = gtk_button_new_from_icon_name ("go-down-symbolic");
   gtk_widget_set_tooltip_text (w, _("Down"));
   gtk_widget_set_sensitive (w, (guint) i + 1 < n);
   g_signal_connect (w, "clicked", G_CALLBACK (cb_move), GINT_TO_POINTER (1));
   ROW (w);

   w = gtk_button_new_from_icon_name ("list-remove-symbolic");
   gtk_widget_set_tooltip_text (w, _("Remove this layout"));
   gtk_widget_set_sensitive (w, n > 1);
   g_signal_connect (w, "clicked", G_CALLBACK (cb_remove), NULL);
   ROW (w);

#undef ROW
   return box;
}


/* the rows made again (after a layout was added, removed, moved) */
static void
rows_fill (void)
{
   GPtrArray *a = gimv_image_view_page_layouts ();
   GtkWidget *child;
   guint i;

   if (!dialog_list) return;
   while ((child = gtk_widget_get_first_child (dialog_list)))
      gtk_list_box_remove (GTK_LIST_BOX (dialog_list), child);
   for (i = 0; i < a->len; i++)
      gtk_list_box_append (GTK_LIST_BOX (dialog_list),
                           row_widget (i, g_ptr_array_index (a, i), a->len));
}


static void
cb_destroy (GtkWidget *window, gpointer data)
{
   dialog_window = NULL;
   dialog_list = NULL;
}


static gboolean
cb_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
        GdkModifierType state, gpointer data)
{
   if (keyval == GDK_KEY_Escape) {
      gtk_window_destroy (GTK_WINDOW (data));
      return TRUE;
   }
   return FALSE;
}


void
gimv_page_layout_dialog_open (GtkWindow *parent, GimvImageView *iv)
{
   GtkWidget *window, *vbox, *label, *scroll, *list, *hbox, *button;
   GtkEventController *key;

   if (dialog_window) {
      gtk_window_present (GTK_WINDOW (dialog_window));
      return;
   }

   window = gtk_window_new ();
   dialog_window = window;
   gtk_window_set_title (GTK_WINDOW (window), _("Layouts of Pages"));
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   gtk_window_set_default_size (GTK_WINDOW (window), 640, 320);
   g_signal_connect (window, "destroy", G_CALLBACK (cb_destroy), NULL);

   key = gtk_event_controller_key_new ();
   g_signal_connect (key, "key-pressed", G_CALLBACK (cb_key), window);
   gtk_widget_add_controller (window, key);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_top (vbox, 10);
   gtk_widget_set_margin_bottom (vbox, 10);
   gtk_widget_set_margin_start (vbox, 10);
   gtk_widget_set_margin_end (vbox, 10);
   gtk_window_set_child (GTK_WINDOW (window), vbox);

   label = gtk_label_new (_("How pages are shown side by side ([Two Pages | Page Layout]): "
                            "pages side by side × rows.  Changes are used at once."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (vbox), label);

   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_box_append (GTK_BOX (vbox), scroll);

   list = gtk_list_box_new ();
   gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
   dialog_list = list;
   rows_fill ();

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_append (GTK_BOX (vbox), hbox);

   button = gtk_button_new_with_mnemonic (_("_Add"));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_add), NULL);
   gtk_box_append (GTK_BOX (hbox), button);

   button = gtk_button_new_with_mnemonic (_("_Close"));
   gtk_widget_set_hexpand (button, TRUE);
   gtk_widget_set_halign (button, GTK_ALIGN_END);
   g_signal_connect_swapped (button, "clicked", G_CALLBACK (gtk_window_destroy), window);
   gtk_box_append (GTK_BOX (hbox), button);

   gtk_window_present (GTK_WINDOW (window));
}
