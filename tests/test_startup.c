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
 * gimv started on an X display (meson runs this under xvfb-run) with a
 * directory of images: its thumbnails must be made (they are kept in the
 * freedesktop.org cache, where this looks for them), and gimv must quit
 * when its window is closed (WM_DELETE_WINDOW, as a window manager sends
 * it), saving its preferences.  Skipped without a display.
 */

#include <signal.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "gimv_test.h"

#define N_IMAGES 3

static gchar *tmp;


static void
write_rc (const gchar *home, const gchar *build)
{
   static const gchar *dirs[] = {
      "archiver", "image_loader", "image_saver", "io_stream", "thumbnail",
      "image_view", "thumbnail_view",
   };
   GString *list = g_string_new (NULL);
   gchar *rc, *text;
   guint i;

   for (i = 0; i < G_N_ELEMENTS (dirs); i++) {
      if (list->len) g_string_append_c (list, ',');
      g_string_append_printf (list, "%s/plugins/%s", build, dirs[i]);
   }
   text = g_strdup_printf ("[Common]\n"
                           "startup_show_splash=FALSE\n"
                           "index_enable=FALSE\n"
                           "cache_write_type=freedesktop.org\n"
                           "cache_read_list=freedesktop.org\n"
                           "thumbwin_thumb_size=96\n"
                           "[Plugin]\n"
                           "plugin_use_default_search_dir_list=FALSE\n"
                           "plugin_search_dir_list=%s\n", list->str);
   rc = g_build_filename (home, ".gimv", "gimvrc", NULL);
   gimv_test_write_string (rc, text);
   g_free (rc);
   g_free (text);
   rc = g_build_filename (home, ".gimv", "keyconf", NULL);
   gimv_test_write_string (rc, "\n");
   g_free (rc);
   g_string_free (list, TRUE);
}


static gchar *
thumb_of (const gchar *cache, const gchar *image)
{
   gchar *uri = g_filename_to_uri (image, NULL, NULL);
   gchar *md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
   gchar *name = g_strconcat (md5, ".png", NULL);
   gchar *path = g_build_filename (cache, "thumbnails", "normal", name, NULL);
   g_free (uri);
   g_free (md5);
   g_free (name);
   return path;
}


static gboolean
has_atom (Display *d, Window w, Atom prop, Atom value)
{
   Atom type;
   gint format;
   gulong n, after, i;
   guchar *data = NULL;
   gboolean ret = FALSE;

   if (XGetWindowProperty (d, w, prop, 0, 64, False, XA_ATOM, &type, &format,
                           &n, &after, &data) != Success || !data)
      return FALSE;
   for (i = 0; i < n && format == 32; i++)
      if (((Atom *) data)[i] == value) ret = TRUE;
   XFree (data);
   return ret;
}


static glong
window_pid (Display *d, Window w)
{
   Atom type;
   gint format;
   gulong n, after;
   guchar *data = NULL;
   glong pid = -1;

   if (XGetWindowProperty (d, w, XInternAtom (d, "_NET_WM_PID", False), 0, 1, False,
                           XA_CARDINAL, &type, &format, &n, &after, &data) == Success
       && data && n == 1)
      pid = *(glong *) data;
   if (data) XFree (data);
   return pid;
}


/* WM_DELETE_WINDOW to the top level windows of pid; how many */
static guint
close_windows (Display *d, glong pid)
{
   Atom protocols = XInternAtom (d, "WM_PROTOCOLS", False);
   Atom del = XInternAtom (d, "WM_DELETE_WINDOW", False);
   Window root = DefaultRootWindow (d), parent, *children = NULL;
   guint n = 0, i, sent = 0;

   if (!XQueryTree (d, root, &root, &parent, &children, &n)) return 0;
   for (i = 0; i < n; i++) {
      XWindowAttributes wa;
      XEvent ev;

      if (window_pid (d, children[i]) != pid) continue;
      if (!XGetWindowAttributes (d, children[i], &wa) || wa.map_state != IsViewable) continue;
      if (!has_atom (d, children[i], protocols, del)) continue;
      memset (&ev, 0, sizeof (ev));
      ev.xclient.type = ClientMessage;
      ev.xclient.window = children[i];
      ev.xclient.message_type = protocols;
      ev.xclient.format = 32;
      ev.xclient.data.l[0] = del;
      ev.xclient.data.l[1] = CurrentTime;
      XSendEvent (d, children[i], False, NoEventMask, &ev);
      sent++;
   }
   XFlush (d);
   if (children) XFree (children);
   return sent;
}


static void
cb_waited (GObject *proc, GAsyncResult *res, gpointer data)
{
   g_subprocess_wait_finish (G_SUBPROCESS (proc), res, NULL);
   *(gboolean *) data = TRUE;
}


static gboolean proc_ended = FALSE;

/* the events of the wait handled; TRUE once the process ended */
static gboolean
exited (GSubprocess *proc)
{
   while (g_main_context_iteration (NULL, FALSE));
   return proc_ended;
}


/* the messages of gimv, kept apart: xvfb-run puts them into the TAP
 * output, where they are not TAP.  Shown as comments when the test fails. */
static gchar *gimv_log = NULL;

static void
show_gimv_log (void)
{
   gchar *text = NULL, **lines;
   guint i;

   if (!gimv_log || !g_file_get_contents (gimv_log, &text, NULL, NULL)) return;
   lines = g_strsplit (text, "\n", -1);
   for (i = 0; lines[i]; i++)
      if (*lines[i]) g_print ("# gimv: %s\n", lines[i]);
   g_strfreev (lines);
   g_free (text);
}


