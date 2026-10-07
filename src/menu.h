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
 * $Id: menu.h,v 1.10 2003/06/13 09:43:37 makeinu Exp $
 */

/*
 *  Menu helpers.
 *
 *  GTK2's GtkItemFactory is gone in GTK4.  This module keeps its table based
 *  interface (GimvMenuEntry has the same layout as GtkItemFactoryEntry) and
 *  builds GMenuModel + GAction based menus from it.  Menus are represented
 *  by GtkPopoverMenuBar (menu bars) and GtkPopoverMenu (popups/submenus)
 *  widgets, menu items by GimvMenuItem objects.
 */

#ifndef __MENU_H__
#define __MENU_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"

G_BEGIN_DECLS

#define GIMV_TYPE_MENU_ITEM            (gimv_menu_item_get_type ())
#define GIMV_MENU_ITEM(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GIMV_TYPE_MENU_ITEM, GimvMenuItem))
#define GIMV_IS_MENU_ITEM(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GIMV_TYPE_MENU_ITEM))

typedef struct _GimvMenuItem      GimvMenuItem;
typedef struct _GimvMenuItemClass GimvMenuItemClass;

/* same as GtkItemFactoryCallback1 */
typedef void (*GimvMenuCallback) (gpointer      data,
                                  guint         action,
                                  GimvMenuItem *item);

/* handler for items added with gimv_menu_append_*()
   (compatible with the GTK2 "activate" signal of GtkMenuItem) */
typedef void (*GimvMenuActivateFunc) (GimvMenuItem *item,
                                      gpointer      data);

/*
 *  As in GtkItemFactoryEntry the callback is not type checked: it is called
 *  as a GimvMenuCallback, (data, callback_action, item).
 */
typedef struct _GimvMenuEntry {
   const gchar      *path;
   const gchar      *accelerator;
   gpointer          callback;
   guint             callback_action;
   /* "<Item>", "<Title>", "<CheckItem>", "<ToggleItem>", "<RadioItem>",
      a path of a radio item to link against, "<Separator>", "<Tearoff>",
      "<Branch>", "<LastBranch>", "<StockItem>", "<ImageItem>" */
   const gchar      *item_type;
   gconstpointer     extra_data;
} GimvMenuEntry;

typedef enum {
   GIMV_MENU_ITEM_NORMAL,
   GIMV_MENU_ITEM_CHECK,
   GIMV_MENU_ITEM_RADIO,
   GIMV_MENU_ITEM_BRANCH,
   GIMV_MENU_ITEM_SEPARATOR,
   GIMV_MENU_ITEM_TITLE
} GimvMenuItemType;


GType         gimv_menu_item_get_type        (void);
gboolean      gimv_menu_item_get_active      (GimvMenuItem *item);
/* like gtk_check_menu_item_set_active(): calls the item's callback
   when the state was changed */
void          gimv_menu_item_set_active      (GimvMenuItem *item,
                                              gboolean      active);
void          gimv_menu_item_set_sensitive   (GimvMenuItem *item,
                                              gboolean      sensitive);
gboolean      gimv_menu_item_get_sensitive   (GimvMenuItem *item);
void          gimv_menu_item_set_visible     (GimvMenuItem *item,
                                              gboolean      visible);
void          gimv_menu_item_set_label       (GimvMenuItem *item,
                                              const gchar  *label);
const gchar  *gimv_menu_item_get_label       (GimvMenuItem *item);
/* returns the menu that contains the item */
GtkWidget    *gimv_menu_item_get_menu        (GimvMenuItem *item);
void          gimv_menu_item_set_submenu     (GimvMenuItem *item,
                                              GtkWidget    *submenu);
GtkWidget    *gimv_menu_item_get_submenu     (GimvMenuItem *item);
void          gimv_menu_item_set_icon        (GimvMenuItem *item,
                                              GIcon        *icon);
void          gimv_menu_item_activate        (GimvMenuItem *item);


