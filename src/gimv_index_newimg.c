/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: desktop notifications of new images.
 *
 * Images that come into the indexed folders while gimv runs are gathered
 * for a while (index_notify_wait, 30 seconds at first) and shown as one
 * desktop notification (org.freedesktop.Notifications: xfce4-notifyd,
 * GNOME Shell, ...): one image with its picture, or "42 new images in
 * folder" with one of them.  Notifications come at most every
 * index_notify_interval seconds.  A click on one opens the image, the
 * folder, or a collection of the new images in gimv.
 *
 * Where the notifications appear is set in the notification daemon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>
#include <gio/gio.h>

#include "fileload.h"
#include "gimv_index_newimg.h"
#include "gimv_thumb_win.h"
#include "prefs.h"

#define NOTIFY_NAME  "org.freedesktop.Notifications"
#define NOTIFY_PATH  "/org/freedesktop/Notifications"
#define NOTIFY_IFACE "org.freedesktop.Notifications"

static GPtrArray       *gathered = NULL;   /* paths of new images */
static guint            timer_id = 0;
static gint64           last_sent = 0;
static GDBusConnection *bus = NULL;
static guint            action_sub = 0, closed_sub = 0;
static GHashTable      *shown = NULL;      /* notification id -> GPtrArray of paths */


static gboolean
excluded (const gchar *path)
{
   gchar **dirs;
   gboolean ret = FALSE;
   gint i;

   if (!conf.index_notify_exclude || !*conf.index_notify_exclude) return FALSE;
   dirs = g_strsplit (conf.index_notify_exclude, ";", -1);
   for (i = 0; dirs[i] && !ret; i++) {
      gchar *d = g_strstrip (dirs[i]);
      gsize len;
      while ((len = strlen (d)) > 1 && d[len - 1] == '/') d[len - 1] = '\0';
      ret = *d && g_str_has_prefix (path, d) && path[strlen (d)] == '/';
   }
   g_strfreev (dirs);
   return ret;
}


/******************************************************************************
 *  opening what a notification shows
 ******************************************************************************/

static gboolean
same_folder (GPtrArray *paths, gchar **folder)
{
   gchar *first = g_path_get_dirname (g_ptr_array_index (paths, 0));
   guint i;

   for (i = 1; i < paths->len; i++) {
      gchar *d = g_path_get_dirname (g_ptr_array_index (paths, i));
      gboolean same = !strcmp (d, first);
      g_free (d);
      if (!same) {
         g_free (first);
         return FALSE;
      }
   }
   *folder = first;
   return TRUE;
}


static void
open_paths (GPtrArray *paths)
{
   GList *wins = gimv_thumb_win_get_list ();
   GimvThumbWin *tw = wins ? g_list_last (wins)->data : NULL;
   FilesLoader *files;
   gchar *folder = NULL;
   guint i;

   if (!paths || !paths->len || files_loader_query_loading ()) return;
   if (!tw) tw = gimv_thumb_win_open_window ();
   gtk_window_present (GTK_WINDOW (tw));

   if (paths->len == 1) {
      /* the image itself */
      files = files_loader_new ();
      files->filelist = g_list_append (NULL, g_strdup (g_ptr_array_index (paths, 0)));
      files->status = END;
      open_image_files (files);
      files_loader_delete (files);
   } else if (same_folder (paths, &folder)) {
      open_dir_images (folder, tw, NULL, LOAD_CACHE, SCAN_SUB_DIR_NONE);
      g_free (folder);
   } else {
      /* a collection of the new images */
      files = files_loader_new ();
      for (i = 0; i < paths->len; i++)
         files->filelist = g_list_prepend (files->filelist,
                                           g_strdup (g_ptr_array_index (paths, i)));
      files->filelist = g_list_reverse (files->filelist);
      files->status = END;
      open_image_files_in_thumbnail_view (files, tw, NULL);
      files_loader_delete (files);
   }
}


static void
cb_signal (GDBusConnection *conn, const gchar *sender, const gchar *path,
           const gchar *iface, const gchar *signal, GVariant *params, gpointer data)
{
   guint32 id = 0;

   if (!strcmp (signal, "ActionInvoked")) {
      const gchar *action = NULL;
      g_variant_get (params, "(u&s)", &id, &action);
      if (shown && g_hash_table_contains (shown, GUINT_TO_POINTER (id)))
         open_paths (g_hash_table_lookup (shown, GUINT_TO_POINTER (id)));
   } else if (!strcmp (signal, "NotificationClosed")) {
      guint32 reason;
      g_variant_get (params, "(uu)", &id, &reason);
      /* the click (ActionInvoked) comes before it */
      if (shown) g_hash_table_remove (shown, GUINT_TO_POINTER (id));
   }
}


static gboolean
ensure_bus (void)
{
   if (bus) return TRUE;
   bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
   if (!bus) return FALSE;
   shown = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                  (GDestroyNotify) g_ptr_array_unref);
   action_sub = g_dbus_connection_signal_subscribe (bus, NULL, NOTIFY_IFACE, "ActionInvoked",
                                                    NOTIFY_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                    cb_signal, NULL, NULL);
   closed_sub = g_dbus_connection_signal_subscribe (bus, NULL, NOTIFY_IFACE, "NotificationClosed",
                                                    NOTIFY_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                    cb_signal, NULL, NULL);
   return TRUE;
}


