/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the keyboard shortcuts window, to change the keys of the
 * menus (~/.gimv/keyaccelrc) and the other keys (~/.gimv/keyconf) without
 * editing the files.
 *
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
 */

#include "gimageview.h"

#include <string.h>

#include "gimv_gtk4_compat.h"
#include "gimv_shortcuts_win.h"
#include "gimv_thumb_view.h"
#include "menu.h"
#include "prefs.h"

/* where a key works: two keys of the same place cannot be the same */
#define SCOPE_THUMBWIN  (1 << 0)
#define SCOPE_IMAGEWIN  (1 << 1)
#define SCOPE_NAVWIN    (1 << 2)
#define SCOPE_ENTRY     (1 << 3)

/* the menus, as menubar_create () and menu_create_items () make them */
extern GimvMenuEntry gimv_thumb_win_menu_items[];
extern GimvMenuEntry gimv_thumb_win_view_items[];
extern GimvMenuEntry gimv_thumb_win_sort_items[];
extern GimvMenuEntry gimv_image_win_menu_items[];
extern GimvMenuEntry gimv_image_win_view_items[];
extern GimvMenuEntry gimv_image_win_move_items[];
extern GimvMenuEntry gimv_image_view_popup_items[];
extern GimvMenuEntry gimv_image_view_zoom_items[];
extern GimvMenuEntry gimv_image_view_rotate_items[];
extern GimvMenuEntry gimv_image_view_playable_items[];
extern GimvMenuEntry gimvhelp_menu_items[];

typedef struct {
   const gchar   *group;
   guint          scope;
   const gchar   *prefix;      /* of the accel paths */
   GimvMenuEntry *entries;
   /* the menus it is in: an entry path ("/_View") or a name to translate */
   const gchar   *parents[3];
} MenuSource;

static const MenuSource menu_sources[] = {
   { N_("Thumbnail Window"), SCOPE_THUMBWIN, "<ThumbWinMainMenu>",
     gimv_thumb_win_menu_items, { NULL } },
   { N_("Thumbnail Window"), SCOPE_THUMBWIN, "<ThumbWinViewSubMenu>",
     gimv_thumb_win_view_items, { "/_View", NULL } },
   { N_("Thumbnail Window"), SCOPE_THUMBWIN, "<ThumbWinSortSubMenu>",
     gimv_thumb_win_sort_items, { "/_View", "/_Sort File List", NULL } },
   { N_("Thumbnail Window"), SCOPE_THUMBWIN, "<ThumbWinPreviewPop>",
     gimv_image_view_popup_items, { N_("Preview"), NULL } },
   { N_("Thumbnail Window"), SCOPE_THUMBWIN, "<ThumbWinPreviewPop>",
     gimv_image_view_zoom_items, { N_("Preview"), "/_Zoom", NULL } },
   { N_("Thumbnail Window"), SCOPE_THUMBWIN, "<ThumbWinPreviewPop>",
     gimv_image_view_rotate_items, { N_("Preview"), "/_Rotate", NULL } },
   { N_("Thumbnail Window"), SCOPE_THUMBWIN, "<ThumbWinPreviewPop>",
     gimv_image_view_playable_items, { N_("Preview"), "/M_ovie", NULL } },
   { N_("Image Window"), SCOPE_IMAGEWIN, "<ImageWinMainMenu>",
     gimv_image_win_menu_items, { NULL } },
   { N_("Image Window"), SCOPE_IMAGEWIN, "<ImageWinViewSubMenu>",
     gimv_image_win_view_items, { "/_View", NULL } },
   { N_("Image Window"), SCOPE_IMAGEWIN, "<ImageWinZoomSubMenu>",
     gimv_image_view_zoom_items, { "/_View", "/_Zoom", NULL } },
   { N_("Image Window"), SCOPE_IMAGEWIN, "<ImageWinRotateSubMenu>",
     gimv_image_view_rotate_items, { "/_View", "/_Rotate", NULL } },
   { N_("Image Window"), SCOPE_IMAGEWIN, "<ImageWinMoveSubMenu>",
     gimv_image_win_move_items, { "/_Go", NULL } },
   { N_("Image Window"), SCOPE_IMAGEWIN, "<ImageWinMovieSubMenu>",
     gimv_image_view_playable_items, { "/M_ovie", NULL } },
   { N_("Help Menu (Both Windows)"), SCOPE_THUMBWIN | SCOPE_IMAGEWIN, "<HelpSubMenu>",
     gimvhelp_menu_items, { "/_Help", NULL } },
};

