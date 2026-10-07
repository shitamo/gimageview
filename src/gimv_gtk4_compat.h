/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2004 Takuro Ashie
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
 *  Helpers used by the GTK4 port.
 *
 *  GTK4 removed a lot of API that GImageView was written against (GtkObject,
 *  GtkContainer, packing flags, GtkTable, GtkOptionMenu, nested gtk_main(),
 *  event signals, ...).  Rather than rewriting every call site by hand the
 *  port funnels those idioms through the small set of functions below, which
 *  are implemented purely on top of the GTK4 API.
 */

#ifndef __GIMV_GTK4_COMPAT_H__
#define __GIMV_GTK4_COMPAT_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*GimvCallback) (GtkWidget *widget, gpointer data);

/******************************************************************************
 *  main loop
 ******************************************************************************/
void       gimv_main                    (void);  /* nested main loop */
void       gimv_main_quit               (void);  /* quit innermost gimv_main() */
guint      gimv_main_level              (void);
#define    gimv_events_pending()        g_main_context_pending (NULL)
#define    gimv_main_iteration()        g_main_context_iteration (NULL, TRUE)
/* process all pending events (the old "while (gtk_events_pending ()) ..." idiom) */
void       gimv_flush_events            (void);
gboolean   gimv_flush_events_running    (void);

/******************************************************************************
 *  containers & packing
 ******************************************************************************/
GtkWidget *gimv_hbox_new                (gboolean     homogeneous,
                                         gint         spacing);
GtkWidget *gimv_vbox_new                (gboolean     homogeneous,
                                         gint         spacing);
void       gimv_box_pack_start          (GtkBox      *box,
                                         GtkWidget   *child,
                                         gboolean     expand,
                                         gboolean     fill,
                                         guint        padding);
void       gimv_box_pack_end            (GtkBox      *box,
                                         GtkWidget   *child,
                                         gboolean     expand,
                                         gboolean     fill,
                                         guint        padding);
void       gimv_box_reorder_child       (GtkBox      *box,
                                         GtkWidget   *child,
                                         gint         position);
void       gimv_container_add           (GtkWidget   *container,
                                         GtkWidget   *child);
void       gimv_container_remove        (GtkWidget   *container,
                                         GtkWidget   *child);
GList     *gimv_container_get_children  (GtkWidget   *container);
void       gimv_container_set_border_width (GtkWidget *widget,
                                         guint        width);
GtkWidget *gimv_bin_get_child           (GtkWidget   *bin);
void       gimv_widget_destroy          (GtkWidget   *widget);
void       gimv_widget_show_all         (GtkWidget   *widget);
void       gimv_widget_hide_all         (GtkWidget   *widget);
GtkWidget *gimv_widget_get_toplevel     (GtkWidget   *widget);
gboolean   gimv_widget_is_toplevel      (GtkWidget   *widget);
gboolean   gimv_widget_is_mapped        (GtkWidget   *widget);
gboolean   gimv_widget_is_visible       (GtkWidget   *widget);
gboolean   gimv_widget_is_realized      (GtkWidget   *widget);
void       gimv_widget_set_size         (GtkWidget   *widget,
                                         gint         width,
                                         gint         height);

/* GtkMisc / GtkAlignment replacements */
void       gimv_misc_set_alignment      (GtkWidget   *widget,
                                         gfloat       xalign,
                                         gfloat       yalign);
void       gimv_misc_set_padding        (GtkWidget   *widget,
                                         gint         xpad,
                                         gint         ypad);
GtkWidget *gimv_alignment_new           (gfloat       xalign,
                                         gfloat       yalign,
                                         gfloat       xscale,
                                         gfloat       yscale);
GtkWidget *gimv_event_box_new           (void);
GtkWidget *gimv_frame_new               (const gchar *label);

/* GtkTable replacement (implemented with GtkGrid) */
typedef enum {
   GIMV_EXPAND = 1 << 0,
   GIMV_SHRINK = 1 << 1,
   GIMV_FILL   = 1 << 2
} GimvAttachOptions;

GtkWidget *gimv_table_new               (guint        rows,
                                         guint        columns,
                                         gboolean     homogeneous);
