/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001 Takuro Ashie
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
 *
 * $Id: menu.c,v 1.19 2003/06/13 09:43:37 makeinu Exp $
 */

/*
 *  GTK4 port: GtkItemFactory replacement built on GMenuModel/GAction.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <gtk/gtk.h>

#include "intl.h"
#include "menu.h"
#include "gimv_gtk4_compat.h"


typedef struct _GimvMenu GimvMenu;

typedef struct {
   GList        *members;
   GimvMenuItem *active;
   gchar        *action_name;
   gint          ref_count;
} RadioGroup;

struct _GimvMenu {
   gchar              *prefix;
   GSimpleActionGroup *actions;
   GHashTable         *items;        /* normalized path -> GimvMenuItem */
   GList              *all_items;    /* owned references */
   GimvMenuItem       *root;
   GtkWidget          *widget;       /* GtkPopoverMenuBar or GtkPopoverMenu */
   GtkWidget          *window;
   gpointer            data;
   gboolean            is_menubar;
   guint               action_seq;
   GList              *inserted_into; /* widgets our group was inserted into */
   gchar              *accel_prefix;  /* e.g. "<ThumbWinMainMenu>" */
};

struct _GimvMenuItem {
   GObject           parent;

   GimvMenu         *menu;
   GimvMenuItemType  type;
   gchar            *path;
   gchar            *label;
   gchar            *accel;
   gchar            *action_name;   /* without prefix */
   gchar            *target;        /* radio items */
   RadioGroup       *radio;

   GimvMenuCallback  callback;
   guint             callback_action;
   GimvMenuActivateFunc activate_func;
   gpointer          activate_data;

   gboolean          active;
   gboolean          sensitive;
   gboolean          visible;

   GimvMenuItem     *parent_item;
   GList            *children;      /* branches */
   GMenu            *model;         /* branches */
   GtkWidget        *submenu;       /* external submenu */
   GIcon            *icon;          /* see gimv_menu_item_set_icon () */
};

struct _GimvMenuItemClass {
   GObjectClass parent_class;
};

#define GIMV_MENU_KEY "gimv-menu"

static guint menu_seq = 0;

/* GimvMenus whose widget displays their model */
static GList   *attached_menus  = NULL;

static void gimv_menu_item_finalize (GObject *object);
static void rebuild_branch          (GimvMenuItem *branch);
static void rebuild_branch_real     (GimvMenuItem *branch);
static void item_invoke             (GimvMenuItem *item);


G_DEFINE_TYPE (GimvMenuItem, gimv_menu_item, G_TYPE_OBJECT)


static void
gimv_menu_item_class_init (GimvMenuItemClass *klass)
{
   G_OBJECT_CLASS (klass)->finalize = gimv_menu_item_finalize;
}


static void
gimv_menu_item_init (GimvMenuItem *item)
{
   item->sensitive = TRUE;
   item->visible   = TRUE;
}


static void
radio_group_unref (RadioGroup *group)
{
   if (!group) return;
   if (--group->ref_count > 0) return;
   g_list_free (group->members);
   g_free (group->action_name);
   g_free (group);
}


static void
gimv_menu_item_finalize (GObject *object)
{
   GimvMenuItem *item = GIMV_MENU_ITEM (object);

   g_clear_object (&item->icon);
   g_free (item->path);
   g_free (item->label);
   g_free (item->accel);
   g_free (item->action_name);
   g_free (item->target);
   g_list_free (item->children);
   if (item->model)
      g_object_unref (item->model);
   if (item->submenu)
      g_object_unref (item->submenu);
   if (item->radio) {
      item->radio->members = g_list_remove (item->radio->members, item);
      if (item->radio->active == item)
         item->radio->active = NULL;
      radio_group_unref (item->radio);
   }

   G_OBJECT_CLASS (gimv_menu_item_parent_class)->finalize (object);
}



/******************************************************************************
 *
 *   GimvMenu (private)
 *
 ******************************************************************************/
static void
cb_inserted_widget_destroyed (gpointer data, GObject *where_the_object_was)
{
   GimvMenu *menu = data;
   menu->inserted_into = g_list_remove (menu->inserted_into, where_the_object_was);
}


static void
gimv_menu_free (gpointer data)
{
   GimvMenu *menu = data;
   GList *node;

   attached_menus = g_list_remove (attached_menus, menu);

   if (menu->window) {
      g_object_remove_weak_pointer (G_OBJECT (menu->window),
                                    (gpointer *) &menu->window);
      menu->window = NULL;
   }

   for (node = menu->inserted_into; node; node = g_list_next (node)) {
      g_object_weak_unref (node->data, cb_inserted_widget_destroyed, menu);
   }
   g_list_free (menu->inserted_into);

   for (node = menu->all_items; node; node = g_list_next (node)) {
      GimvMenuItem *item = node->data;
      item->menu = NULL;
      g_object_unref (item);
   }
   g_list_free (menu->all_items);
   g_hash_table_destroy (menu->items);
   g_object_unref (menu->actions);
   g_free (menu->prefix);
   g_free (menu->accel_prefix);
   g_free (menu);
}


static void
gimv_menu_insert_actions (GimvMenu *menu, GtkWidget *widget)
{
   if (!widget) return;
   if (g_list_find (menu->inserted_into, widget)) return;

   gtk_widget_insert_action_group (widget, menu->prefix,
                                   G_ACTION_GROUP (menu->actions));
   menu->inserted_into = g_list_prepend (menu->inserted_into, widget);
   g_object_weak_ref (G_OBJECT (widget), cb_inserted_widget_destroyed, menu);
}


static GimvMenu *
gimv_menu_get (GtkWidget *widget)
{
   if (!widget) return NULL;
   return g_object_get_data (G_OBJECT (widget), GIMV_MENU_KEY);
}


/*
 *  GtkPopoverMenu(Bar) doesn't cope well with changes of its GMenuModel
 *  while it's attached (submenu pages are added to the internal GtkStack
 *  again -> "duplicate child name in GtkStack" and stale pages).  So all
 *  menu widgets are detached from their models while models are rebuilt
 *  (see menus_flush()).
 */
static void
menu_widget_set_model (GimvMenu *menu, GMenuModel *model)
{
   if (!menu->widget) return;
   if (GTK_IS_POPOVER_MENU_BAR (menu->widget))
      gtk_popover_menu_bar_set_menu_model (GTK_POPOVER_MENU_BAR (menu->widget), model);
   else if (GTK_IS_POPOVER_MENU (menu->widget))
      gtk_popover_menu_set_menu_model (GTK_POPOVER_MENU (menu->widget), model);
}


static GList *pending_branches = NULL;   /* referenced GimvMenuItems */
static guint  pending_id       = 0;


/* rebuild all pending branches now (detached from the widgets) */
static void
menus_flush (void)
{
   GList *node, *list, *affected = NULL;

   if (pending_id) {
      g_source_remove (pending_id);
      pending_id = 0;
   }
   if (!pending_branches) return;

   /* Detach only the menu widgets whose models are going to change.
      Resetting the model of every menu bar (e.g. when a new window builds
      its own menus) makes a GtkPopoverMenuBar that just closed a popover
      open its first menu again. */
   for (node = pending_branches; node; node = g_list_next (node)) {
      GimvMenuItem *branch = node->data;
      if (branch->menu && g_list_find (attached_menus, branch->menu)
          && !g_list_find (affected, branch->menu))
      {
         affected = g_list_prepend (affected, branch->menu);
      }
   }

   for (node = affected; node; node = g_list_next (node))
      menu_widget_set_model (node->data, NULL);

   /* rebuilding may queue further branches */
   while (pending_branches) {
      list = pending_branches;
      pending_branches = NULL;
      for (node = list; node; node = g_list_next (node)) {
         rebuild_branch_real (node->data);
         g_object_unref (node->data);
      }
      g_list_free (list);
   }

   for (node = affected; node; node = g_list_next (node)) {
      GimvMenu *menu = node->data;
      if (g_list_find (attached_menus, menu))
         menu_widget_set_model (menu, G_MENU_MODEL (menu->root->model));
   }
   g_list_free (affected);
}