/* table based construction (GtkItemFactory replacement) */
gint          menu_count_ifactory_entry_num  (GimvMenuEntry *entries);
GtkWidget    *menubar_create                 (GtkWidget     *window,
                                              GimvMenuEntry *entries,
                                              guint          n_entries,
                                              const gchar   *path,
                                              gpointer       data);
GtkWidget    *menu_create_items              (GtkWidget     *window,
                                              GimvMenuEntry *entries,
                                              guint          n_entries,
                                              const gchar   *path,
                                              gpointer       data);

/* path based access */
GimvMenuItem *gimv_menu_get_item             (GtkWidget    *menu,
                                              const gchar  *path);
void          menu_item_set_sensitive        (GtkWidget    *menu,
                                              const gchar  *path,
                                              gboolean      sensitive);
void          menu_check_item_set_active     (GtkWidget    *menu,
                                              const gchar  *path,
                                              gboolean      active);
gboolean      menu_check_item_get_active     (GtkWidget    *menu,
                                              const gchar  *path);
void          menu_set_submenu               (GtkWidget    *menu,
                                              const gchar  *path,
                                              GtkWidget    *submenu);
GtkWidget    *menu_get_submenu               (GtkWidget    *menu,
                                              const gchar  *path);
void          menu_remove_submenu            (GtkWidget    *menu,
                                              const gchar  *path,
                                              GtkWidget    *submenu);

/* imperative construction (GtkMenu/GtkMenuItem replacement) */
GtkWidget    *gimv_menu_new                  (GtkWidget    *window);
GimvMenuItem *gimv_menu_append_item          (GtkWidget    *menu,
                                              const gchar  *label,
                                              GimvMenuActivateFunc func,
                                              gpointer      data);
GimvMenuItem *gimv_menu_append_check_item    (GtkWidget    *menu,
                                              const gchar  *label,
                                              gboolean      active,
                                              GimvMenuActivateFunc func,
                                              gpointer      data);
/* group_member: NULL to start a new group */
GimvMenuItem *gimv_menu_append_radio_item    (GtkWidget    *menu,
                                              GimvMenuItem *group_member,
                                              const gchar  *label,
                                              GimvMenuActivateFunc func,
                                              gpointer      data);
GimvMenuItem *gimv_menu_append_submenu       (GtkWidget    *menu,
                                              const gchar  *label,
                                              GtkWidget    *submenu);
void          gimv_menu_append_separator     (GtkWidget    *menu);
GList        *gimv_menu_get_items            (GtkWidget    *menu); /* free the list */
void          gimv_menu_clear                (GtkWidget    *menu);
gboolean      gimv_is_menu                   (GtkWidget    *widget);

/* show a popup menu at (x, y) of relative_to.  (-1, -1): at the pointer */
void          gimv_menu_popup                (GtkWidget    *menu,
                                              GtkWidget    *relative_to,
                                              gdouble       x,
                                              gdouble       y);
void          gimv_menu_popdown              (GtkWidget    *menu);
/* release a menu created by menu_create_items() or gimv_menu_new() */
void          gimv_menu_destroy              (GtkWidget    *menu);

/* key bindings of the menus (GTK2: gtk_accel_map_load () / _save ()) */
void          gimv_accel_map_load            (const gchar  *filename);
void          gimv_accel_map_save            (const gchar  *filename);


/* option menu helpers */
GtkWidget    *create_option_menu_simple      (const gchar **menu_items,
                                              gint          def_val,
                                              gint         *data);
GtkWidget    *create_option_menu             (const gchar **menu_items,
                                              gint          def_val,
                                              gpointer      func,
                                              gpointer      data);


/* modal popup: returns callback_action of the selected entry or -1 */
void          menu_modal_cb                  (gpointer      data,
                                              guint         action,
                                              GimvMenuItem *item);
gint          menu_popup_modal               (GtkWidget    *popup,
                                              GtkWidget    *relative_to,
                                              gdouble       x,
                                              gdouble       y);

G_END_DECLS

#endif /* __MENU_H__ */
