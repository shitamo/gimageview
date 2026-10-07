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
 *  GTK2 style drag and drop on top of GtkDragSource / GtkDropTargetAsync.
 *
 *  Source side: when a drag starts ("prepare") the "drag_begin" and
 *  "drag_data_get" handlers are run for every target type and the data is
 *  offered through a GdkContentProvider.  URI lists are additionally offered
 *  as GdkFileList so that other GTK4 applications accept them.
 *
 *  Destination side: the drop data is read asynchronously for the first
 *  matching target and passed to the "drag_data_received" handlers.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>

#include "gimv_gtk4_compat.h"


typedef struct {
   GimvDndSignal signal;
   GCallback     handler;
   gpointer      data;
} DndHandler;

typedef struct {
   GList              *handlers;
   GimvTargetEntry    *src_targets;
   gint                n_src_targets;
   GdkDragAction       src_actions;
   GtkDragSource      *source;
   GimvDragContext    *src_context;

   GimvTargetEntry    *dest_targets;
   gint                n_dest_targets;
   GdkDragAction       dest_actions;
   GtkDropTargetAsync *dest;
} DndData;

#define DND_DATA_KEY     "gimv-dnd-data"
#define DROP_CONTEXT_KEY "gimv-dnd-drop-context"
#define DRAG_SOURCE_KEY  "gimv-dnd-source-widget"
#define DRAG_DELETE_KEY "gimv-drag-delete"


static void
targets_free (GimvTargetEntry *targets, gint n)
{
   gint i;

   if (!targets) return;
   for (i = 0; i < n; i++)
      g_free ((gchar *) targets[i].target);
   g_free (targets);
}


static GimvTargetEntry *
targets_copy (const GimvTargetEntry *targets, gint n)
{
   GimvTargetEntry *copy;
   gint i;

   if (!targets || n <= 0) return NULL;

   copy = g_new0 (GimvTargetEntry, n);
   for (i = 0; i < n; i++) {
      copy[i].target = g_strdup (targets[i].target);
      copy[i].flags  = targets[i].flags;
      copy[i].info   = targets[i].info;
   }

   return copy;
}


static void
context_free (GimvDragContext *ctx)
{
   if (!ctx) return;
   if (ctx->icon) g_object_unref (ctx->icon);
   g_free (ctx);
}


static void
dnd_data_free (gpointer data)
{
   DndData *dd = data;

   g_list_free_full (dd->handlers, g_free);
   targets_free (dd->src_targets, dd->n_src_targets);
   targets_free (dd->dest_targets, dd->n_dest_targets);
   context_free (dd->src_context);
   g_free (dd);
}


static DndData *
get_dnd_data (GtkWidget *widget, gboolean create)
{
   DndData *dd = g_object_get_data (G_OBJECT (widget), DND_DATA_KEY);

   if (!dd && create) {
      dd = g_new0 (DndData, 1);
      g_object_set_data_full (G_OBJECT (widget), DND_DATA_KEY, dd, dnd_data_free);
   }

   return dd;
}


typedef void     (*SimpleFunc)   (GtkWidget *, GimvDragContext *, gpointer);
typedef void     (*DataGetFunc)  (GtkWidget *, GimvDragContext *, GimvSelectionData *,
                                  guint, guint, gpointer);
typedef gboolean (*MotionFunc)   (GtkWidget *, GimvDragContext *, gint, gint,
                                  guint, gpointer);
typedef void     (*LeaveFunc)    (GtkWidget *, GimvDragContext *, guint, gpointer);
typedef void     (*ReceivedFunc) (GtkWidget *, GimvDragContext *, gint, gint,
                                  GimvSelectionData *, guint, guint, gpointer);


static GList *
handlers_for (DndData *dd, GimvDndSignal signal)
{
   GList *list = NULL, *node;

   for (node = dd->handlers; node; node = g_list_next (node)) {
      DndHandler *h = node->data;
      if (h->signal == signal)
         list = g_list_append (list, h);
   }

   return list;
}