typedef struct {
   gchar      **field;       /* &akey.xxx */
   const gchar *label;
   guint        scope;
   gboolean     next_start;  /* read when the window is made */
} KeySource;

static const KeySource key_sources[] = {
   { &akey.common_popup_menu, N_("Open the Right Button Menu"),
     SCOPE_THUMBWIN | SCOPE_IMAGEWIN, FALSE },
   { &akey.common_auto_completion1, N_("Complete a Path in an Entry (1)"), SCOPE_ENTRY, FALSE },
   { &akey.common_auto_completion2, N_("Complete a Path in an Entry (2)"), SCOPE_ENTRY, FALSE },
   { &akey.imgwin_zoomin,  N_("Navigator: Zoom In"),  SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoomout, N_("Navigator: Zoom Out"), SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_fit_img, N_("Navigator: Fit to Window"), SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom10,  N_("Navigator: 10%"),  SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom25,  N_("Navigator: 25%"),  SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom50,  N_("Navigator: 50%"),  SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom75,  N_("Navigator: 75%"),  SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom100, N_("Navigator: 100%"), SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom125, N_("Navigator: 125%"), SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom150, N_("Navigator: 150%"), SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom175, N_("Navigator: 175%"), SCOPE_NAVWIN, FALSE },
   { &akey.imgwin_zoom200, N_("Navigator: 200%"), SCOPE_NAVWIN, FALSE },
};

typedef struct {
   const gchar *group;
   gchar       *label;
   gchar       *path;        /* the accel path of a menu item, or */
   gchar      **field;       /* a key of ~/.gimv/keyconf */
   gchar       *default_key; /* canonical, "" for none */
   guint        scope;
   gboolean     next_start;
   GtkWidget   *key_button;
   GtkWidget   *reset_button;
} Shortcut;

static struct {
   GtkWidget  *window;
   GtkWidget  *list;
   GtkWidget  *entry;
   GPtrArray  *shortcuts;
} sw;

/* the window a key is set in */
static struct {
   GtkWidget  *window;
   GtkWidget  *key;          /* the key pressed */
   GtkWidget  *status;       /* what it is now */
   GtkWidget  *ok;
   Shortcut   *sc;
   gchar      *pending;      /* canonical, "" for none */
} kd;


/* "" for none or a key not understood */
static gchar *
canonical (const gchar *accel)
{
   guint keyval = 0;
   GdkModifierType mods = 0;

   if (!accel || !*accel || !gimv_accelerator_parse (accel, &keyval, &mods) || !keyval)
      return g_strdup ("");
   return gtk_accelerator_name (keyval, mods);
}


static gchar *
key_label (const gchar *accel)
{
   guint keyval = 0;
   GdkModifierType mods = 0;

   if (!accel || !*accel || !gimv_accelerator_parse (accel, &keyval, &mods) || !keyval)
      return g_strdup ("");
   return gtk_accelerator_get_label (keyval, mods);
}


static gchar *
current_key (Shortcut *sc)
{
   if (sc->path) return canonical (gimv_accel_map_get (sc->path, NULL));
   return canonical (*sc->field);
}


static void
shortcut_free (Shortcut *sc)
{
   g_free (sc->label);
   g_free (sc->path);
   g_free (sc->default_key);
   g_free (sc);
}


/* "View › Zoom › Zoom In" */
static gchar *
menu_label (const MenuSource *src, const gchar *entry_path)
{
   GString *str = g_string_new (NULL);
   gchar *l, **parts;
   gint i;

   for (i = 0; i < 3 && src->parents[i]; i++) {
      if (src->parents[i][0] == '/') {
         l = gimv_menu_entry_label (src->parents[i]);
         g_string_append (str, l);
         g_free (l);
      } else {
         g_string_append (str, _(src->parents[i]));
      }
      g_string_append (str, " \342\200\272 ");
   }
   l = gimv_menu_entry_label (entry_path);
   parts = g_strsplit (l, "/", -1);
   for (i = 0; parts[i]; i++) {
      if (i > 0) g_string_append (str, " \342\200\272 ");
      g_string_append (str, parts[i]);
   }
   g_strfreev (parts);
   g_free (l);
   return g_string_free (str, FALSE);
}