void       gimv_table_attach            (GtkWidget   *table,
                                         GtkWidget   *child,
                                         guint        left_attach,
                                         guint        right_attach,
                                         guint        top_attach,
                                         guint        bottom_attach,
                                         GimvAttachOptions xoptions,
                                         GimvAttachOptions yoptions,
                                         guint        xpadding,
                                         guint        ypadding);
void       gimv_table_attach_defaults   (GtkWidget   *table,
                                         GtkWidget   *child,
                                         guint        left_attach,
                                         guint        right_attach,
                                         guint        top_attach,
                                         guint        bottom_attach);
#define    gimv_table_set_row_spacings(t, s) \
              gtk_grid_set_row_spacing (GTK_GRID (t), (s))
#define    gimv_table_set_col_spacings(t, s) \
              gtk_grid_set_column_spacing (GTK_GRID (t), (s))

void       gimv_container_foreach       (GtkWidget   *container,
                                         GimvCallback  callback,
                                         gpointer     data);

/* GtkPaned: gtk_paned_pack1/2 */
void       gimv_paned_pack1             (GtkPaned    *paned,
                                         GtkWidget   *child,
                                         gboolean     resize,
                                         gboolean     shrink);
void       gimv_paned_pack2             (GtkPaned    *paned,
                                         GtkWidget   *child,
                                         gboolean     resize,
                                         gboolean     shrink);

/* windows */
GtkWidget *gimv_popup_window_new        (void);  /* undecorated window */
void       gimv_window_set_default_transient (GtkWindow *window);
void       gimv_grab_add                (GtkWidget   *widget);
void       gimv_grab_remove             (GtkWidget   *widget);

/* entries */
GtkWidget *gimv_entry_new_with_max_length (gint     max);

/* scrolled window */
GtkWidget *gimv_scrolled_window_new     (GtkAdjustment *hadj,
                                         GtkAdjustment *vadj);

/******************************************************************************
 *  buttons
 ******************************************************************************/
/* work for both GtkToggleButton and GtkCheckButton (they are unrelated in GTK4) */
gboolean   gimv_toggle_get_active       (GtkWidget   *widget);
void       gimv_toggle_set_active       (GtkWidget   *widget,
                                         gboolean     active);
GtkWidget *gimv_radio_button_new_with_label (GSList   *group,
                                         const gchar *label);
GtkWidget *gimv_radio_button_new_with_label_from_widget (GtkWidget *group_member,
                                         const gchar *label);
GSList    *gimv_radio_button_get_group  (GtkWidget   *radio);
GtkWidget *gimv_button_new_with_mnemonic_label (const gchar *label);
GtkWidget *gimv_button_new_from_stock   (const gchar *stock_id);
GtkWidget *gimv_arrow_new               (GtkArrowType arrow_type);

/* GTK 4 has no stock labels: the labels are translated by gimv (the
   including file must provide _()) */
#define GIMV_STOCK_OK      _("_OK")
#define GIMV_STOCK_CANCEL  _("_Cancel")
#define GIMV_STOCK_APPLY   _("_Apply")
#define GIMV_STOCK_CLOSE   _("_Close")
#define GIMV_STOCK_YES     _("_Yes")
#define GIMV_STOCK_NO      _("_No")

/******************************************************************************
 *  option menu (GtkOptionMenu replacement, implemented with GtkDropDown)
 ******************************************************************************/
/*
 *  "func" is called as func (widget, data) when the user changes the
 *  selection.  The selected index is available as
 *  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), "num")).
 */
GtkWidget *gimv_option_menu_new         (const gchar **labels,  /* NULL terminated */
                                         gint          n_labels, /* -1 : count */
                                         gint          def_val,
                                         GCallback     func,
                                         gpointer      data);
void       gimv_option_menu_set_history (GtkWidget    *option_menu,
                                         gint          index);
gint       gimv_option_menu_get_history (GtkWidget    *option_menu);
void       gimv_option_menu_set_sensitive_item (GtkWidget *option_menu,
                                         gint          index,
                                         gboolean      sensitive);

/******************************************************************************
 *  combo box with entry (GtkCombo replacement)
 ******************************************************************************/