static void
emit_simple (GtkWidget *widget, DndData *dd, GimvDndSignal signal,
             GimvDragContext *ctx)
{
   GList *list = handlers_for (dd, signal), *node;

   for (node = list; node; node = g_list_next (node)) {
      DndHandler *h = node->data;
      ((SimpleFunc) h->handler) (widget, ctx, h->data);
   }
   g_list_free (list);
}


gulong
gimv_dnd_connect (GtkWidget *widget, GimvDndSignal signal,
                  GCallback handler, gpointer data)
{
   DndData *dd;
   DndHandler *h;
   static gulong seq = 0;

   g_return_val_if_fail (GTK_IS_WIDGET (widget), 0);

   dd = get_dnd_data (widget, TRUE);
   h = g_new0 (DndHandler, 1);
   h->signal  = signal;
   h->handler = handler;
   h->data    = data;
   dd->handlers = g_list_append (dd->handlers, h);

   return ++seq;
}


void
gimv_selection_data_set (GimvSelectionData *seldata, const gchar *type,
                         gint format, const guchar *data, gint length)
{
   g_return_if_fail (seldata);

   g_free (seldata->data);
   seldata->data = NULL;
   seldata->length = -1;
   seldata->format = format;

   if (!data || length < 0) return;

   seldata->data = g_malloc (length + 1);
   memcpy (seldata->data, data, length);
   seldata->data[length] = '\0';
   seldata->length = length;
}



/******************************************************************************
 *
 *   source side
 *
 ******************************************************************************/
static GdkContentProvider *
file_list_provider (const guchar *uris, gint length)
{
   gchar *str = g_strndup ((const gchar *) uris, length);
   gchar **v = g_uri_list_extract_uris (str);
   GSList *files = NULL;
   GdkFileList *file_list;
   GdkContentProvider *provider;
   gint i;

   g_free (str);

   for (i = 0; v && v[i]; i++) {
      GFile *file;
      if (!g_str_has_prefix (v[i], "file:") && v[i][0] == '/')
         file = g_file_new_for_path (v[i]);
      else
         file = g_file_new_for_uri (v[i]);
      files = g_slist_prepend (files, file);
   }
   g_strfreev (v);

   if (!files) return NULL;

   files = g_slist_reverse (files);
   file_list = gdk_file_list_new_from_list (files);
   provider = gdk_content_provider_new_typed (GDK_TYPE_FILE_LIST, file_list);
   g_boxed_free (GDK_TYPE_FILE_LIST, file_list);
   g_slist_free_full (files, g_object_unref);

   return provider;
}


static GdkContentProvider *
cb_drag_prepare (GtkDragSource *source, gdouble x, gdouble y, gpointer user_data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (source));
   DndData *dd = get_dnd_data (widget, FALSE);
   GimvDragContext *ctx;
   GPtrArray *providers;
   GList *list, *node;
   GdkContentProvider *result = NULL;
   gint i;

   if (!dd || !dd->src_targets) return NULL;

   context_free (dd->src_context);
   ctx = dd->src_context = g_new0 (GimvDragContext, 1);
   ctx->actions = dd->src_actions;
   ctx->suggested_action = ctx->action = GDK_ACTION_COPY;
   ctx->source_widget = widget;
   ctx->widget = widget;

   /* GTK2 emitted "drag_begin" before any data was requested */
   emit_simple (widget, dd, GIMV_DND_DRAG_BEGIN, ctx);

   providers = g_ptr_array_new ();
   list = handlers_for (dd, GIMV_DND_DRAG_DATA_GET);

   for (i = 0; i < dd->n_src_targets; i++) {
      GimvSelectionData sel;

      memset (&sel, 0, sizeof (sel));
      sel.target = dd->src_targets[i].target;
      sel.length = -1;

      for (node = list; node; node = g_list_next (node)) {
         DndHandler *h = node->data;
         ((DataGetFunc) h->handler) (widget, ctx, &sel, dd->src_targets[i].info,
                                     GDK_CURRENT_TIME, h->data);
      }

      if (sel.data && sel.length >= 0) {
         GBytes *bytes = g_bytes_new (sel.data, sel.length);
         g_ptr_array_add (providers,
                          gdk_content_provider_new_for_bytes (sel.target, bytes));
         g_bytes_unref (bytes);

         if (!strcmp (sel.target, "text/uri-list")) {
            GdkContentProvider *p = file_list_provider (sel.data, sel.length);
            if (p) g_ptr_array_add (providers, p);
         }
      }
      g_free (sel.data);
   }
   g_list_free (list);

   if (providers->len > 0)
      result = gdk_content_provider_new_union ((GdkContentProvider **) providers->pdata,
                                               providers->len);
   g_ptr_array_free (providers, TRUE);

   if (ctx->icon)
      gtk_drag_source_set_icon (source, ctx->icon, ctx->icon_hot_x, ctx->icon_hot_y);

   return result;
}