static gboolean
entry_has_action (const GimvMenuEntry *entry)
{
   const gchar *type = entry->item_type;

   if (!entry->callback) return FALSE;
   if (type && (!strcmp (type, "<Branch>") || !strcmp (type, "<LastBranch>")
                || !strcmp (type, "<Separator>") || !strcmp (type, "<Title>")
                || !strcmp (type, "<Tearoff>")))
      return FALSE;
   return TRUE;
}


static void
collect_shortcuts (void)
{
   GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
   guint i, j;

   sw.shortcuts = g_ptr_array_new_with_free_func ((GDestroyNotify) shortcut_free);

   for (i = 0; i < G_N_ELEMENTS (menu_sources); i++) {
      const MenuSource *src = &menu_sources[i];
      for (j = 0; src->entries[j].path; j++) {
         const GimvMenuEntry *entry = &src->entries[j];
         gchar *norm, *path;
         Shortcut *sc;

         if (!entry_has_action (entry)) continue;
         norm = gimv_menu_entry_path (entry->path);
         path = g_strconcat (src->prefix, norm, NULL);
         g_free (norm);
         if (g_hash_table_contains (seen, path)) {
            g_free (path);
            continue;
         }
         sc = g_new0 (Shortcut, 1);
         sc->group = src->group;
         sc->path  = path;
         sc->scope = src->scope;
         sc->label = menu_label (src, entry->path);
         /* known (and its default) even when the menu is not made yet */
         gimv_accel_map_get (path, entry->accelerator);
         sc->default_key = canonical (gimv_accel_map_get_default (path));
         g_hash_table_add (seen, sc->path);
         g_ptr_array_add (sw.shortcuts, sc);
      }
   }
   g_hash_table_unref (seen);

   for (i = 0; i < G_N_ELEMENTS (key_sources); i++) {
      Shortcut *sc = g_new0 (Shortcut, 1);
      sc->group = N_("Other Keys");
      sc->field = key_sources[i].field;
      sc->label = g_strdup (_(key_sources[i].label));
      sc->scope = key_sources[i].scope;
      sc->default_key = canonical (prefs_key_default (sc->field));
      g_ptr_array_add (sw.shortcuts, sc);
   }
   /* the display modes of the thumbnail window */
   {
      gint n = 0, num = sizeof (akey.thumbwin_disp_mode) / sizeof (gchar *);
      gchar **labels = gimv_thumb_view_get_summary_mode_labels (&n);
      for (i = 0; labels && (gint) i < n && (gint) i < num; i++) {
         Shortcut *sc = g_new0 (Shortcut, 1);
         sc->group = N_("Other Keys");
         sc->field = &akey.thumbwin_disp_mode[i];
         sc->label = g_strdup_printf (_("Thumbnail Window: Display Mode \"%s\""),
                                      _(labels[i]));
         sc->scope = SCOPE_THUMBWIN;
         sc->next_start = TRUE;
         sc->default_key = canonical (prefs_key_default (sc->field));
         g_ptr_array_add (sw.shortcuts, sc);
      }
   }
}


/* the key shown on the row of sc */
static void
update_row (Shortcut *sc)
{
   gchar *cur = current_key (sc), *l;

   l = key_label (cur);
   gtk_button_set_label (GTK_BUTTON (sc->key_button), *l ? l : _("None"));
   if (*l) gtk_widget_remove_css_class (sc->key_button, "dim-label");
   else gtk_widget_add_css_class (sc->key_button, "dim-label");
   g_free (l);
   gtk_widget_set_visible (sc->reset_button, strcmp (cur, sc->default_key) != 0);
   if (strcmp (cur, sc->default_key)) {
      gchar *d = key_label (sc->default_key), *tip;
      tip = g_strdup_printf (_("Back to the default (%s)"), *d ? d : _("None"));
      gtk_widget_set_tooltip_text (sc->reset_button, tip);
      g_free (tip);
      g_free (d);
   }
   g_free (cur);
}


static void
save_keys (gboolean menus, gboolean others)
{
   if (menus) {
      gchar *file = g_build_filename (g_get_home_dir (), GIMV_RC_DIR, GIMV_KEYACCEL_RC, NULL);
      gimv_accel_map_save (file);
      g_free (file);
   }
   if (others) prefs_save_config ();
}