static gboolean
idle_menus_flush (gpointer data)
{
   pending_id = 0;
   menus_flush ();
   return G_SOURCE_REMOVE;
}


/*
 *  Rebuilding is deferred to an idle callback: menu contents are often
 *  changed from a menu item's "activate" handler, and replacing the model
 *  of a menu widget while it handles a click breaks GTK's state tracking.
 */
static void
rebuild_branch (GimvMenuItem *branch)
{
   if (!branch || !branch->model) return;

   if (!attached_menus) {
      /* no menu widget displays anything yet */
      rebuild_branch_real (branch);
      return;
   }

   if (g_list_find (pending_branches, branch)) return;
   pending_branches = g_list_append (pending_branches, g_object_ref (branch));

   if (!pending_id)
      pending_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE, idle_menus_flush,
                                    NULL, NULL);
}


static void
menu_attach_widget (GimvMenu *menu)
{
   attached_menus = g_list_prepend (attached_menus, menu);
}


/* remove mnemonic underscores: "/_File/_Open..." -> "/File/Open..." */
static gchar *
normalize_path (const gchar *path)
{
   GString *str;
   const gchar *p;

   if (!path) return g_strdup ("");

   str = g_string_new (NULL);
   for (p = path; *p; p++) {
      if (*p == '_') {
         if (p[1] == '_') {
            g_string_append_c (str, '_');
            p++;
         }
         continue;
      }
      g_string_append_c (str, *p);
   }

   /* strip trailing slash */
   if (str->len > 1 && str->str[str->len - 1] == '/')
      g_string_truncate (str, str->len - 1);

   return g_string_free (str, FALSE);
}


static gchar *
path_dirname (const gchar *path)
{
   const gchar *p = strrchr (path, '/');
   if (!p || p == path) return g_strdup ("");
   return g_strndup (path, p - path);
}


static const gchar *
path_basename (const gchar *path)
{
   const gchar *p = strrchr (path, '/');
   return p ? p + 1 : path;
}


static gchar *
translate_path (const gchar *path)
{
#ifdef ENABLE_NLS
   const gchar *t = gettext (path);
   /* translation must keep the same depth */
   if (t && t != path) {
      gint n1 = 0, n2 = 0;
      const gchar *p;
      for (p = path; *p; p++) if (*p == '/') n1++;
      for (p = t; *p; p++) if (*p == '/') n2++;
      if (n1 == n2) return g_strdup (t);
   }
#endif
   return g_strdup (path);
}


static GimvMenu *
gimv_menu_new_real (GtkWidget *window, gboolean menubar, gpointer data)
{
   GimvMenu *menu = g_new0 (GimvMenu, 1);
   GimvMenuItem *root;

   menu->prefix  = g_strdup_printf ("gimvmenu%u", ++menu_seq);
   menu->actions = g_simple_action_group_new ();
   menu->items   = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   menu->window  = window;
   /* the window may be finalized before the menu (a menu bar whose last
      reference is dropped later, e.g. by idle_restore_target ()) */
   if (window)
      g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &menu->window);
   menu->data    = data;
   menu->is_menubar = menubar;

   root = g_object_new (GIMV_TYPE_MENU_ITEM, NULL);
   root->menu  = menu;
   root->type  = GIMV_MENU_ITEM_BRANCH;
   root->path  = g_strdup ("");
   root->label = g_strdup ("");
   root->model = g_menu_new ();
   menu->root = root;
   menu->all_items = g_list_prepend (menu->all_items, root);
   g_hash_table_insert (menu->items, g_strdup (""), root);

   if (window) {
      gimv_menu_insert_actions (menu, window);
   }

   return menu;
}


static gboolean
idle_item_invoke (gpointer data)
{
   GimvMenuItem *item = data;

   if (item->menu && item->sensitive)
      item_invoke (item);

   return G_SOURCE_REMOVE;
}


/*
 *  GTK4 (4.24, X11): a GtkModelButton closes its popovers before it
 *  activates the action, and while the popovers are unmapped GTK
 *  synthesizes crossing events on the toplevel at the pointer position
 *  inside the popover, taken as window coordinates.  If that lands on the
 *  menu bar, the bar opens one of its menus again (e.g. Help > Document >
 *  HTML > 01.html reopened "View", which then stayed on top of the web
 *  browser).  Close such menus when the action arrives; the bar ignores the
 *  pointer meanwhile so that closing doesn't reopen another one.
 */
static gboolean
idle_restore_target (gpointer data)
{
   GtkWidget *widget = data;

   gtk_widget_set_can_target (widget, TRUE);
   g_object_unref (widget);

   return G_SOURCE_REMOVE;
}


static gboolean
popdown_visible_popovers (GtkWidget *widget)
{
   GtkWidget *child;
   gboolean found = FALSE;

   if (GTK_IS_POPOVER (widget) && gtk_widget_get_visible (widget)) {
      gtk_popover_popdown (GTK_POPOVER (widget));
      return TRUE;
   }
   for (child = gtk_widget_get_first_child (widget); child;
        child = gtk_widget_get_next_sibling (child))
   {
      found |= popdown_visible_popovers (child);
   }

   return found;
}


static void
close_reopened_menus (void)
{
   GList *node;

   for (node = attached_menus; node; node = g_list_next (node)) {
      GimvMenu *menu = node->data;
      GtkWidget *bar = menu->widget;

      if (!menu->is_menubar || !bar || !gtk_widget_get_mapped (bar)) continue;
      if (!gtk_widget_get_can_target (bar)) continue;

      gtk_widget_set_can_target (bar, FALSE);
      popdown_visible_popovers (bar);
      g_idle_add (idle_restore_target, g_object_ref (bar));
   }
}


static void
cb_action_activate (GSimpleAction *action, GVariant *parameter, gpointer data)
{
   GimvMenuItem *item = data;

   if (!item->menu) return;

   close_reopened_menus ();

   switch (item->type) {
   case GIMV_MENU_ITEM_CHECK:
      gimv_menu_item_set_active (item, !item->active);
      break;
   case GIMV_MENU_ITEM_RADIO:
   {
      const gchar *target;
      GList *node;

      if (!parameter || !item->radio) break;
      target = g_variant_get_string (parameter, NULL);
      for (node = item->radio->members; node; node = g_list_next (node)) {
         GimvMenuItem *member = node->data;
         if (member->target && !strcmp (member->target, target)) {
            gimv_menu_item_set_active (member, TRUE);
            break;
         }
      }
      break;
   }
   default:
      /* GTK4: a GtkModelButton activates the action *before* it closes its
         popover menus.  GTK2 deactivated the menu first, and many callbacks
         open dialogs or run nested main loops, which would leave the menus
         on screen (and grabbing input) meanwhile.  Run the callback once
         the menus are closed, like GTK2 did. */
      g_idle_add_full (G_PRIORITY_HIGH_IDLE, idle_item_invoke,
                       g_object_ref (item), g_object_unref);
      break;
   }
}