static void
cb_drag_begin (GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (source));
   DndData *dd = get_dnd_data (widget, FALSE);

   g_object_set_data (G_OBJECT (drag), DRAG_SOURCE_KEY, widget);
   if (dd && dd->src_context) {
      dd->src_context->drag = drag;
      if (dd->src_context->icon)
         gtk_drag_source_set_icon (source, dd->src_context->icon,
                                   dd->src_context->icon_hot_x,
                                   dd->src_context->icon_hot_y);
   }
}


static void
cb_drag_end (GtkDragSource *source, GdkDrag *drag, gboolean delete_data,
             gpointer user_data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (source));
   DndData *dd = get_dnd_data (widget, FALSE);
   GimvDragContext *ctx;

   if (!dd) return;

   ctx = dd->src_context;
   if (!ctx) {
      ctx = dd->src_context = g_new0 (GimvDragContext, 1);
      ctx->source_widget = widget;
      ctx->widget = widget;
   }
   ctx->drag = drag;
   ctx->action = gdk_drag_get_selected_action (drag);

   if (delete_data || g_object_get_data (G_OBJECT (drag), DRAG_DELETE_KEY))
      emit_simple (widget, dd, GIMV_DND_DRAG_DATA_DELETE, ctx);
   emit_simple (widget, dd, GIMV_DND_DRAG_END, ctx);

   context_free (dd->src_context);
   dd->src_context = NULL;
}


void
gimv_drag_source_set (GtkWidget *widget, GdkModifierType start_button_mask,
                      const GimvTargetEntry *targets, gint n_targets,
                      GdkDragAction actions)
{
   DndData *dd;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   dd = get_dnd_data (widget, TRUE);

   targets_free (dd->src_targets, dd->n_src_targets);
   dd->src_targets   = targets_copy (targets, n_targets);
   dd->n_src_targets = n_targets;
   /* GDK_ACTION_ASK is chosen by the destination in GTK4 */
   dd->src_actions   = actions & (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
   if (!dd->src_actions) dd->src_actions = GDK_ACTION_COPY;

   if (!dd->source) {
      dd->source = gtk_drag_source_new ();
      /* GTK2 code allowed dragging with any of buttons 1-3 */
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (dd->source),
                                     (start_button_mask & (GDK_BUTTON2_MASK | GDK_BUTTON3_MASK))
                                     ? 0 : GDK_BUTTON_PRIMARY);
      g_signal_connect (dd->source, "prepare",  G_CALLBACK (cb_drag_prepare), NULL);
      g_signal_connect (dd->source, "drag-begin", G_CALLBACK (cb_drag_begin), NULL);
      g_signal_connect (dd->source, "drag-end", G_CALLBACK (cb_drag_end), NULL);
      gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (dd->source));
   }
   gtk_drag_source_set_actions (dd->source, dd->src_actions);
}