GtkWidget *gimv_combo_new               (void);
GtkWidget *gimv_combo_get_entry         (GtkWidget   *combo);
void       gimv_combo_set_popdown_strings (GtkWidget *combo,
                                         GList       *strings);

/******************************************************************************
 *  dialogs
 ******************************************************************************/
GtkWidget *gimv_dialog_get_vbox         (GtkWidget   *dialog);
GtkWidget *gimv_dialog_get_action_area  (GtkWidget   *dialog);
/* run a dialog modally, like gtk_dialog_run () of GTK2 */
gint       gimv_dialog_run              (GtkDialog   *dialog);
/* run until the window is closed/destroyed */
void       gimv_window_run_modal        (GtkWindow   *window);
void       gimv_window_set_icon_pixbuf  (GtkWindow   *window,
                                         GdkPixbuf   *pixbuf);

/******************************************************************************
 *  keyboard shortcuts
 ******************************************************************************/
typedef gboolean (*GimvShortcutFunc) (GtkWidget *widget, gpointer data);
void       gimv_widget_add_shortcut     (GtkWidget   *widget,
                                         guint        keyval,
                                         GdkModifierType mods,
                                         GimvShortcutFunc func,
                                         gpointer     data);
/* parse "<control>O", "<shift><alt>F1", "Page_Up" ... */
gboolean   gimv_accelerator_parse       (const gchar *accel,
                                         guint       *keyval,
                                         GdkModifierType *mods);

/******************************************************************************
 *  event emulation
 *
 *  GTK4 has no event signals any more.  The structures below carry the
 *  fields the GTK2 based code used from GdkEvent*, and gimv_event_connect()
 *  installs the proper event controllers and calls the handler with them.
 ******************************************************************************/
typedef enum {
   GIMV_EVENT_BUTTON_PRESS,     /* gboolean (*) (GtkWidget*, GimvEventButton*, gpointer) */
   GIMV_EVENT_BUTTON_RELEASE,   /* gboolean (*) (GtkWidget*, GimvEventButton*, gpointer) */
   GIMV_EVENT_MOTION_NOTIFY,    /* gboolean (*) (GtkWidget*, GimvEventMotion*, gpointer) */
   GIMV_EVENT_KEY_PRESS,        /* gboolean (*) (GtkWidget*, GimvEventKey*,    gpointer) */
   GIMV_EVENT_KEY_RELEASE,      /* gboolean (*) (GtkWidget*, GimvEventKey*,    gpointer) */
   GIMV_EVENT_SCROLL,           /* gboolean (*) (GtkWidget*, GimvEventScroll*, gpointer) */
   GIMV_EVENT_ENTER_NOTIFY,     /* gboolean (*) (GtkWidget*, GimvEventCrossing*, gpointer) */
   GIMV_EVENT_LEAVE_NOTIFY,     /* gboolean (*) (GtkWidget*, GimvEventCrossing*, gpointer) */
   GIMV_EVENT_FOCUS_IN,         /* gboolean (*) (GtkWidget*, gpointer, gpointer) */
   GIMV_EVENT_FOCUS_OUT,        /* gboolean (*) (GtkWidget*, gpointer, gpointer) */
   GIMV_EVENT_DELETE,           /* gboolean (*) (GtkWidget*, gpointer, gpointer); windows only */
   GIMV_EVENT_CONFIGURE,        /* gboolean (*) (GtkWidget*, GimvEventConfigure*, gpointer) */
   GIMV_EVENT_N_TYPES
} GimvEventSignal;

/* values of the "type" member of GimvEventButton */
#define GIMV_BUTTON_PRESS    GDK_BUTTON_PRESS
#define GIMV_2BUTTON_PRESS   ((GdkEventType) 1000)
#define GIMV_3BUTTON_PRESS   ((GdkEventType) 1001)
#define GIMV_BUTTON_RELEASE  GDK_BUTTON_RELEASE

typedef struct {
   GdkEventType    type;
   guint32         time;
   gdouble         x, y;          /* widget coordinates */
   gdouble         x_root, y_root;/* root (toplevel) coordinates */
   GdkModifierType state;
   guint           button;
   GdkEvent       *event;         /* underlying GTK4 event (may be NULL) */
   GtkEventController *controller;
} GimvEventButton;