static void
item_create_action (GimvMenuItem *item)
{
   GimvMenu *menu = item->menu;
   GSimpleAction *action = NULL;

   switch (item->type) {
   case GIMV_MENU_ITEM_NORMAL:
      item->action_name = g_strdup_printf ("a%u", ++menu->action_seq);
      action = g_simple_action_new (item->action_name, NULL);
      break;
   case GIMV_MENU_ITEM_CHECK:
      item->action_name = g_strdup_printf ("a%u", ++menu->action_seq);
      action = g_simple_action_new_stateful (item->action_name, NULL,
                                             g_variant_new_boolean (item->active));
      break;
   case GIMV_MENU_ITEM_RADIO:
      item->target = g_strdup_printf ("r%u", ++menu->action_seq);
      if (!item->radio->action_name) {
         item->radio->action_name = g_strdup_printf ("a%u", ++menu->action_seq);
         action = g_simple_action_new_stateful (item->radio->action_name,
                                                G_VARIANT_TYPE_STRING,
                                                g_variant_new_string (item->target));
         item->radio->active = item;
         item->active = TRUE;
      }
      item->action_name = g_strdup (item->radio->action_name);
      break;
   default:
      break;
   }

   if (action) {
      g_signal_connect (action, "activate", G_CALLBACK (cb_action_activate), item);
      g_action_map_add_action (G_ACTION_MAP (menu->actions), G_ACTION (action));
      g_object_unref (action);
   }
}


static GSimpleAction *
item_get_action (GimvMenuItem *item)
{
   if (!item->menu || !item->action_name) return NULL;
   return G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (item->menu->actions),
                                                       item->action_name));
}


static void
item_invoke (GimvMenuItem *item)
{
   GimvMenu *menu = item->menu;

   if (!menu) return;

   g_object_ref (item);
   if (item->callback)
      item->callback (menu->data, item->callback_action, item);
   if (item->activate_func)
      item->activate_func (item, item->activate_data);
   g_object_unref (item);
}


/******************************************************************************
 *
 *   Accelerator map.
 *
 *   GTK2 kept the accelerators of GtkItemFactory menus in the GtkAccelMap,
 *   which was loaded from / saved to ~/.gimv/keyaccelrc, so users could
 *   change the key bindings of the menus.  This is the same with the same
 *   file format:
 *
 *     ; (gtk_accel_path "<ThumbWinMainMenu>/File/Open..." "<Control>f")
 *     (gtk_accel_path "<ImageWinMainMenu>/File/Close" "<Control>w")
 *
 *   Commented lines are defaults, others are changed bindings.
 *
 ******************************************************************************/
typedef struct {
   gchar    *accel;          /* current binding ("" = none) */
   gchar    *default_accel;  /* NULL: path not used by any menu (yet) */
} AccelEntry;

static GHashTable *accel_map = NULL;   /* accel path -> AccelEntry */


/* GTK4 port: menu items renamed when their English was corrected;
   ~/.gimv/keyaccelrc may still name them by their old paths.  A path
   matches when it ends with the old item path ("<Prefix>" + path). */
static const struct {
   const gchar *old_path;
   const gchar *new_path;
} renamed_menu_paths[] = {
   { "/Load Thumbnail", "/Load Thumbnails" },
   { "/Load Thumbnail recursively", "/Load Thumbnails Recursively" },
   { "/Load Thumbnail recursively in one tab", "/Load Thumbnails Recursively in One Tab" },
   { "/Go to here", "/Go Here" },
   { "/Property...", "/Properties..." },
   { "/Open in new tab", "/Open in New Tab" },
   { "/View modes", "/View Modes" },
   { "/Keep aspect ratio", "/Keep Aspect Ratio" },
   { "/Rotate 90 degrees CW", "/Rotate 90 Degrees Clockwise" },
   { "/Rotate 90 degrees CCW", "/Rotate 90 Degrees Counterclockwise" },
   { "/Rotate 180 degrees", "/Rotate 180 Degrees" },
   { "/Continuance", "/Continuous Play" },
   { "/Move", "/Go" },
   { "/Menu Bar", "/Menubar" },
   { "/Tool Bar", "/Toolbar" },
   { "/Slide Show Player", "/Slideshow Player" },
   { "/Status Bar", "/Statusbar" },
   { "/Scroll Bar", "/Scrollbar" },
   { "/Full Screen", "/Fullscreen" },
   { "/Edit/Update All Thumbnail", "/Edit/Update All Thumbnails" },
   { "/Edit/Remove file...", "/Edit/Remove Files..." },
   { "/Tab/Move tab forward", "/Tab/Move Tab Forward" },
   { "/Tab/Move tab backward", "/Tab/Move Tab Backward" },
   { "/Tab/Detach tab", "/Tab/Detach Tab" },
   { "/Tool", "/Tools" },
   { "/Tool/Clear all cache", "/Tools/Clear All Caches" },
   { "/Tool/Find duplicates", "/Tools/Find Duplicates" },
   { "/Tool/Wallpaper setting", "/Tools/Set as Wallpaper" },
   { "/Slideshow Options/Start from the first", "/Slideshow Options/Start from the First" },
   { "/Slideshow Options/Start from the selected", "/Slideshow Options/Start from the Selected" },
   { "/Slideshow Options/Random order", "/Slideshow Options/Random Order" },
   { "/Slideshow Options/Selected only", "/Slideshow Options/Selected Only" },
   { "/Slideshow Options/Images and movies", "/Slideshow Options/Images and Movies" },
   { "/Slideshow Options/Images only", "/Slideshow Options/Images Only" },
   { "/Slideshow Options/Movies only", "/Slideshow Options/Movies Only" },
   { "/DirView Tool Bar", "/DirView Toolbar" },
   { "/Case insensitive", "/Case Insensitive" },
   { "/Directory insensitive", "/Mix Directories and Files" },
   { "/Remove file...", "/Remove Files..." },
};


static gchar *
accel_path_migrate (const gchar *path)
{
   const gchar *start = strchr (path, '>');
   gsize best_len = 0;
   gint best = -1;
   guint i;

   if (!start) return g_strdup (path);
   start++;

   /* the item itself, or an item in a renamed submenu (longest match) */
   for (i = 0; i < G_N_ELEMENTS (renamed_menu_paths); i++) {
      const gchar *old = renamed_menu_paths[i].old_path;
      gsize len = strlen (old);

      if (len > best_len && !strncmp (start, old, len)
          && (!start[len] || start[len] == '/'))
      {
         best = i;
         best_len = len;
      }
   }
   if (best < 0) return g_strdup (path);

   return g_strdup_printf ("%.*s%s%s", (gint) (start - path), path,
                           renamed_menu_paths[best].new_path, start + best_len);
}


static void
accel_entry_free (gpointer data)
{
   AccelEntry *entry = data;
   g_free (entry->accel);
   g_free (entry->default_accel);
   g_free (entry);
}


static GHashTable *
accel_map_get (void)
{
   if (!accel_map)
      accel_map = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, accel_entry_free);
   return accel_map;
}