void
gimv_drag_source_unset (GtkWidget *widget)
{
   DndData *dd;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   dd = get_dnd_data (widget, FALSE);
   if (!dd || !dd->source) return;

   gtk_widget_remove_controller (widget, GTK_EVENT_CONTROLLER (dd->source));
   dd->source = NULL;
   targets_free (dd->src_targets, dd->n_src_targets);
   dd->src_targets = NULL;
   dd->n_src_targets = 0;
}


void
gimv_drag_set_icon_texture (GimvDragContext *context, GdkTexture *texture,
                            gint hot_x, gint hot_y)
{
   g_return_if_fail (context);

   if (context->icon) g_object_unref (context->icon);
   context->icon = texture ? g_object_ref (GDK_PAINTABLE (texture)) : NULL;
   context->icon_hot_x = hot_x;
   context->icon_hot_y = hot_y;
}



/******************************************************************************
 *
 *   destination side
 *
 ******************************************************************************/
static GimvDragContext *
drop_get_context (GtkWidget *widget, GdkDrop *drop)
{
   GimvDragContext *ctx = g_object_get_data (G_OBJECT (drop), DROP_CONTEXT_KEY);
   GdkDrag *drag;

   if (ctx) return ctx;

   ctx = g_new0 (GimvDragContext, 1);
   ctx->drop = drop;
   ctx->widget = widget;
   ctx->actions = gdk_drop_get_actions (drop);
   drag = gdk_drop_get_drag (drop);
   if (drag)
      ctx->source_widget = g_object_get_data (G_OBJECT (drag), DRAG_SOURCE_KEY);

   g_object_set_data_full (G_OBJECT (drop), DROP_CONTEXT_KEY, ctx,
                           (GDestroyNotify) context_free);

   return ctx;
}


static GdkDragAction
choose_action (GtkWidget *widget, GdkDragAction actions)
{
   GdkModifierType state = gimv_get_current_modifier_state (widget);

   if ((state & GDK_SHIFT_MASK) && (state & GDK_CONTROL_MASK)
       && (actions & GDK_ACTION_LINK))
      return GDK_ACTION_LINK;
   if ((state & GDK_SHIFT_MASK) && (actions & GDK_ACTION_MOVE))
      return GDK_ACTION_MOVE;
   if ((state & GDK_CONTROL_MASK) && (actions & GDK_ACTION_COPY))
      return GDK_ACTION_COPY;

   if (actions & GDK_ACTION_COPY) return GDK_ACTION_COPY;
   if (actions & GDK_ACTION_MOVE) return GDK_ACTION_MOVE;
   if (actions & GDK_ACTION_LINK) return GDK_ACTION_LINK;
   if (actions & GDK_ACTION_ASK)  return GDK_ACTION_ASK;

   return 0;
}


static GdkDragAction
dest_motion (GtkWidget *widget, DndData *dd, GdkDrop *drop, gdouble x, gdouble y)
{
   GimvDragContext *ctx = drop_get_context (widget, drop);
   GList *list, *node;
   gboolean handled = FALSE;

   ctx->actions = gdk_drop_get_actions (drop) & (dd->dest_actions | GDK_ACTION_ASK);
   ctx->suggested_action = choose_action (widget, ctx->actions);
   ctx->status_set = FALSE;

   list = handlers_for (dd, GIMV_DND_DRAG_MOTION);
   for (node = list; node && !handled; node = g_list_next (node)) {
      DndHandler *h = node->data;
      handled = ((MotionFunc) h->handler) (widget, ctx, (gint) x, (gint) y,
                                           GDK_CURRENT_TIME, h->data);
   }
   g_list_free (list);

   if (handled && ctx->status_set)
      return ctx->action;

   ctx->action = ctx->suggested_action;
   return ctx->action;
}