static void
test_startup (void)
{
   const gchar *build = g_getenv ("GIMV_TEST_BUILD_ROOT"), *gimv = g_getenv ("GIMV_TEST_GIMV");
   gchar *home, *images, *cache, *thumbs[N_IMAGES], *rc;
   GSubprocessLauncher *l;
   GSubprocess *proc;
   GError *error = NULL;
   Display *d;
   GStatBuf rc_before, rc_after;
   gint64 end;
   guint i, sent = 0, found = 0;
   glong pid;

   if (!g_getenv ("DISPLAY") || !(d = XOpenDisplay (NULL))) {
      g_test_skip ("no X display (xvfb-run is needed)");
      return;
   }
   g_assert_nonnull (build);
   g_assert_nonnull (gimv);

   home = g_build_filename (tmp, "home", NULL);
   images = g_build_filename (tmp, "images", NULL);
   cache = g_build_filename (home, ".cache", NULL);
   write_rc (home, build);
   for (i = 0; i < N_IMAGES; i++) {
      gchar *name = g_strdup_printf ("image%u.png", i);
      gchar *path = g_build_filename (images, name, NULL);
      GdkPixbuf *p = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 200 + i * 50, 150);
      g_mkdir_with_parents (images, 0700);
      gdk_pixbuf_fill (p, 0x112233ff + (i << 8));
      gdk_pixbuf_save (p, path, "png", &error, NULL);
      g_assert_no_error (error);
      g_object_unref (p);
      thumbs[i] = thumb_of (cache, path);
      g_free (name);
      g_free (path);
   }
   rc = g_build_filename (home, ".gimv", "gimvrc", NULL);
   gimv_test_set_mtime (rc, 1000000000);
   g_assert_cmpint (g_stat (rc, &rc_before), ==, 0);

   /* its stdout is not TAP */
   l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
   gimv_log = g_build_filename (tmp, "gimv.log", NULL);
   g_subprocess_launcher_set_stderr_file_path (l, gimv_log);
   g_subprocess_launcher_setenv (l, "HOME", home, TRUE);
   g_subprocess_launcher_setenv (l, "XDG_CACHE_HOME", cache, TRUE);
   g_subprocess_launcher_setenv (l, "XDG_CONFIG_HOME", home, TRUE);
   g_subprocess_launcher_setenv (l, "XDG_DATA_HOME", home, TRUE);
   g_subprocess_launcher_setenv (l, "GDK_BACKEND", "x11", TRUE);
   g_subprocess_launcher_setenv (l, "GSK_RENDERER", "cairo", TRUE);
   g_subprocess_launcher_setenv (l, "NO_AT_BRIDGE", "1", TRUE);
   /* -d: the directory (the files in it) */
   proc = g_subprocess_launcher_spawn (l, &error, gimv, "-d", images, NULL);
   g_assert_no_error (error);
   g_subprocess_wait_async (proc, NULL, cb_waited, &proc_ended);
   g_object_unref (l);
   pid = atol (g_subprocess_get_identifier (proc));

   /* the thumbnails */
   end = g_get_monotonic_time () + 60 * G_USEC_PER_SEC;
   while (g_get_monotonic_time () < end) {
      for (found = 0, i = 0; i < N_IMAGES; i++)
         if (g_file_test (thumbs[i], G_FILE_TEST_IS_REGULAR)) found++;
      if (found == N_IMAGES) break;
      if (exited (proc)) break;
      g_usleep (100 * 1000);
   }
   if (exited (proc)) {
      show_gimv_log ();
      g_error ("gimv ended before closing it");
   }
   if (found < N_IMAGES) {
      g_subprocess_force_exit (proc);
      show_gimv_log ();
      g_error ("%u of %u thumbnails made in 60 seconds", found, N_IMAGES);
   }
   for (i = 0; i < N_IMAGES; i++) {
      GdkPixbuf *p = gdk_pixbuf_new_from_file (thumbs[i], &error);
      g_assert_no_error (error);
      g_assert_cmpint (gdk_pixbuf_get_width (p), ==, 128);
      g_object_unref (p);
   }

   /* closed: gimv quits */
   end = g_get_monotonic_time () + 30 * G_USEC_PER_SEC;
   while (g_get_monotonic_time () < end && !exited (proc)) {
      if (!sent) sent = close_windows (d, pid);
      g_usleep (100 * 1000);
   }
   if (!exited (proc)) {
      g_subprocess_force_exit (proc);
      show_gimv_log ();
      g_error ("gimv still runs 30 seconds after its window was closed (%u windows)", sent);
   }
   g_assert_cmpuint (sent, >=, 1);
   g_assert_true (g_subprocess_get_if_exited (proc));
   if (g_subprocess_get_exit_status (proc) != 0) show_gimv_log ();
   g_assert_cmpint (g_subprocess_get_exit_status (proc), ==, 0);

   /* the preferences saved on the way out */
   g_assert_cmpint (g_stat (rc, &rc_after), ==, 0);
   g_assert_cmpint (rc_after.st_mtime, !=, rc_before.st_mtime);

   XCloseDisplay (d);
   g_object_unref (proc);
   for (i = 0; i < N_IMAGES; i++) g_free (thumbs[i]);
   g_free (rc);
   g_free (cache);
   g_free (images);
   g_free (home);
}


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   tmp = gimv_test_mkdtemp ();
   g_test_add_func ("/startup/thumbnails-and-quit", test_startup);
   ret = g_test_run ();
   gimv_test_rm_rf (tmp);
   g_free (tmp);
   return ret;
}