typedef struct {
   GdkEventType    type;
   guint32         time;
   gdouble         x, y;
   gdouble         x_root, y_root;
   GdkModifierType state;
   gboolean        is_hint;       /* always FALSE */
   GdkEvent       *event;
   GtkEventController *controller;
} GimvEventMotion;

typedef struct {
   GdkEventType    type;
   guint32         time;
   guint           keyval;
   guint           hardware_keycode;
   GdkModifierType state;
   GdkEvent       *event;
   GtkEventController *controller;
} GimvEventKey;

typedef struct {
   GdkEventType       type;
   guint32            time;
   gdouble            x, y;
   GdkModifierType    state;
   GdkScrollDirection direction;
   gdouble            delta_x, delta_y;
   GdkEvent          *event;
   GtkEventController *controller;
} GimvEventScroll;

typedef struct {
   GdkEventType    type;
   gdouble         x, y;
   GdkModifierType state;
} GimvEventCrossing;

typedef struct {
   gint x, y, width, height;
} GimvEventConfigure;

gulong     gimv_event_connect           (GtkWidget      *widget,
                                         GimvEventSignal signal,
                                         GCallback       handler,
                                         gpointer        data);
gulong     gimv_event_connect_after     (GtkWidget      *widget,
                                         GimvEventSignal signal,
                                         GCallback       handler,
                                         gpointer        data);
void       gimv_event_disconnect_by_func (GtkWidget     *widget,
                                         GCallback       handler,
                                         gpointer        data);
void       gimv_event_block_by_func     (GtkWidget      *widget,
                                         GCallback       handler,
                                         gpointer        data);
void       gimv_event_unblock_by_func   (GtkWidget      *widget,
                                         GCallback       handler,
                                         gpointer        data);
/* current modifier state (replacement of gdk_window_get_pointer's mask) */
GdkModifierType gimv_get_current_modifier_state (GtkWidget *widget);
/* pointer position relative to widget. returns FALSE if unknown */
gboolean   gimv_widget_get_pointer      (GtkWidget      *widget,
                                         gint           *x,
                                         gint           *y);

/******************************************************************************
 *  drag and drop emulation
 *
 *  GTK2 style DnD (target entries, "drag_data_get"/"drag_data_received"
 *  handlers, selection data) implemented with GtkDragSource and
 *  GtkDropTargetAsync.
 ******************************************************************************/
#define GIMV_TARGET_SAME_APP    (1 << 0)
#define GIMV_TARGET_SAME_WIDGET (1 << 1)

typedef struct {
   const gchar *target;   /* mime type */
   guint        flags;
   guint        info;
} GimvTargetEntry;

typedef struct {
   const gchar *target;
   gint         format;
   guchar      *data;
   gint         length;
} GimvSelectionData;

typedef struct {
   GdkDragAction  actions;
   GdkDragAction  suggested_action;
   GdkDragAction  action;
   GdkDrag       *drag;           /* source side */
   GdkDrop       *drop;           /* destination side */
   GtkWidget     *source_widget;  /* only for drags inside the application */
   GtkWidget     *widget;
   /* private */
   GdkPaintable  *icon;
   gint           icon_hot_x, icon_hot_y;
   gboolean       status_set;
   gboolean       finished;
} GimvDragContext;

typedef enum {
   GIMV_DND_DRAG_BEGIN,          /* void     (*) (GtkWidget*, GimvDragContext*, gpointer) */
   GIMV_DND_DRAG_DATA_GET,       /* void     (*) (GtkWidget*, GimvDragContext*, GimvSelectionData*,
                                                  guint info, guint time, gpointer) */
   GIMV_DND_DRAG_DATA_DELETE,    /* void     (*) (GtkWidget*, GimvDragContext*, gpointer) */
   GIMV_DND_DRAG_END,            /* void     (*) (GtkWidget*, GimvDragContext*, gpointer) */
   GIMV_DND_DRAG_MOTION,         /* gboolean (*) (GtkWidget*, GimvDragContext*, gint x, gint y,
                                                  guint time, gpointer) */
   GIMV_DND_DRAG_LEAVE,          /* void     (*) (GtkWidget*, GimvDragContext*, guint time, gpointer) */
   GIMV_DND_DRAG_DATA_RECEIVED,  /* void     (*) (GtkWidget*, GimvDragContext*, gint x, gint y,
                                                  GimvSelectionData*, guint info, guint time,
                                                  gpointer) */
   GIMV_DND_N_SIGNALS
} GimvDndSignal;