/* canonical accelerator name ("" for none or unparsable) */
static gchar *
accel_canonicalize (const gchar *accel)
{
   guint keyval;
   GdkModifierType mods;

   if (!accel || !*accel) return g_strdup ("");
   if (!gimv_accelerator_parse (accel, &keyval, &mods)) return g_strdup ("");
   return gtk_accelerator_name (keyval, mods);
}


/* the binding to use for accel_path; registers the default binding */
static const gchar *
accel_map_lookup (const gchar *accel_path, const gchar *default_accel)
{
   GHashTable *map = accel_map_get ();
   AccelEntry *entry = g_hash_table_lookup (map, accel_path);

   if (!entry) {
      entry = g_new0 (AccelEntry, 1);
      entry->accel = accel_canonicalize (default_accel);
      g_hash_table_insert (map, g_strdup (accel_path), entry);
   }
   if (!entry->default_accel)
      entry->default_accel = accel_canonicalize (default_accel);

   return entry->accel;
}


/* read a quoted string of a GtkAccelMap rc line, returns the end or NULL */
static const gchar *
accel_rc_parse_string (const gchar *p, gchar **str)
{
   GString *buf;

   *str = NULL;
   while (*p && *p != '"') p++;
   if (!*p) return NULL;
   p++;

   buf = g_string_new (NULL);
   for (; *p && *p != '"'; p++) {
      if (*p == '\\' && p[1]) p++;
      g_string_append_c (buf, *p);
   }
   if (*p != '"') {
      g_string_free (buf, TRUE);
      return NULL;
   }
   *str = g_string_free (buf, FALSE);
   return p + 1;
}


void
gimv_accel_map_load (const gchar *filename)
{
   gchar *contents = NULL, **lines;
   gint i;

   g_return_if_fail (filename);

   if (!g_file_get_contents (filename, &contents, NULL, NULL)) return;

   lines = g_strsplit (contents, "\n", -1);
   g_free (contents);

   for (i = 0; lines[i]; i++) {
      const gchar *p = lines[i];
      gchar *path = NULL, *accel = NULL;
      AccelEntry *entry;

      while (*p == ' ' || *p == '\t') p++;
      if (strncmp (p, "(gtk_accel_path", 15)) continue;   /* also comments */

      p = accel_rc_parse_string (p + 15, &path);
      if (p) p = accel_rc_parse_string (p, &accel);
      if (!path || !accel) {
         g_free (path);
         g_free (accel);
         continue;
      }

      {
         gchar *migrated = accel_path_migrate (path);
         g_free (path);
         path = migrated;
      }

      entry = g_hash_table_lookup (accel_map_get (), path);
      if (!entry) {
         entry = g_new0 (AccelEntry, 1);
         g_hash_table_insert (accel_map_get (), g_strdup (path), entry);
      }
      g_free (entry->accel);
      entry->accel = accel_canonicalize (accel);

      g_free (path);
      g_free (accel);
   }

   g_strfreev (lines);
}


void
gimv_accel_map_save (const gchar *filename)
{
   GString *out;
   GList *keys, *node;

   g_return_if_fail (filename);

   out = g_string_new (NULL);
   g_string_append_printf (out,
                           "; %s GtkAccelMap rc-file         -*- scheme -*-\n"
                           "; this file is an automated accelerator map dump\n"
                           ";\n",
                           g_get_prgname () ? g_get_prgname () : "gimv");

   keys = g_list_sort (g_hash_table_get_keys (accel_map_get ()),
                       (GCompareFunc) strcmp);
   for (node = keys; node; node = g_list_next (node)) {
      const gchar *path = node->data;
      AccelEntry *entry = g_hash_table_lookup (accel_map_get (), path);
      gchar *epath = g_strescape (path, NULL);
      gchar *eaccel = g_strescape (entry->accel, NULL);
      gboolean is_default = entry->default_accel
                            && !strcmp (entry->accel, entry->default_accel);

      g_string_append_printf (out, "%s(gtk_accel_path \"%s\" \"%s\")\n",
                              is_default ? "; " : "", epath, eaccel);
      g_free (epath);
      g_free (eaccel);
   }
   g_list_free (keys);

   if (!g_file_set_contents (filename, out->str, out->len, NULL))
      g_warning ("Can't save key accelerators to %s", filename);

   g_string_free (out, TRUE);
}


static void
shortcut_add (GimvMenuItem *item, const gchar *accel)
{
   GimvMenu *menu = item->menu;
   GtkEventController *controller;
   GtkShortcut *shortcut;
   GtkShortcutAction *action;
   guint keyval;
   GdkModifierType mods;
   gchar *action_name;

   /* user's binding from the accelerator map (GTK2: GtkAccelMap) */
   if (menu && menu->accel_prefix && item->path && *item->path
       && item->action_name)
   {
      gchar *accel_path = g_strconcat (menu->accel_prefix, item->path, NULL);
      accel = accel_map_lookup (accel_path, accel);
      g_free (accel_path);
   }

   if (!accel || !*accel) return;
   if (!gimv_accelerator_parse (accel, &keyval, &mods)) return;

   g_free (item->accel);
   item->accel = gtk_accelerator_name (keyval, mods);

   if (!menu->window || !item->action_name) return;

   controller = g_object_get_data (G_OBJECT (menu->window), "gimv-menu-shortcuts");
   if (!controller) {
      controller = gtk_shortcut_controller_new ();
      gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_BUBBLE);
      gtk_widget_add_controller (menu->window, controller);
      g_object_set_data (G_OBJECT (menu->window), "gimv-menu-shortcuts", controller);
   }

   action_name = g_strconcat (menu->prefix, ".", item->action_name, NULL);
   action = gtk_named_action_new (action_name);
   g_free (action_name);

   shortcut = gtk_shortcut_new (gtk_keyval_trigger_new (keyval, mods), action);
   if (item->type == GIMV_MENU_ITEM_RADIO && item->target)
      gtk_shortcut_set_arguments (shortcut, g_variant_new_string (item->target));
   gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (controller),
                                         shortcut);
}


static GimvMenuItem *
item_new (GimvMenu *menu, GimvMenuItem *parent, GimvMenuItemType type,
          const gchar *path, const gchar *label)
{
   GimvMenuItem *item = g_object_new (GIMV_TYPE_MENU_ITEM, NULL);

   item->menu  = menu;
   item->type  = type;
   item->path  = g_strdup (path);
   item->label = g_strdup (label ? label : "");
   item->parent_item = parent;

   if (type == GIMV_MENU_ITEM_BRANCH)
      item->model = g_menu_new ();

   menu->all_items = g_list_append (menu->all_items, item);
   if (path && *path && !g_hash_table_lookup (menu->items, path))
      g_hash_table_insert (menu->items, g_strdup (path), item);

   if (parent)
      parent->children = g_list_append (parent->children, item);

   return item;
}


static GMenuModel *
item_submenu_model (GimvMenuItem *item)
{
   if (item->submenu) {
      GimvMenu *sub = gimv_menu_get (item->submenu);
      if (sub) return G_MENU_MODEL (sub->root->model);
   }
   if (item->model) return G_MENU_MODEL (item->model);
   return NULL;
}