/* the key of sc, kept at once */
static void
set_key (Shortcut *sc, const gchar *accel)
{
   if (sc->path) {
      gimv_accel_map_change (sc->path, accel);
   } else {
      g_free (*sc->field);
      *sc->field = g_strdup (accel ? accel : "");
   }
   update_row (sc);
}


typedef struct {
   Shortcut *sc, *other;
   gchar    *accel;
} Conflict;


static void
conflict_free (Conflict *c)
{
   g_free (c->accel);
   g_free (c);
}


static void
cb_conflict_done (GObject *source, GAsyncResult *res, gpointer data)
{
   Conflict *c = data;
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);

   if (button == 1 && sw.window) {
      set_key (c->other, "");
      set_key (c->sc, c->accel);
      save_keys (c->sc->path || c->other->path, !c->sc->path || !c->other->path);
   }
   conflict_free (c);
}


static void
assign (Shortcut *sc, const gchar *accel)
{
   gchar *key = canonical (accel), *cur = current_key (sc);
   guint i;

   if (!strcmp (key, cur)) goto out;
   /* the same key for another thing of the same place: asked */
   if (*key) {
      for (i = 0; i < sw.shortcuts->len; i++) {
         Shortcut *other = g_ptr_array_index (sw.shortcuts, i);
         gchar *okey;
         if (other == sc || !(other->scope & sc->scope)) continue;
         okey = current_key (other);
         if (!strcmp (okey, key)) {
            const gchar *buttons[] = { _("_Cancel"), _("_Use It Here"), NULL };
            gchar *l = key_label (key);
            GtkAlertDialog *dialog = gtk_alert_dialog_new (_("%s is already the key of \"%s\"."),
                                                           l, other->label);
            gchar *detail = g_strdup_printf (_("Using it for \"%s\" leaves \"%s\" without a key."),
                                             sc->label, other->label);
            Conflict *c = g_new0 (Conflict, 1);
            c->sc = sc;
            c->other = other;
            c->accel = g_strdup (key);
            gtk_alert_dialog_set_detail (dialog, detail);
            gtk_alert_dialog_set_buttons (dialog, buttons);
            gtk_alert_dialog_set_cancel_button (dialog, 0);
            gtk_alert_dialog_set_default_button (dialog, 0);
            gtk_alert_dialog_choose (dialog, GTK_WINDOW (sw.window), NULL,
                                     cb_conflict_done, c);
            g_object_unref (dialog);
            g_free (detail);
            g_free (l);
            g_free (okey);
            goto out;
         }
         g_free (okey);
      }
   }
   set_key (sc, key);
   save_keys (sc->path != NULL, sc->path == NULL);
out:
   g_free (key);
   g_free (cur);
}


static void
cb_reset_clicked (GtkButton *button, Shortcut *sc)
{
   if (kd.window) gtk_window_destroy (GTK_WINDOW (kd.window));
   assign (sc, sc->default_key);
}


static gboolean
is_modifier (guint keyval)
{
   switch (keyval) {
   case GDK_KEY_Shift_L: case GDK_KEY_Shift_R:
   case GDK_KEY_Control_L: case GDK_KEY_Control_R:
   case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
   case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
   case GDK_KEY_Super_L: case GDK_KEY_Super_R:
   case GDK_KEY_Hyper_L: case GDK_KEY_Hyper_R:
   case GDK_KEY_Caps_Lock: case GDK_KEY_Shift_Lock:
   case GDK_KEY_ISO_Level3_Shift: case GDK_KEY_ISO_Level5_Shift:
   case GDK_KEY_Num_Lock: case GDK_KEY_Mode_switch:
      return TRUE;
   default:
      return FALSE;
   }
}


/* the keys gimv itself uses, not changed here (with no modifier) */
typedef struct {
   guint        keyval;
   guint        scope;
   const gchar *label;
} FixedKey;