void       gimv_drag_source_set         (GtkWidget             *widget,
                                         GdkModifierType        start_button_mask,
                                         const GimvTargetEntry *targets,
                                         gint                   n_targets,
                                         GdkDragAction          actions);
void       gimv_drag_source_unset       (GtkWidget             *widget);
void       gimv_drag_dest_set           (GtkWidget             *widget,
                                         guint                  flags,
                                         const GimvTargetEntry *targets,
                                         gint                   n_targets,
                                         GdkDragAction          actions);
void       gimv_drag_dest_unset         (GtkWidget             *widget);
gulong     gimv_dnd_connect             (GtkWidget             *widget,
                                         GimvDndSignal          signal,
                                         GCallback              handler,
                                         gpointer               data);
void       gimv_selection_data_set      (GimvSelectionData     *seldata,
                                         const gchar           *type,
                                         gint                   format,
                                         const guchar          *data,
                                         gint                   length);
GtkWidget *gimv_drag_get_source_widget  (GimvDragContext       *context);
void       gimv_drag_status             (GimvDragContext       *context,
                                         GdkDragAction          action,
                                         guint32                time);
void       gimv_drag_finish             (GimvDragContext       *context,
                                         gboolean               success,
                                         gboolean               del,
                                         guint32                time);
void       gimv_drag_set_icon_texture   (GimvDragContext       *context,
                                         GdkTexture            *texture,
                                         gint                   hot_x,
                                         gint                   hot_y);
/* GdkContentFormats for a target list (for GtkTreeView model DnD) */
GdkContentFormats *gimv_target_entries_to_formats (const GimvTargetEntry *targets,
                                                   gint                   n_targets);

/******************************************************************************
 *  drawing helpers
 ******************************************************************************/
GdkTexture *gimv_texture_new_for_pixbuf (GdkPixbuf   *pixbuf);
/* draw a texture with cairo (textures are converted/cached as cairo surfaces) */
void       gimv_cairo_draw_texture      (cairo_t     *cr,
                                         GdkTexture  *texture,
                                         gdouble      x,
                                         gdouble      y);
void       gimv_cairo_draw_pixbuf       (cairo_t     *cr,
                                         GdkPixbuf   *pixbuf,
                                         gdouble      x,
                                         gdouble      y);
void       gimv_cairo_set_source_color_name (cairo_t *cr,
                                         const gchar *spec);
/* foreground/background colors of a widget's CSS style */
void       gimv_widget_get_fg_color     (GtkWidget   *widget,
                                         GdkRGBA     *color);
void       gimv_widget_get_selected_bg_color (GtkWidget *widget,
                                         GdkRGBA     *color);
void       gimv_widget_get_base_color   (GtkWidget   *widget,
                                         GdkRGBA     *color);
GdkPixbuf *gimv_pixbuf_from_texture     (GdkTexture  *texture);
GdkPixbuf *gimv_widget_render_icon      (GtkWidget   *widget,
                                         const gchar *icon_name,
                                         gint         size);

/******************************************************************************
 *  misc
 ******************************************************************************/
void       gimv_widget_set_tooltip      (GtkWidget   *widget,
                                         const gchar *text);
void       gimv_beep                    (void);
GdkDisplay *gimv_display                (void);
void       gimv_screen_get_size         (gint        *width,
                                         gint        *height);
void       gimv_clipboard_set_text      (const gchar *text);

/* GtkTreeView reacts to column resizing only in a 6 pixel strip left of
   each header separator; make the strip wider, on both sides */
void       gimv_tree_view_widen_column_resize (GtkTreeView *treeview);

G_END_DECLS

#endif /* __GIMV_GTK4_COMPAT_H__ */