static GMenuItem *
item_make_gmenu_item (GimvMenuItem *item)
{
   GMenuItem *mi = NULL;
   GMenuModel *submodel = item_submenu_model (item);
   gchar *detailed;

   if (submodel) {
      mi = g_menu_item_new_submenu (item->label, submodel);
      return mi;
   }

   switch (item->type) {
   case GIMV_MENU_ITEM_NORMAL:
   case GIMV_MENU_ITEM_CHECK:
      detailed = g_strconcat (item->menu->prefix, ".", item->action_name, NULL);
      mi = g_menu_item_new (item->label, detailed);
      g_free (detailed);
      break;
   case GIMV_MENU_ITEM_RADIO:
      detailed = g_strconcat (item->menu->prefix, ".", item->action_name, NULL);
      mi = g_menu_item_new (item->label, NULL);
      g_menu_item_set_action_and_target_value (mi, detailed,
                                               g_variant_new_string (item->target));
      g_free (detailed);
      break;
   case GIMV_MENU_ITEM_TITLE:
   default:
      /* an item without action is displayed insensitive */
      mi = g_menu_item_new (item->label, "gimvmenu-none.none");
      break;
   }

   if (item->accel)
      g_menu_item_set_attribute (mi, "accel", "s", item->accel);

   if (item->icon) {
      GVariant *icon = g_icon_serialize (item->icon);
      guint key;
      GdkModifierType mods;

      /* the label is the tooltip of an icon button, which doesn't show
         the accelerator */
      if (item->accel && gtk_accelerator_parse (item->accel, &key, &mods) && key) {
         gchar *accel = gtk_accelerator_get_label (key, mods);
         gchar *label = g_strdup_printf ("%s (%s)", item->label ? item->label : "", accel);
         g_menu_item_set_label (mi, label);
         g_free (label);
         g_free (accel);
      }
      if (icon) {
         g_menu_item_set_attribute_value (mi, G_MENU_ATTRIBUTE_ICON, icon);
         g_menu_item_set_attribute_value (mi, "verb-icon", icon);
         g_variant_unref (icon);
      }
   }

   return mi;
}


/* a section whose items all have icons is shown as a row of icon buttons
   (GtkPopoverMenu "horizontal-buttons"; the label becomes the tooltip) */
static void
append_section (GMenu *model, GMenu *section, gboolean iconic)
{
   GMenuItem *mi;

   if (g_menu_model_get_n_items (G_MENU_MODEL (section)) <= 0) return;

   mi = g_menu_item_new_section (NULL, G_MENU_MODEL (section));
   if (iconic)
      g_menu_item_set_attribute (mi, "display-hint", "s", "horizontal-buttons");
   g_menu_append_item (model, mi);
   g_object_unref (mi);
}


static void
rebuild_branch_real (GimvMenuItem *branch)
{
   GMenu *section = NULL;
   GList *node;
   gboolean flat, iconic = FALSE;

   if (!branch || !branch->model) return;

   /* the top level of a menu bar must consist of submenus only */
   flat = branch->menu && branch->menu->is_menubar && branch == branch->menu->root;

   g_menu_remove_all (branch->model);

   for (node = branch->children; node; node = g_list_next (node)) {
      GimvMenuItem *child = node->data;
      GMenuItem *mi;

      if (!child->visible) continue;

      if (child->type == GIMV_MENU_ITEM_SEPARATOR) {
         if (flat) continue;
         if (section) {
            append_section (branch->model, section, iconic);
            g_object_unref (section);
            section = NULL;
         }
         continue;
      }

      mi = item_make_gmenu_item (child);
      if (flat) {
         g_menu_append_item (branch->model, mi);
      } else {
         if (!section) {
            section = g_menu_new ();
            iconic = TRUE;
         }
         if (!child->icon || child->submenu || child->model) iconic = FALSE;
         g_menu_append_item (section, mi);
      }
      g_object_unref (mi);
   }

   if (section) {
      append_section (branch->model, section, iconic);
      g_object_unref (section);
   }
}


static GimvMenuItem *
ensure_branch (GimvMenu *menu, const gchar *norm_path, const gchar *label_path)
{
   GimvMenuItem *item, *parent;
   gchar *parent_path, *parent_label_path;

   item = g_hash_table_lookup (menu->items, norm_path);
   if (item) {
      if (item->type != GIMV_MENU_ITEM_BRANCH && !item->model) {
         item->type = GIMV_MENU_ITEM_BRANCH;
         item->model = g_menu_new ();
      }
      return item;
   }

   parent_path = path_dirname (norm_path);
   parent_label_path = path_dirname (label_path);
   parent = ensure_branch (menu, parent_path, parent_label_path);
   item = item_new (menu, parent, GIMV_MENU_ITEM_BRANCH, norm_path,
                    path_basename (label_path));
   g_free (parent_path);
   g_free (parent_label_path);

   return item;
}


static void
gimv_menu_create_items (GimvMenu *menu, GimvMenuEntry *entries, guint n_entries)
{
   guint i;
   GHashTable *branches = g_hash_table_new (g_direct_hash, g_direct_equal);
   GList *list, *node;

   for (i = 0; i < n_entries && entries[i].path; i++) {
      GimvMenuEntry *entry = &entries[i];
      const gchar *type_str = entry->item_type;
      GimvMenuItemType type = GIMV_MENU_ITEM_NORMAL;
      gchar *norm_path, *label_path, *parent_path, *parent_label_path;
      GimvMenuItem *parent, *item;
      RadioGroup *radio = NULL;

      if (!type_str || !*type_str || !strcmp (type_str, "<Item>")
          || !strcmp (type_str, "<StockItem>") || !strcmp (type_str, "<ImageItem>"))
      {
         type = GIMV_MENU_ITEM_NORMAL;
      } else if (!strcmp (type_str, "<Title>")) {
         type = GIMV_MENU_ITEM_TITLE;
      } else if (!strcmp (type_str, "<CheckItem>") || !strcmp (type_str, "<ToggleItem>")) {
         type = GIMV_MENU_ITEM_CHECK;
      } else if (!strcmp (type_str, "<RadioItem>")) {
         type = GIMV_MENU_ITEM_RADIO;
      } else if (!strcmp (type_str, "<Separator>")) {
         type = GIMV_MENU_ITEM_SEPARATOR;
      } else if (!strcmp (type_str, "<Tearoff>")) {
         continue;
      } else if (!strcmp (type_str, "<Branch>") || !strcmp (type_str, "<LastBranch>")) {
         type = GIMV_MENU_ITEM_BRANCH;
      } else if (type_str[0] == '/') {
         /* link to the group of another radio item */
         gchar *link = normalize_path (type_str);
         GimvMenuItem *leader = g_hash_table_lookup (menu->items, link);
         g_free (link);
         type = GIMV_MENU_ITEM_RADIO;
         if (leader && leader->radio)
            radio = leader->radio;
      }

      norm_path  = normalize_path (entry->path);
      label_path = translate_path (entry->path);
      parent_path = path_dirname (norm_path);
      parent_label_path = path_dirname (label_path);
      parent = ensure_branch (menu, parent_path, parent_label_path);
      g_hash_table_insert (branches, parent, parent);

      if (type == GIMV_MENU_ITEM_BRANCH) {
         item = ensure_branch (menu, norm_path, label_path);
         g_free (item->label);
         item->label = g_strdup (path_basename (label_path));
      } else {
         item = item_new (menu, parent, type,
                          type == GIMV_MENU_ITEM_SEPARATOR ? NULL : norm_path,
                          path_basename (label_path));
      }

      item->callback        = (GimvMenuCallback) entry->callback;
      item->callback_action = entry->callback_action;

      if (type == GIMV_MENU_ITEM_RADIO) {
         if (!radio) {
            radio = g_new0 (RadioGroup, 1);
         }
         radio->ref_count++;
         radio->members = g_list_append (radio->members, item);
         item->radio = radio;
      }

      item_create_action (item);
      shortcut_add (item, entry->accelerator);

      g_free (norm_path);
      g_free (label_path);
      g_free (parent_path);
      g_free (parent_label_path);
   }

   /* build models; children first is not necessary since submenus are linked */
   list = g_hash_table_get_keys (branches);
   for (node = list; node; node = g_list_next (node))
      rebuild_branch (node->data);
   g_list_free (list);
   for (node = menu->all_items; node; node = g_list_next (node)) {
      GimvMenuItem *item = node->data;
      if (item->type == GIMV_MENU_ITEM_BRANCH && !g_hash_table_lookup (branches, item))
         rebuild_branch (item);
   }
   rebuild_branch (menu->root);

   g_hash_table_destroy (branches);
}