static const FixedKey fixed_keys[] = {
   { GDK_KEY_Left,      SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: scroll") },
   { GDK_KEY_Right,     SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: scroll") },
   { GDK_KEY_Up,        SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: scroll") },
   { GDK_KEY_Down,      SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: scroll") },
   { GDK_KEY_Page_Up,   SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: the previous one") },
   { GDK_KEY_Page_Down, SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: the next one") },
   { GDK_KEY_space,     SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: the next one") },
   { GDK_KEY_Home,      SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: the first one") },
   { GDK_KEY_End,       SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: the last one") },
   { GDK_KEY_plus,      SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: zoom in") },
   { GDK_KEY_KP_Add,    SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: zoom in") },
   { GDK_KEY_minus,     SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: zoom out") },
   { GDK_KEY_KP_Subtract, SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: zoom out") },
   { GDK_KEY_equal,     SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: 100%") },
   { GDK_KEY_KP_Enter,  SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: 100%") },
   { GDK_KEY_KP_Divide, SCOPE_THUMBWIN | SCOPE_IMAGEWIN, N_("the image: fit to the window") },
   { GDK_KEY_Return,    SCOPE_THUMBWIN, N_("the list: open the image") },
   { GDK_KEY_Escape,    SCOPE_IMAGEWIN, N_("leave the full screen") },
   { GDK_KEY_Left,      SCOPE_NAVWIN, N_("the navigator: move") },
   { GDK_KEY_Right,     SCOPE_NAVWIN, N_("the navigator: move") },
   { GDK_KEY_Up,        SCOPE_NAVWIN, N_("the navigator: move") },
   { GDK_KEY_Down,      SCOPE_NAVWIN, N_("the navigator: move") },
   { GDK_KEY_Escape,    SCOPE_NAVWIN, N_("the navigator: close") },
   { GDK_KEY_equal,     SCOPE_NAVWIN, N_("the navigator: zoom in") },
   { GDK_KEY_minus,     SCOPE_NAVWIN, N_("the navigator: zoom out") },
};


/* what the key pressed is now, under it */
static void
key_dialog_update (void)
{
   Shortcut *sc = kd.sc;
   GString *str = g_string_new (NULL);
   gchar *cur = current_key (sc), *l = key_label (kd.pending);
   guint keyval = 0, i;
   GdkModifierType mods = 0;

   gtk_label_set_text (GTK_LABEL (kd.key), *l ? l : _("None"));
   gtk_widget_set_sensitive (kd.ok, strcmp (kd.pending, cur) != 0);

   if (!strcmp (kd.pending, cur)) {
      g_string_append (str, *cur ? _("The key now.") : _("No key now."));
   } else if (*kd.pending) {
      gboolean used = FALSE;

      for (i = 0; i < sw.shortcuts->len; i++) {
         Shortcut *other = g_ptr_array_index (sw.shortcuts, i);
         gchar *okey;
         if (other == sc) continue;
         okey = current_key (other);
         if (!strcmp (okey, kd.pending)) {
            if (str->len) g_string_append_c (str, '\n');
            if (other->scope & sc->scope)
               g_string_append_printf (str, _("The key of \"%s\" (%s): OK asks to take it from there."),
                                       other->label, _(other->group));
            else
               g_string_append_printf (str, _("In another window, the key of \"%s\" (%s)."),
                                       other->label, _(other->group));
            used = used || (other->scope & sc->scope);
         }
         g_free (okey);
      }
      gimv_accelerator_parse (kd.pending, &keyval, &mods);
      for (i = 0; !mods && i < G_N_ELEMENTS (fixed_keys); i++) {
         if (fixed_keys[i].keyval != keyval || !(fixed_keys[i].scope & sc->scope)) continue;
         if (str->len) g_string_append_c (str, '\n');
         g_string_append_printf (str, _("Used by gimv itself (%s): it may not work here."),
                                 _(fixed_keys[i].label));
         used = TRUE;
         break;
      }
      if (!used) {
         if (str->len) g_string_append_c (str, '\n');
         g_string_append (str, _("Not used in this window."));
      }
   }
   gtk_label_set_text (GTK_LABEL (kd.status), str->str);
   g_string_free (str, TRUE);
   g_free (l);
   g_free (cur);
}


/* any key is the key: Escape and Backspace too */
static gboolean
cb_key_dialog_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
                   GdkModifierType state, gpointer data)
{
   GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();

   if (!kd.sc) return FALSE;
   if (is_modifier (keyval)) return TRUE;
   if (keyval == GDK_KEY_ISO_Left_Tab) keyval = GDK_KEY_Tab;
   keyval = gdk_keyval_to_lower (keyval);
   g_free (kd.pending);
   kd.pending = gtk_accelerator_name (keyval, mods);
   key_dialog_update ();
   return TRUE;
}


static void
cb_key_dialog_destroy (GtkWidget *window, gpointer data)
{
   kd.window = NULL;
   kd.sc = NULL;
   g_clear_pointer (&kd.pending, g_free);
}


static void
cb_key_dialog_ok (GtkButton *button, gpointer data)
{
   Shortcut *sc = kd.sc;
   gchar *key = g_strdup (kd.pending);

   gtk_window_destroy (GTK_WINDOW (kd.window));
   if (sc && sw.window) assign (sc, key);
   g_free (key);
}


static void
cb_key_dialog_none (GtkButton *button, gpointer data)
{
   Shortcut *sc = kd.sc;

   gtk_window_destroy (GTK_WINDOW (kd.window));
   if (sc && sw.window) assign (sc, "");
}


/* a window to press the new key in, as Mozc has: the key shown with what
   it is now, OK to take it */
static void
key_dialog_open (Shortcut *sc)
{
   GtkWidget *vbox, *label, *frame, *hbox, *button;
   GtkEventController *keys;
   gchar *markup;

   if (kd.window) gtk_window_destroy (GTK_WINDOW (kd.window));

   kd.sc = sc;
   kd.pending = current_key (sc);
   kd.window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (kd.window), _("Set a Key"));
   gtk_window_set_transient_for (GTK_WINDOW (kd.window), GTK_WINDOW (sw.window));
   gtk_window_set_modal (GTK_WINDOW (kd.window), TRUE);
   gtk_window_set_resizable (GTK_WINDOW (kd.window), FALSE);
   gtk_window_set_default_size (GTK_WINDOW (kd.window), 420, -1);
   g_signal_connect (kd.window, "destroy", G_CALLBACK (cb_key_dialog_destroy), NULL);

   /* before the buttons and the mnemonics: every key is the key */
   keys = gtk_event_controller_key_new ();
   gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
   g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_key_dialog_key), NULL);
   gtk_widget_add_controller (kd.window, keys);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (kd.window), vbox);

   label = gtk_label_new (NULL);
   markup = g_markup_printf_escaped ("<b>%s</b>\n%s", sc->label, _(sc->group));
   gtk_label_set_markup (GTK_LABEL (label), markup);
   g_free (markup);
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (vbox), label);

   label = gtk_label_new (_("Press the key to use (any key: Escape and Backspace too)."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_box_append (GTK_BOX (vbox), label);

   frame = gtk_frame_new (NULL);
   kd.key = gtk_label_new (NULL);
   gtk_widget_add_css_class (kd.key, "title-2");
   gtk_widget_set_margin_top (kd.key, 8);
   gtk_widget_set_margin_bottom (kd.key, 8);
   gtk_label_set_selectable (GTK_LABEL (kd.key), FALSE);
   gtk_frame_set_child (GTK_FRAME (frame), kd.key);
   gtk_box_append (GTK_BOX (vbox), frame);

   kd.status = gtk_label_new (NULL);
   gtk_label_set_wrap (GTK_LABEL (kd.status), TRUE);
   gtk_label_set_xalign (GTK_LABEL (kd.status), 0.0);
   gtk_label_set_max_width_chars (GTK_LABEL (kd.status), 50);
   gtk_box_append (GTK_BOX (vbox), kd.status);

   /* no mnemonics: Alt and a letter is a key to set */
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_margin_top (hbox, 6);
   gtk_box_append (GTK_BOX (vbox), hbox);
   button = gtk_button_new_with_label (_("No Key"));
   gtk_widget_set_tooltip_text (button, _("Take the key away"));
   gtk_widget_set_focusable (button, FALSE);
   g_signal_connect (button, "clicked", G_CALLBACK (cb_key_dialog_none), NULL);
   gtk_box_append (GTK_BOX (hbox), button);
   label = gtk_label_new (NULL);
   gtk_widget_set_hexpand (label, TRUE);
   gtk_box_append (GTK_BOX (hbox), label);
   button = gtk_button_new_with_label (_("Cancel"));
   gtk_widget_set_focusable (button, FALSE);
   g_signal_connect_swapped (button, "clicked", G_CALLBACK (gtk_window_destroy), kd.window);
   gtk_box_append (GTK_BOX (hbox), button);
   kd.ok = gtk_button_new_with_label (_("OK"));
   gtk_widget_add_css_class (kd.ok, "suggested-action");
   gtk_widget_set_focusable (kd.ok, FALSE);
   g_signal_connect (kd.ok, "clicked", G_CALLBACK (cb_key_dialog_ok), NULL);
   gtk_box_append (GTK_BOX (hbox), kd.ok);

   key_dialog_update ();
   gtk_window_present (GTK_WINDOW (kd.window));
}


static void
cb_key_clicked (GtkButton *button, Shortcut *sc)
{
   key_dialog_open (sc);
}


static gboolean
filter_row (GtkListBoxRow *row, gpointer data)
{
   Shortcut *sc = g_object_get_data (G_OBJECT (row), "shortcut");
   const gchar *typed = gtk_editable_get_text (GTK_EDITABLE (sw.entry));
   gchar *fold, *lf, *cur, *kl, *kf;
   gboolean ok;

   if (!sc || !typed || !*typed) return TRUE;
   fold = g_utf8_casefold (typed, -1);
   lf = g_utf8_casefold (sc->label, -1);
   cur = current_key (sc);
   kl = key_label (cur);
   kf = g_utf8_casefold (kl, -1);
   ok = strstr (lf, fold) || strstr (kf, fold);
   g_free (kf);
   g_free (kl);
   g_free (cur);
   g_free (lf);
   g_free (fold);
   return ok;
}


/* the name of the place above its first row */
static void
header_row (GtkListBoxRow *row, GtkListBoxRow *before, gpointer data)
{
   Shortcut *sc = g_object_get_data (G_OBJECT (row), "shortcut");
   Shortcut *prev = before ? g_object_get_data (G_OBJECT (before), "shortcut") : NULL;
   GtkWidget *label;

   if (!sc) return;
   if (prev && !strcmp (prev->group, sc->group)) {
      gtk_list_box_row_set_header (row, NULL);
      return;
   }
   label = gtk_label_new (_(sc->group));
   gtk_widget_add_css_class (label, "heading");
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_set_margin_top (label, before ? 12 : 4);
   gtk_widget_set_margin_bottom (label, 4);
   gtk_widget_set_margin_start (label, 6);
   gtk_list_box_row_set_header (row, label);
}


static void
cb_search_changed (GtkSearchEntry *entry, gpointer data)
{
   gtk_list_box_invalidate_filter (GTK_LIST_BOX (sw.list));
   gtk_list_box_invalidate_headers (GTK_LIST_BOX (sw.list));
}


static void
cb_reset_all_done (GObject *source, GAsyncResult *res, gpointer data)
{
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
   guint i;

   if (button != 1 || !sw.window) return;
   for (i = 0; i < sw.shortcuts->len; i++) {
      Shortcut *sc = g_ptr_array_index (sw.shortcuts, i);
      gchar *cur = current_key (sc);
      if (strcmp (cur, sc->default_key)) set_key (sc, sc->default_key);
      g_free (cur);
   }
   save_keys (TRUE, TRUE);
}


static void
cb_reset_all (GtkButton *button, gpointer data)
{
   const gchar *buttons[] = { _("_Cancel"), _("_Reset All"), NULL };
   GtkAlertDialog *dialog;

   dialog = gtk_alert_dialog_new ("%s", _("Reset all the keys to the defaults?"));
   gtk_alert_dialog_set_detail (dialog, _("The keys changed here or in the files are lost."));
   gtk_alert_dialog_set_buttons (dialog, buttons);
   gtk_alert_dialog_set_cancel_button (dialog, 0);
   gtk_alert_dialog_set_default_button (dialog, 0);
   gtk_alert_dialog_choose (dialog, GTK_WINDOW (sw.window), NULL, cb_reset_all_done, NULL);
   g_object_unref (dialog);
}


static void
cb_destroy (GtkWidget *window, gpointer data)
{
   sw.window = NULL;
   if (kd.window) gtk_window_destroy (GTK_WINDOW (kd.window));
   g_clear_pointer (&sw.shortcuts, g_ptr_array_unref);
}


static GtkWidget *
make_row (Shortcut *sc)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   GtkWidget *label = gtk_label_new (sc->label);
   GtkWidget *row;

   gtk_widget_set_margin_start (box, 12);
   gtk_widget_set_margin_end (box, 6);
   gtk_widget_set_margin_top (box, 2);
   gtk_widget_set_margin_bottom (box, 2);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   gtk_widget_set_hexpand (label, TRUE);
   gtk_widget_set_tooltip_text (label, sc->label);
   if (sc->next_start) {
      gchar *tip = g_strdup_printf ("%s\n%s", sc->label,
                                    _("A key changed takes effect from the next start."));
      gtk_widget_set_tooltip_text (label, tip);
      g_free (tip);
   }
   gtk_box_append (GTK_BOX (box), label);

   sc->reset_button = gtk_button_new_from_icon_name ("edit-undo-symbolic");
   gtk_widget_add_css_class (sc->reset_button, "flat");
   gtk_widget_set_valign (sc->reset_button, GTK_ALIGN_CENTER);
   g_signal_connect (sc->reset_button, "clicked", G_CALLBACK (cb_reset_clicked), sc);
   gtk_box_append (GTK_BOX (box), sc->reset_button);

   sc->key_button = gtk_button_new_with_label ("");
   gtk_widget_set_size_request (sc->key_button, 150, -1);
   gtk_widget_set_valign (sc->key_button, GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text (sc->key_button,
                                _("Click to set the key"));
   g_signal_connect (sc->key_button, "clicked", G_CALLBACK (cb_key_clicked), sc);
   gtk_box_append (GTK_BOX (box), sc->key_button);

   /* the data first: the header is made when it is added */
   row = gtk_list_box_row_new ();
   g_object_set_data (G_OBJECT (row), "shortcut", sc);
   gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
   gtk_list_box_append (GTK_LIST_BOX (sw.list), row);
   gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
   update_row (sc);
   return row;
}


void
gimv_shortcuts_win_open (GtkWindow *parent)
{
   GtkWidget *vbox, *label, *scroll, *hbox, *button;
   guint i;

   if (sw.window) {
      gtk_window_present (GTK_WINDOW (sw.window));
      return;
   }

   collect_shortcuts ();

   sw.window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (sw.window), _("Keyboard Shortcuts"));
   gtk_window_set_default_size (GTK_WINDOW (sw.window), 640, 620);
   if (parent) gtk_window_set_transient_for (GTK_WINDOW (sw.window), parent);
   g_signal_connect (sw.window, "destroy", G_CALLBACK (cb_destroy), NULL);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (sw.window), vbox);

   label = gtk_label_new (_("Click a key to set another: press it in the window that opens, "
                            "which tells what the key is now. A key changed works at once "
                            "and is kept."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_box_append (GTK_BOX (vbox), label);

   sw.entry = gtk_search_entry_new ();
   gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (sw.entry),
                                          _("A part of a menu item or a key"));
   g_signal_connect (sw.entry, "search-changed", G_CALLBACK (cb_search_changed), NULL);
   gtk_box_append (GTK_BOX (vbox), sw.entry);

   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_box_append (GTK_BOX (vbox), scroll);

   sw.list = gtk_list_box_new ();
   gtk_list_box_set_selection_mode (GTK_LIST_BOX (sw.list), GTK_SELECTION_NONE);
   gtk_list_box_set_filter_func (GTK_LIST_BOX (sw.list), filter_row, NULL, NULL);
   gtk_list_box_set_header_func (GTK_LIST_BOX (sw.list), header_row, NULL, NULL);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), sw.list);
   for (i = 0; i < sw.shortcuts->len; i++)
      make_row (g_ptr_array_index (sw.shortcuts, i));

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_append (GTK_BOX (vbox), hbox);
   button = gtk_button_new_with_mnemonic (_("Reset _All to Defaults..."));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_reset_all), NULL);
   gtk_box_append (GTK_BOX (hbox), button);
   label = gtk_label_new (NULL);
   gtk_widget_set_hexpand (label, TRUE);
   gtk_box_append (GTK_BOX (hbox), label);
   button = gtk_button_new_with_mnemonic (_("_Close"));
   g_signal_connect_swapped (button, "clicked", G_CALLBACK (gtk_window_destroy), sw.window);
   gtk_box_append (GTK_BOX (hbox), button);

   gtk_window_present (GTK_WINDOW (sw.window));
}