static gboolean
cb_drop_accept (GtkDropTargetAsync *target, GdkDrop *drop, gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
   DndData *dd = get_dnd_data (widget, FALSE);
   GdkContentFormats *formats;
   gint i;

   if (!dd) return FALSE;

   formats = gdk_drop_get_formats (drop);
   for (i = 0; i < dd->n_dest_targets; i++) {
      const gchar *mime = dd->dest_targets[i].target;
      if (gdk_content_formats_contain_mime_type (formats, mime)) {
         if ((dd->dest_targets[i].flags & GIMV_TARGET_SAME_APP)
             && !gdk_drop_get_drag (drop))
            continue;
         return TRUE;
      }
   }

   return FALSE;
}


static GdkDragAction
cb_drop_enter (GtkDropTargetAsync *target, GdkDrop *drop, gdouble x, gdouble y,
               gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
   DndData *dd = get_dnd_data (widget, FALSE);
   return dd ? dest_motion (widget, dd, drop, x, y) : 0;
}


static GdkDragAction
cb_drop_motion (GtkDropTargetAsync *target, GdkDrop *drop, gdouble x, gdouble y,
                gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
   DndData *dd = get_dnd_data (widget, FALSE);
   return dd ? dest_motion (widget, dd, drop, x, y) : 0;
}


static void
cb_drop_leave (GtkDropTargetAsync *target, GdkDrop *drop, gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
   DndData *dd = get_dnd_data (widget, FALSE);
   GimvDragContext *ctx;
   GList *list, *node;

   if (!dd) return;

   ctx = drop_get_context (widget, drop);
   list = handlers_for (dd, GIMV_DND_DRAG_LEAVE);
   for (node = list; node; node = g_list_next (node)) {
      DndHandler *h = node->data;
      ((LeaveFunc) h->handler) (widget, ctx, GDK_CURRENT_TIME, h->data);
   }
   g_list_free (list);
}


typedef struct {
   GtkWidget       *widget;
   GdkDrop         *drop;
   gdouble          x, y;
   guint            info;
   gchar           *mime;
   GOutputStream   *out;
} ReadInfo;


static void
read_info_free (ReadInfo *ri)
{
   g_object_unref (ri->widget);
   g_object_unref (ri->drop);
   g_free (ri->mime);
   if (ri->out) g_object_unref (ri->out);
   g_free (ri);
}


static void
unset_drop_active (GtkWidget *widget)
{
   GtkWidget *child;

   gtk_widget_unset_state_flags (widget, GTK_STATE_FLAG_DROP_ACTIVE);
   for (child = gtk_widget_get_first_child (widget);
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      unset_drop_active (child);
   }
}


static void
deliver_drop (ReadInfo *ri, const guchar *data, gint length)
{
   DndData *dd = get_dnd_data (ri->widget, FALSE);
   GimvDragContext *ctx = drop_get_context (ri->widget, ri->drop);
   GimvSelectionData sel;
   GList *list, *node;

   ctx->finished = FALSE;
   if (!ctx->action)
      ctx->action = choose_action (ri->widget, gdk_drop_get_actions (ri->drop));

   memset (&sel, 0, sizeof (sel));
   sel.target = ri->mime;
   sel.format = 8;
   sel.data   = (guchar *) data;
   sel.length = data ? length : -1;

   if (dd) {
      list = handlers_for (dd, GIMV_DND_DRAG_DATA_RECEIVED);
      for (node = list; node; node = g_list_next (node)) {
         DndHandler *h = node->data;
         ((ReceivedFunc) h->handler) (ri->widget, ctx, (gint) ri->x, (gint) ri->y,
                                      &sel, ri->info, GDK_CURRENT_TIME, h->data);
      }
      g_list_free (list);
   }

   /* GTK_DEST_DEFAULT_DROP finishes the drag automatically */
   if (!ctx->finished)
      gimv_drag_finish (ctx, data != NULL, FALSE, GDK_CURRENT_TIME);

   /* The drop target normally clears the highlight on the crossing out of
      the widget, which never comes when a handler moved the widget (e.g. a
      notebook tab that was reordered), nor when a child's own drop target
      was disabled (GtkText in an entry keeps DROP_ACTIVE). */
   unset_drop_active (ri->widget);
}