static void
cb_window_destroy_release_menu (GtkWidget *window, GtkWidget *popover)
{
   gimv_menu_destroy (popover);
}


static void
cb_popover_destroy (GtkWidget *popover, gpointer data)
{
   GimvMenu *menu = gimv_menu_get (popover);
   if (menu && menu->window)
      g_signal_handlers_disconnect_by_func (menu->window,
                                            cb_window_destroy_release_menu,
                                            popover);
}


static GtkWidget *
popover_new_for_menu (GimvMenu *menu)
{
   GtkWidget *popover;

   popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu->root->model));
   gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
   gtk_widget_set_halign (popover, GTK_ALIGN_START);
   g_object_ref_sink (popover);
   menu->widget = popover;
   g_object_set_data_full (G_OBJECT (popover), GIMV_MENU_KEY, menu, gimv_menu_free);
   gimv_menu_insert_actions (menu, popover);
   menu_attach_widget (menu);

   /* the reference taken above is released when the window is destroyed or
      by gimv_menu_destroy() */
   if (menu->window) {
      g_signal_connect (menu->window, "destroy",
                        G_CALLBACK (cb_window_destroy_release_menu), popover);
   }
   g_signal_connect (popover, "destroy", G_CALLBACK (cb_popover_destroy), NULL);

   return popover;
}



/******************************************************************************
 *
 *   public: GimvMenuItem
 *
 ******************************************************************************/
gboolean
gimv_menu_item_get_active (GimvMenuItem *item)
{
   g_return_val_if_fail (GIMV_IS_MENU_ITEM (item), FALSE);
   return item->active;
}


void
gimv_menu_item_set_active (GimvMenuItem *item, gboolean active)
{
   GSimpleAction *action;

   g_return_if_fail (GIMV_IS_MENU_ITEM (item));

   active = active ? TRUE : FALSE;

   switch (item->type) {
   case GIMV_MENU_ITEM_CHECK:
      if (item->active == active) return;
      item->active = active;
      action = item_get_action (item);
      if (action)
         g_simple_action_set_state (action, g_variant_new_boolean (active));
      item_invoke (item);
      break;

   case GIMV_MENU_ITEM_RADIO:
   {
      GimvMenuItem *prev = item->radio ? item->radio->active : NULL;

      if (!active) {
         /* GTK2 does not allow to deactivate the active radio item */
         return;
      }
      if (item->active && prev == item) return;

      if (prev && prev != item)
         prev->active = FALSE;
      item->active = TRUE;
      if (item->radio)
         item->radio->active = item;

      action = item_get_action (item);
      if (action)
         g_simple_action_set_state (action, g_variant_new_string (item->target));
      item_invoke (item);
      break;
   }

   default:
      break;
   }
}


void
gimv_menu_item_set_sensitive (GimvMenuItem *item, gboolean sensitive)
{
   GSimpleAction *action;

   g_return_if_fail (GIMV_IS_MENU_ITEM (item));

   item->sensitive = sensitive ? TRUE : FALSE;
   action = item_get_action (item);
   if (action)
      g_simple_action_set_enabled (action, item->sensitive);
}


gboolean
gimv_menu_item_get_sensitive (GimvMenuItem *item)
{
   g_return_val_if_fail (GIMV_IS_MENU_ITEM (item), FALSE);
   return item->sensitive;
}


void
gimv_menu_item_set_visible (GimvMenuItem *item, gboolean visible)
{
   g_return_if_fail (GIMV_IS_MENU_ITEM (item));

   visible = visible ? TRUE : FALSE;
   if (item->visible == visible) return;
   item->visible = visible;
   rebuild_branch (item->parent_item);
}


void
gimv_menu_item_set_label (GimvMenuItem *item, const gchar *label)
{
   g_return_if_fail (GIMV_IS_MENU_ITEM (item));

   g_free (item->label);
   item->label = g_strdup (label ? label : "");
   rebuild_branch (item->parent_item);
}


const gchar *
gimv_menu_item_get_label (GimvMenuItem *item)
{
   g_return_val_if_fail (GIMV_IS_MENU_ITEM (item), NULL);
   return item->label;
}


GtkWidget *
gimv_menu_item_get_menu (GimvMenuItem *item)
{
   g_return_val_if_fail (GIMV_IS_MENU_ITEM (item), NULL);
   return item->menu ? item->menu->widget : NULL;
}


void
gimv_menu_item_set_submenu (GimvMenuItem *item, GtkWidget *submenu)
{
   GimvMenu *sub;

   g_return_if_fail (GIMV_IS_MENU_ITEM (item));

   if (item->submenu == submenu) return;

   if (item->submenu) {
      g_object_unref (item->submenu);
      item->submenu = NULL;
   }

   if (submenu) {
      sub = gimv_menu_get (submenu);
      g_return_if_fail (sub);

      item->submenu = g_object_ref (submenu);

      /* make the submenu's actions reachable from our widgets */
      if (item->menu) {
         if (item->menu->widget)
            gimv_menu_insert_actions (sub, item->menu->widget);
         if (item->menu->window)
            gimv_menu_insert_actions (sub, item->menu->window);
      }
   }

   rebuild_branch (item->parent_item);
}


/* GTK4 port: an icon for the item.  GtkPopoverMenu shows icons only in
   a "horizontal-buttons" section, so a group of items (between separators)
   that all have icons is shown as a row of icon buttons. */
void
gimv_menu_item_set_icon (GimvMenuItem *item, GIcon *icon)
{
   static gboolean css_done = FALSE;

   g_return_if_fail (GIMV_IS_MENU_ITEM (item));

   if (icon) g_object_ref (icon);
   g_clear_object (&item->icon);
   item->icon = icon;

   if (!css_done && gdk_display_get_default ()) {
      GtkCssProvider *provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (provider,
         "popover.menu button.model.image-button image { -gtk-icon-size: 36px; }\n"
         "popover.menu button.model.image-button { padding: 3px; }\n");
      gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                  GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_unref (provider);
      css_done = TRUE;
   }

   rebuild_branch (item->parent_item);
}


GtkWidget *
gimv_menu_item_get_submenu (GimvMenuItem *item)
{
   g_return_val_if_fail (GIMV_IS_MENU_ITEM (item), NULL);
   return item->submenu;
}


void
gimv_menu_item_activate (GimvMenuItem *item)
{
   g_return_if_fail (GIMV_IS_MENU_ITEM (item));

   switch (item->type) {
   case GIMV_MENU_ITEM_CHECK:
      gimv_menu_item_set_active (item, !item->active);
      break;
   case GIMV_MENU_ITEM_RADIO:
      gimv_menu_item_set_active (item, TRUE);
      break;
   case GIMV_MENU_ITEM_NORMAL:
      if (item->sensitive) item_invoke (item);
      break;
   default:
      break;
   }
}