/******************************************************************************
 *  sending
 ******************************************************************************/

static void
cb_notified (GObject *source, GAsyncResult *res, gpointer data)
{
   GPtrArray *paths = data;
   GError *error = NULL;
   GVariant *ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

   if (!ret) {
      /* no notification daemon */
      g_message ("GImageView: cannot show a notification: %s", error->message);
      g_error_free (error);
      g_ptr_array_unref (paths);
      return;
   }
   {
      guint32 id;
      g_variant_get (ret, "(u)", &id);
      g_hash_table_insert (shown, GUINT_TO_POINTER (id), paths);
   }
   g_variant_unref (ret);
}


static void
send_notification (GPtrArray *paths)
{
   const gchar *picture = g_ptr_array_index (paths, paths->len - 1);  /* the newest */
   gchar *summary, *body, *folder = NULL, *uri;
   GVariantBuilder actions, hints;

   if (!ensure_bus ()) {
      g_ptr_array_unref (paths);
      return;
   }

   if (paths->len == 1) {
      gchar *name = g_path_get_basename (picture);
      gchar *dir  = g_path_get_dirname (picture);
      summary = g_strdup (_("New image"));
      body = g_strdup_printf ("%s\n%s", name, dir);
      g_free (name);
      g_free (dir);
   } else if (same_folder (paths, &folder)) {
      gchar *name = g_path_get_basename (folder);
      summary = g_strdup_printf (_("%u new images"), paths->len);
      body = g_strdup_printf (_("%u new images in %s"), paths->len, name);
      g_free (name);
      g_free (folder);
   } else {
      gchar *dir  = g_path_get_dirname (picture);
      gchar *name = g_path_get_basename (dir);
      summary = g_strdup_printf (_("%u new images"), paths->len);
      body = g_strdup_printf (_("%u new images in %s and other folders"), paths->len, name);
      g_free (name);
      g_free (dir);
   }

   g_variant_builder_init (&actions, G_VARIANT_TYPE ("as"));
   g_variant_builder_add (&actions, "s", "default");
   g_variant_builder_add (&actions, "s", _("Open"));

   uri = g_filename_to_uri (picture, NULL, NULL);
   g_variant_builder_init (&hints, G_VARIANT_TYPE ("a{sv}"));
   if (uri)
      g_variant_builder_add (&hints, "{sv}", "image-path", g_variant_new_string (uri));
   g_variant_builder_add (&hints, "{sv}", "desktop-entry", g_variant_new_string ("gimageview"));
   g_variant_builder_add (&hints, "{sv}", "category", g_variant_new_string ("transfer.complete"));
   g_free (uri);

   g_dbus_connection_call (bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_IFACE, "Notify",
                           g_variant_new ("(susssasa{sv}i)", "GImageView", 0, "gimageview",
                                          summary, body, &actions, &hints,
                                          conf.index_notify_timeout > 0
                                          ? conf.index_notify_timeout * 1000 : -1),
                           G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
                           NULL, cb_notified, paths);
   g_free (summary);
   g_free (body);
   last_sent = g_get_monotonic_time ();
}


static gboolean
cb_timer (gpointer data)
{
   timer_id = 0;
   if (gathered && gathered->len && conf.index_notify) {
      send_notification (gathered);     /* owned by the notification now */
      gathered = NULL;
   }
   return G_SOURCE_REMOVE;
}


void
gimv_index_newimg_add (const gchar *path)
{
   gint64 now, at, wait_us, interval_us;

   g_return_if_fail (path);
   if (!conf.index_notify || excluded (path)) return;

   if (!gathered)
      gathered = g_ptr_array_new_with_free_func (g_free);
   if (!g_ptr_array_find_with_equal_func (gathered, path, g_str_equal, NULL))
      g_ptr_array_add (gathered, g_strdup (path));

   if (timer_id) return;

   /* gather for a while, and keep the interval since the last one */
   now = g_get_monotonic_time ();
   wait_us = (gint64) MAX (conf.index_notify_wait, 0) * G_USEC_PER_SEC;
   interval_us = (gint64) MAX (conf.index_notify_interval, 0) * G_USEC_PER_SEC;
   at = now + wait_us;
   if (last_sent && last_sent + interval_us > at)
      at = last_sent + interval_us;
   timer_id = g_timeout_add ((guint) ((at - now) / 1000), cb_timer, NULL);
}


void
gimv_index_newimg_quit (void)
{
   if (timer_id) g_source_remove (timer_id);
   timer_id = 0;
   g_clear_pointer (&gathered, g_ptr_array_unref);
   if (bus) {
      g_dbus_connection_signal_unsubscribe (bus, action_sub);
      g_dbus_connection_signal_unsubscribe (bus, closed_sub);
      g_clear_object (&bus);
   }
   g_clear_pointer (&shown, g_hash_table_unref);
}