static void
cb_splice_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
   ReadInfo *ri = user_data;
   GError *error = NULL;
   gssize n;

   n = g_output_stream_splice_finish (G_OUTPUT_STREAM (source), result, &error);
   if (n < 0) {
      g_warning ("drop: %s", error ? error->message : "read error");
      g_clear_error (&error);
      deliver_drop (ri, NULL, -1);
   } else {
      GMemoryOutputStream *mem = G_MEMORY_OUTPUT_STREAM (ri->out);
      deliver_drop (ri, g_memory_output_stream_get_data (mem),
                    g_memory_output_stream_get_data_size (mem));
   }

   read_info_free (ri);
}


static void
cb_drop_read_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
   ReadInfo *ri = user_data;
   GInputStream *stream;
   GError *error = NULL;
   const char *mime = NULL;

   stream = gdk_drop_read_finish (GDK_DROP (source), result, &mime, &error);
   if (!stream) {
      g_warning ("drop: %s", error ? error->message : "read error");
      g_clear_error (&error);
      deliver_drop (ri, NULL, -1);
      read_info_free (ri);
      return;
   }

   ri->out = g_memory_output_stream_new_resizable ();
   g_output_stream_splice_async (ri->out, stream,
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE
                                 | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                 G_PRIORITY_DEFAULT, NULL,
                                 cb_splice_done, ri);
   g_object_unref (stream);
}


static gboolean
cb_drop_drop (GtkDropTargetAsync *target, GdkDrop *drop, gdouble x, gdouble y,
              gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
   DndData *dd = get_dnd_data (widget, FALSE);
   GdkContentFormats *formats;
   GimvDragContext *ctx;
   ReadInfo *ri;
   const char *mimes[2] = { NULL, NULL };
   gint i;

   if (!dd) return FALSE;

   formats = gdk_drop_get_formats (drop);
   for (i = 0; i < dd->n_dest_targets; i++) {
      if (gdk_content_formats_contain_mime_type (formats, dd->dest_targets[i].target))
         break;
   }
   if (i >= dd->n_dest_targets) return FALSE;

   ctx = drop_get_context (widget, drop);
   if (!ctx->action)
      dest_motion (widget, dd, drop, x, y);

   ri = g_new0 (ReadInfo, 1);
   ri->widget = g_object_ref (widget);
   ri->drop   = g_object_ref (drop);
   ri->x      = x;
   ri->y      = y;
   ri->info   = dd->dest_targets[i].info;
   ri->mime   = g_strdup (dd->dest_targets[i].target);
   mimes[0]   = ri->mime;

   gdk_drop_read_async (drop, mimes, G_PRIORITY_DEFAULT, NULL,
                        cb_drop_read_done, ri);

   return TRUE;
}


static void
disable_child_drop_targets (GtkWidget *widget)
{
   GtkWidget *child;

   for (child = gtk_widget_get_first_child (widget);
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      GListModel *controllers = gtk_widget_observe_controllers (child);
      guint i, n = g_list_model_get_n_items (controllers);

      for (i = 0; i < n; i++) {
         GtkEventController *c = g_list_model_get_item (controllers, i);
         if (GTK_IS_DROP_TARGET (c) || GTK_IS_DROP_TARGET_ASYNC (c))
            gtk_event_controller_set_propagation_phase (c, GTK_PHASE_NONE);
         g_object_unref (c);
      }
      g_object_unref (controllers);

      disable_child_drop_targets (child);
   }
}


GdkContentFormats *
gimv_target_entries_to_formats (const GimvTargetEntry *targets, gint n_targets)
{
   GdkContentFormatsBuilder *builder = gdk_content_formats_builder_new ();
   gint i;

   for (i = 0; i < n_targets; i++)
      gdk_content_formats_builder_add_mime_type (builder, targets[i].target);

   return gdk_content_formats_builder_free_to_formats (builder);
}