/******************************************************************************
 *
 *   public: table based menus
 *
 ******************************************************************************/
gint
menu_count_ifactory_entry_num (GimvMenuEntry *entries)
{
   gint i;

   if (!entries) return -1;

   for (i = 0; entries[i].path; i++) {continue;}
   return i;
}


GtkWidget *
menubar_create (GtkWidget *window, GimvMenuEntry *entries, guint n_entries,
                const gchar *path, gpointer data)
{
   GimvMenu *menu;
   GtkWidget *menubar;

   menu = gimv_menu_new_real (window, TRUE, data);
   menu->accel_prefix = g_strdup (path);
   gimv_menu_create_items (menu, entries, n_entries);

   menubar = gtk_popover_menu_bar_new_from_model (G_MENU_MODEL (menu->root->model));
   menu->widget = menubar;
   g_object_set_data_full (G_OBJECT (menubar), GIMV_MENU_KEY, menu, gimv_menu_free);
   gimv_menu_insert_actions (menu, menubar);
   menu_attach_widget (menu);

   return menubar;
}


GtkWidget *
menu_create_items (GtkWidget *window, GimvMenuEntry *entries, guint n_entries,
                   const gchar *path, gpointer data)
{
   GimvMenu *menu;

   menu = gimv_menu_new_real (window, FALSE, data);
   menu->accel_prefix = g_strdup (path);
   gimv_menu_create_items (menu, entries, n_entries);

   return popover_new_for_menu (menu);
}


GimvMenuItem *
gimv_menu_get_item (GtkWidget *widget, const gchar *path)
{
   GimvMenu *menu = gimv_menu_get (widget);
   GimvMenuItem *item;
   gchar *norm;

   g_return_val_if_fail (menu, NULL);

   norm = normalize_path (path);
   item = g_hash_table_lookup (menu->items, norm);
   g_free (norm);

   if (!item)
      g_warning ("menu item \"%s\" not found", path);

   return item;
}


void
menu_item_set_sensitive (GtkWidget *widget, const gchar *path, gboolean sensitive)
{
   GimvMenuItem *item = gimv_menu_get_item (widget, path);
   if (item) gimv_menu_item_set_sensitive (item, sensitive);
}


void
menu_check_item_set_active (GtkWidget *widget, const gchar *path, gboolean active)
{
   GimvMenuItem *item = gimv_menu_get_item (widget, path);
   if (item) gimv_menu_item_set_active (item, active);
}


gboolean
menu_check_item_get_active (GtkWidget *widget, const gchar *path)
{
   GimvMenuItem *item = gimv_menu_get_item (widget, path);
   return item ? item->active : FALSE;
}


void
menu_set_submenu (GtkWidget *widget, const gchar *path, GtkWidget *submenu)
{
   GimvMenuItem *item = gimv_menu_get_item (widget, path);
   if (item) gimv_menu_item_set_submenu (item, submenu);
}


GtkWidget *
menu_get_submenu (GtkWidget *widget, const gchar *path)
{
   GimvMenuItem *item = gimv_menu_get_item (widget, path);
   return item ? item->submenu : NULL;
}


void
menu_remove_submenu (GtkWidget *widget, const gchar *path, GtkWidget *submenu)
{
   GimvMenuItem *item = gimv_menu_get_item (widget, path);
   if (item) gimv_menu_item_set_submenu (item, NULL);
}



/******************************************************************************
 *
 *   public: imperative menus
 *
 ******************************************************************************/
GtkWidget *
gimv_menu_new (GtkWidget *window)
{
   GimvMenu *menu = gimv_menu_new_real (window, FALSE, NULL);
   return popover_new_for_menu (menu);
}


static GimvMenuItem *
append_item_real (GtkWidget *widget, GimvMenuItemType type, const gchar *label,
                  GimvMenuItem *group_member, gboolean active,
                  GimvMenuActivateFunc func, gpointer data)
{
   GimvMenu *menu = gimv_menu_get (widget);
   GimvMenuItem *item;
   gchar *path;

   g_return_val_if_fail (menu, NULL);

   path = g_strdup_printf ("/%s", label ? label : "");
   item = item_new (menu, menu->root, type,
                    type == GIMV_MENU_ITEM_SEPARATOR ? NULL : path, label);
   g_free (path);

   item->activate_func = func;
   item->activate_data = data;
   item->active = active;

   if (type == GIMV_MENU_ITEM_RADIO) {
      RadioGroup *radio = group_member ? group_member->radio : NULL;
      if (!radio) radio = g_new0 (RadioGroup, 1);
      radio->ref_count++;
      radio->members = g_list_append (radio->members, item);
      item->radio = radio;
   }

   item_create_action (item);
   rebuild_branch (menu->root);

   return item;
}


GimvMenuItem *
gimv_menu_append_item (GtkWidget *menu, const gchar *label,
                       GimvMenuActivateFunc func, gpointer data)
{
   return append_item_real (menu, GIMV_MENU_ITEM_NORMAL, label, NULL, FALSE,
                            func, data);
}


GimvMenuItem *
gimv_menu_append_check_item (GtkWidget *menu, const gchar *label, gboolean active,
                             GimvMenuActivateFunc func, gpointer data)
{
   return append_item_real (menu, GIMV_MENU_ITEM_CHECK, label, NULL, active,
                            func, data);
}


GimvMenuItem *
gimv_menu_append_radio_item (GtkWidget *menu, GimvMenuItem *group_member,
                             const gchar *label,
                             GimvMenuActivateFunc func, gpointer data)
{
   return append_item_real (menu, GIMV_MENU_ITEM_RADIO, label, group_member,
                            FALSE, func, data);
}


GimvMenuItem *
gimv_menu_append_submenu (GtkWidget *menu, const gchar *label, GtkWidget *submenu)
{
   GimvMenuItem *item;

   item = append_item_real (menu, GIMV_MENU_ITEM_NORMAL, label, NULL, FALSE,
                            NULL, NULL);
   if (item && submenu)
      gimv_menu_item_set_submenu (item, submenu);

   return item;
}


void
gimv_menu_append_separator (GtkWidget *menu)
{
   append_item_real (menu, GIMV_MENU_ITEM_SEPARATOR, NULL, NULL, FALSE, NULL, NULL);
}


GList *
gimv_menu_get_items (GtkWidget *widget)
{
   GimvMenu *menu = gimv_menu_get (widget);
   GList *list = NULL, *node;

   g_return_val_if_fail (menu, NULL);

   for (node = menu->root->children; node; node = g_list_next (node)) {
      GimvMenuItem *item = node->data;
      if (item->type == GIMV_MENU_ITEM_SEPARATOR) continue;
      list = g_list_append (list, item);
   }

   return list;
}


void
gimv_menu_clear (GtkWidget *widget)
{
   GimvMenu *menu = gimv_menu_get (widget);
   GList *node;

   g_return_if_fail (menu);

   for (node = menu->root->children; node; node = g_list_next (node)) {
      GimvMenuItem *item = node->data;
      if (item->action_name
          && (!item->radio || item->radio->members->data == item))
      {
         g_action_map_remove_action (G_ACTION_MAP (menu->actions),
                                     item->action_name);
      }
      if (item->path)
         g_hash_table_remove (menu->items, item->path);
      item->parent_item = NULL;
   }
   g_list_free (menu->root->children);
   menu->root->children = NULL;

   rebuild_branch (menu->root);
}


gboolean
gimv_is_menu (GtkWidget *widget)
{
   return gimv_menu_get (widget) ? TRUE : FALSE;
}


static void
cb_relative_destroy (GtkWidget *relative, GtkWidget *popover)
{
   if (gtk_widget_get_parent (popover) == relative)
      gtk_widget_unparent (popover);
}


static gboolean
idle_menu_popup (gpointer data)
{
   GtkWidget *popover = data;

   if (gtk_widget_get_parent (popover))
      gtk_popover_popup (GTK_POPOVER (popover));

   return G_SOURCE_REMOVE;
}


void
gimv_menu_popup (GtkWidget *popover, GtkWidget *relative_to, gdouble x, gdouble y)
{
   GtkWidget *parent;
   GdkRectangle rect;

   g_return_if_fail (GTK_IS_POPOVER (popover));

   menus_flush ();

   if (!relative_to) {
      GimvMenu *menu = gimv_menu_get (popover);
      relative_to = menu ? menu->window : NULL;
   }
   g_return_if_fail (GTK_IS_WIDGET (relative_to));

   /* GTK4: a GtkTreeView keeps the CSS nodes of its children (column
      headers) below an internal header node, so gtk_widget_set_parent ()
      of a popover on a tree view with headers fails a CSS node assertion
      (GTK 4.14; its own type-ahead popover has the same problem).
      Attach the popover to the tree view's parent instead. */
   while (GTK_IS_TREE_VIEW (relative_to) && gtk_widget_get_parent (relative_to)) {
      GtkWidget *tv_parent = gtk_widget_get_parent (relative_to);
      graphene_point_t p = GRAPHENE_POINT_INIT (x, y), q;

      if (x >= 0 && y >= 0
          && gtk_widget_compute_point (relative_to, tv_parent, &p, &q))
      {
         x = q.x;
         y = q.y;
      } else if (x < 0 || y < 0) {
         gint px, py;
         if (gimv_widget_get_pointer (relative_to, &px, &py)) {
            p = GRAPHENE_POINT_INIT (px, py);
            if (gtk_widget_compute_point (relative_to, tv_parent, &p, &q)) {
               x = q.x;
               y = q.y;
            }
         }
      }
      relative_to = tv_parent;
   }

   parent = gtk_widget_get_parent (popover);
   if (parent != relative_to) {
      if (parent) {
         g_signal_handlers_disconnect_by_func (parent, cb_relative_destroy, popover);
         gtk_widget_unparent (popover);
      }
      gtk_widget_set_parent (popover, relative_to);
      g_signal_connect (relative_to, "destroy",
                        G_CALLBACK (cb_relative_destroy), popover);
   }

   if (x < 0 || y < 0) {
      gint px, py;
      if (gimv_widget_get_pointer (relative_to, &px, &py)) {
         x = px; y = py;
      } else {
         x = gtk_widget_get_width (relative_to) / 2;
         y = gtk_widget_get_height (relative_to) / 2;
      }
   }

   rect.x = x;
   rect.y = y;
   rect.width = rect.height = 1;
   gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
   gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);

   /* GTK adds the separators between the sections of a new popover menu
      from an idle callback, and a popover already shown keeps the size it
      got without them (GTK 4.14 and 4.18, X11): the last items of a
      context menu made just before were cut off.  Show it after that. */
   g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, idle_menu_popup,
                    g_object_ref (popover), g_object_unref);
}


void
gimv_menu_popdown (GtkWidget *popover)
{
   g_return_if_fail (GTK_IS_POPOVER (popover));
   gtk_popover_popdown (GTK_POPOVER (popover));
}


void
gimv_menu_destroy (GtkWidget *widget)
{
   GtkWidget *parent;
   GimvMenu *menu;

   if (!widget) return;

   if (!GTK_IS_POPOVER (widget)) {
      gimv_widget_destroy (widget);
      return;
   }

   menu = gimv_menu_get (widget);
   if (menu && menu->window) {
      g_signal_handlers_disconnect_by_func (menu->window,
                                            cb_window_destroy_release_menu,
                                            widget);
   }

   parent = gtk_widget_get_parent (widget);
   if (parent) {
      g_signal_handlers_disconnect_by_func (parent, cb_relative_destroy, widget);
      gtk_widget_unparent (widget);
   }

   /* the reference taken in popover_new_for_menu() */
   g_object_unref (widget);
}



/******************************************************************************
 *
 *   option menu
 *
 ******************************************************************************/
static void
cb_get_data_from_menuitem (GtkWidget *widget, gint *conf)
{
   *conf = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), "num"));
}


static const gchar **
translate_labels (const gchar **menu_items)
{
   gint i, n;
   const gchar **labels;

   for (n = 0; menu_items[n]; n++);
   labels = g_new0 (const gchar *, n + 1);
   for (i = 0; i < n; i++)
      labels[i] = _(menu_items[i]);

   return labels;
}


GtkWidget *
create_option_menu_simple (const gchar **menu_items, gint def_val, gint *data)
{
   const gchar **labels = translate_labels (menu_items);
   GtkWidget *option_menu;

   option_menu = gimv_option_menu_new (labels, -1, def_val,
                                       G_CALLBACK (cb_get_data_from_menuitem),
                                       data);
   g_free (labels);

   return option_menu;
}


GtkWidget *
create_option_menu (const gchar **menu_items, gint def_val,
                    gpointer func, gpointer data)
{
   const gchar **labels = translate_labels (menu_items);
   GtkWidget *option_menu;

   option_menu = gimv_option_menu_new (labels, -1, def_val, G_CALLBACK (func), data);
   gtk_widget_set_name (option_menu, "/ThumbWin/DispModeOptionMenu");
   g_free (labels);

   return option_menu;
}



/******************************************************************************
 *
 *   modal popup menu
 *
 ******************************************************************************/
void
menu_modal_cb (gpointer data, guint action, GimvMenuItem *item)
{
   GtkWidget *menu = gimv_menu_item_get_menu (item);

   if (menu)
      g_object_set_data (G_OBJECT (menu), "return_val", GINT_TO_POINTER (action));
}


static void
cb_modal_closed (GtkPopover *popover, GMainLoop *loop)
{
   if (g_main_loop_is_running (loop))
      g_main_loop_quit (loop);
}


/*
 *  menu_popup_modal:
 *     @runs the popup menu modally and returns the callback_action value of the
 *      selected item entry, or -1 if none..
 */
gint
menu_popup_modal (GtkWidget *popup, GtkWidget *relative_to, gdouble x, gdouble y)
{
   GMainLoop *loop;
   gulong id;
   gint retval;

   g_return_val_if_fail (GTK_IS_POPOVER (popup), -1);

   g_object_set_data (G_OBJECT (popup), "return_val", GINT_TO_POINTER (-1));

   loop = g_main_loop_new (NULL, FALSE);
   id = g_signal_connect (popup, "closed", G_CALLBACK (cb_modal_closed), loop);

   gimv_menu_popup (popup, relative_to, x, y);
   g_main_loop_run (loop);

   g_signal_handler_disconnect (popup, id);
   g_main_loop_unref (loop);

   /* the item's action may be activated right after the popover was closed */
   gimv_flush_events ();

   retval = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (popup), "return_val"));

   return retval;
}