void
gimv_drag_dest_set (GtkWidget *widget, guint flags,
                    const GimvTargetEntry *targets, gint n_targets,
                    GdkDragAction actions)
{
   DndData *dd;
   GdkContentFormats *formats;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   dd = get_dnd_data (widget, TRUE);

   targets_free (dd->dest_targets, dd->n_dest_targets);
   dd->dest_targets   = targets_copy (targets, n_targets);
   dd->n_dest_targets = n_targets;
   dd->dest_actions   = actions;

   formats = gimv_target_entries_to_formats (targets, n_targets);

   /* GTK2's gtk_drag_dest_set () replaced a GtkEntry's own text drop
      handling; in GTK4 the entry's GtkText child has a drop target of its
      own, deeper in the tree, which would insert the dropped file names as
      text in addition to our handler.  Turn it off. */
   if (GTK_IS_EDITABLE (widget))
      disable_child_drop_targets (widget);

   if (!dd->dest) {
      dd->dest = gtk_drop_target_async_new (formats, actions);
      g_signal_connect (dd->dest, "accept",      G_CALLBACK (cb_drop_accept), NULL);
      g_signal_connect (dd->dest, "drag-enter",  G_CALLBACK (cb_drop_enter),  NULL);
      g_signal_connect (dd->dest, "drag-motion", G_CALLBACK (cb_drop_motion), NULL);
      g_signal_connect (dd->dest, "drag-leave",  G_CALLBACK (cb_drop_leave),  NULL);
      g_signal_connect (dd->dest, "drop",        G_CALLBACK (cb_drop_drop),   NULL);
      gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (dd->dest));
   } else {
      gtk_drop_target_async_set_formats (dd->dest, formats);
      gtk_drop_target_async_set_actions (dd->dest, actions);
      gdk_content_formats_unref (formats);
   }
}


void
gimv_drag_dest_unset (GtkWidget *widget)
{
   DndData *dd;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   dd = get_dnd_data (widget, FALSE);
   if (!dd || !dd->dest) return;

   gtk_widget_remove_controller (widget, GTK_EVENT_CONTROLLER (dd->dest));
   dd->dest = NULL;
}


GtkWidget *
gimv_drag_get_source_widget (GimvDragContext *context)
{
   g_return_val_if_fail (context, NULL);
   return context->source_widget;
}


void
gimv_drag_status (GimvDragContext *context, GdkDragAction action, guint32 time)
{
   g_return_if_fail (context);

   context->action = action;
   context->status_set = TRUE;
}


void
gimv_drag_finish (GimvDragContext *context, gboolean success, gboolean del,
                  guint32 time)
{
   GdkDragAction action;

   g_return_if_fail (context);

   if (context->finished || !context->drop) return;
   context->finished = TRUE;

   action = success ? context->action : 0;
   if (success && del)
      action = GDK_ACTION_MOVE;
   if (action == GDK_ACTION_ASK)
      action = GDK_ACTION_COPY;

   /* GTK2: gtk_drag_finish (del = TRUE) made the source emit "drag_data_delete"
      (e.g. to refresh its file list after a move chosen from the drop menu).
      GTK4's source decides from the action last reported by the drop target
      during motion (usually COPY or ASK), so report the final action first,
      and mark local drags explicitly. */
   if (action && (gdk_drop_get_actions (context->drop) & action))
      gdk_drop_status (context->drop, gdk_drop_get_actions (context->drop), action);
   if (action == GDK_ACTION_MOVE && gdk_drop_get_drag (context->drop))
      g_object_set_data (G_OBJECT (gdk_drop_get_drag (context->drop)),
                         DRAG_DELETE_KEY, GINT_TO_POINTER (TRUE));

   gdk_drop_finish (context->drop, action);
}
