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
 * $Id: help.c,v 1.45 2004/12/28 04:27:42 makeinu Exp $
 */

#include "gimageview.h"

#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#  include <gdk-pixbuf/gdk-pixbuf-features.h>

#include "charset.h"
#include "fileutil.h"
#include "gfileutil.h"
#include "help.h"
#include "gimv_icon_stock.h"
#include "menu.h"
#include "gimv_plugin.h"
#include "prefs.h"
#include "text_viewer.h"

#define DOC_HTML_DIR "html"
#define DOC_TEXT_DIR "text"
#define GIMV_MANUAL_FILE "index.html"

typedef struct GimvInfoWin_Tag
{
   GtkWidget *window;
   GtkWidget *scrolled_win;
   GtkWidget *text_box;
} GimvInfoWin;


/* callback functions */
static void cb_open_manual             (gpointer   data,
                                        guint      action,
                                        GimvMenuItem *widget);
static void cb_open_text               (GimvMenuItem *menuitem,
                                        gpointer   filename);
static void cb_gimv_info_win_ok_button (GtkWidget *widget,
                                        GtkWidget *window);
static void cb_open_info               (gpointer   data,
                                        guint      action,
                                        GimvMenuItem *widget);

/* other private functions */
static gchar       *get_doc_dir_name     (const gchar *lang,
                                          const gchar *type);
static GtkWidget   *get_dirlist_sub_menu (GtkWidget   *window,
                                          const gchar *dir,
                                          gpointer     func,
                                          GList      **filelist);


static GimvInfoWin info_win;

static gchar *license = 
N_("Copyright (C) 2001 %s <%s>\n\n"
   "This program is free software; you can redistribute it and/or modify\n"
   "it under the terms of the GNU General Public License as published by\n"
   "the Free Software Foundation; either version 2, or (at your option)\n"
   "any later version.\n\n"

   "This program is distributed in the hope that it will be useful,\n"
   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
   "See the GNU General Public License for more details.\n\n"

   "You should have received a copy of the GNU General Public License\n"
   "along with this program; if not, write to the Free Software\n"
   "Foundation, Inc., 59 Temple Place - Suite 330, Boston,\n"
   "MA 02111-1307, USA.");

static gchar *authors = 
N_("Main Program:\n"
   "    Takuro Ashie <ashie@homa.ne.jp>\n"
   "Document:\n"
   "    Nyan2 <t-nyan2@nifty.com>\n"
   "Logo:\n"
   "    eins <eins@milk.freemail.ne.jp>\n"
   "Translation:\n"
   "\n"
   "Special Thanks:\n"
   "    horam\n"
   "    TAM\n"
   "    Hiroyuki Komatsu\n"
   "    Kazuki Iwamoto\n"
   "    katsu\n"
   "    kourin\n"
   "    jissama\n"
   "    shitamori\n"
   "    knee\n"
   "    matsu\n"
   "    Shlomi Fish\n"
   "    Jin Suh\n"
   "    sheepman\n"
   "    MINAMI Hirokazu\n"
   "    Brent Baccala\n"
   "    Christian Hammers\n"
   "    And all GImageView users");

static gchar *system_info = NULL;

static gchar *plugin_info = NULL;

GimvMenuEntry gimvhelp_menu_items[] =
{
   {N_("/_Manual"),               NULL, cb_open_manual, 0, NULL},
   {N_("/_Document"),             NULL, NULL,           0, "<Branch>"},
   {N_("/_Document/_HTML"),       NULL, NULL,           0, "<Branch>"},
   {N_("/_Document/Plain _Text"), NULL, NULL,           0, "<Branch>"},
   {N_("/_About"),                NULL, cb_open_info,   0, NULL},
   {NULL, NULL, NULL, 0, NULL},
};


GList *html_filelist = NULL;
GList *text_filelist = NULL;


/******************************************************************************
 *
 *   Callback functions for menubar.
 *
 ******************************************************************************/
static void
cb_open_manual(gpointer data, guint action, GimvMenuItem *widget)
{
   gchar *dir, manual[MAX_PATH_LEN], *cmd, buf[BUF_SIZE];
   dir = get_doc_dir_name (NULL, DOC_HTML_DIR);
   g_snprintf (manual, MAX_PATH_LEN, "%s/%s", dir, GIMV_MANUAL_FILE);
   if (file_exists(manual)) {
      g_snprintf (buf, BUF_SIZE, conf.web_browser, manual);
      cmd = g_strconcat (buf, " &", NULL);
      system (cmd);
      g_free (cmd);
   }
   g_free (dir);
}


static void
cb_open_text (GimvMenuItem *menuitem, gpointer data)
{
   gchar *filename = data;
   gchar *cmd = NULL;

   g_return_if_fail (filename);

   if (!conf.text_viewer_use_internal && conf.text_viewer) {
      cmd = g_strconcat (conf.text_viewer, " ", filename, " &", NULL);
      system (cmd);
   } else {
      text_viewer_create (filename);
   }

   g_free (cmd);
}


static void
cb_open_html (GimvMenuItem *menuitem, gpointer data)
{
   gchar *filename = data;
   gchar buf[BUF_SIZE], *cmd;

   g_return_if_fail (filename);

   if (conf.web_browser && *conf.web_browser) {
      g_snprintf (buf, BUF_SIZE, conf.web_browser, filename);
      cmd = g_strconcat (buf, " &", NULL);
      system (cmd);
      g_free (cmd);
   }
}


static void
cb_progurl_clicked (GtkWidget *widget, gchar *url)
{
   gchar buf[BUF_SIZE], *cmd;

   g_return_if_fail (url);

   if (conf.web_browser && *conf.web_browser) {
      g_snprintf (buf, BUF_SIZE, conf.web_browser, url);
      cmd = g_strconcat (buf, " &", NULL);
      system (cmd);
      g_free (cmd);
   }
}


static void
set_copyleft_str (void)
{
   gchar buf[BUF_SIZE];

   g_snprintf (buf, BUF_SIZE, _(license),
               GIMV_PROG_AUTHOR, GIMV_PROG_ADDRESS);

   {
      GtkTextBuffer *buffer;

      buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (info_win.text_box));
      gtk_text_buffer_set_text (buffer, "\0", -1);
   }

   if (*buf) {
      {
         GtkTextBuffer *buffer;

         buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (info_win.text_box));
         gtk_text_buffer_set_text (buffer, buf, -1);
      }
   }
}


static void
cb_gimv_info_change_text (GtkWidget *widget, gchar *text)
{
   g_return_if_fail (info_win.text_box);

   /* GTK4: connected to "toggled" (radio buttons are GtkCheckButtons),
      ignore the button that was deactivated */
   if (!gimv_toggle_get_active (widget)) return;

   if (!text) {
      set_copyleft_str ();
      return;
   }

   {
      GtkTextBuffer *buffer;

      buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (info_win.text_box));
      gtk_text_buffer_set_text (buffer, "\0", -1);
   }

   if (text && *text) {
      {
         GtkTextBuffer *buffer;

         buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (info_win.text_box));
         gtk_text_buffer_set_text (buffer, text, -1);
      }
   }
}


static void
cb_gimv_info_win_ok_button (GtkWidget *widget, GtkWidget *window)
{
   gimv_widget_destroy (window);
}


static void
cb_open_info (gpointer data, guint action, GimvMenuItem *widget)
{
   gimvhelp_open_info_window ();
}


/******************************************************************************
 *
 *   Private functions.
 *
 ******************************************************************************/
static gchar *
get_doc_dir_name (const gchar *lang, const gchar *type)
{
   const gchar * const *names;
   gchar *dir;
   gint i;

   g_return_val_if_fail (type && *type, NULL);

   /* the languages of the messages, e.g. ja_JP.UTF-8, ja_JP, ja, C
      ($LANGUAGE, $LC_ALL, $LC_MESSAGES, $LANG) */
   if (lang) {
      dir = g_strconcat (DOCDIR, "/", type, "/", lang, NULL);
      if (isdir (dir)) return dir;
      g_free (dir);
   }

   names = g_get_language_names ();
   for (i = 0; names && names[i]; i++) {
      dir = g_strconcat (DOCDIR, "/", type, "/", names[i], NULL);
      if (isdir (dir)) return dir;
      g_free (dir);
   }

   /* no documents in this language: English, or else the first language
      that has them instead of an empty menu */
   dir = g_strconcat (DOCDIR, "/", type, "/en", NULL);
   if (isdir (dir)) return dir;
   g_free (dir);

   {
      gchar *base = g_strconcat (DOCDIR, "/", type, NULL);
      GDir *gdir = g_dir_open (base, 0, NULL);
      const gchar *name;
      gchar *found = NULL;

      while (gdir && (name = g_dir_read_name (gdir))) {
         gchar *path = g_build_filename (base, name, NULL);
         if (isdir (path) && (!found || strcmp (path, found) < 0)) {
            g_free (found);
            found = path;
         } else {
            g_free (path);
         }
      }
      if (gdir) g_dir_close (gdir);
      g_free (base);
      return found;
   }
}


static GtkWidget *
get_dirlist_sub_menu (GtkWidget *window, const gchar *dir, gpointer func,
                      GList **filelist)
{
   GtkWidget *menu = NULL;
   GList *node;

   g_return_val_if_fail (filelist, NULL);

   if (!dir) return NULL;;

   menu = gimv_menu_new (window);

   if (!*filelist)
      get_dir (dir, GETDIR_FOLLOW_SYMLINK, filelist, NULL);

   node = *filelist;
   while (node) {
      gchar *filename = node->data;

      node = g_list_next (node);

      if (!filename) continue;
      /* GTK4 port: the style sheet is not a document */
      if (g_str_has_suffix (filename, ".css")) continue;

      {
         gchar *basename = g_path_get_basename (filename);
         gimv_menu_append_item (menu, basename,
                                (GimvMenuActivateFunc) func, filename);
         g_free (basename);
      }

   }

   return menu;
}


/* GTK4 port: plugins grouped by type, with their description and the file
   extensions they handle */
static const gchar *
help_plugin_type_label (const gchar *type)
{
   if (!strcmp (type, GIMV_PLUGIN_IMAGE_LOADER))      return _("Image loaders");
   if (!strcmp (type, GIMV_PLUGIN_IMAGE_SAVER))       return _("Image savers");
   if (!strcmp (type, GIMV_PLUGIN_IO_STREAMER))       return _("I/O streams");
   if (!strcmp (type, GIMV_PLUGIN_EXT_ARCHIVER))      return _("Archivers");
   if (!strcmp (type, GIMV_PLUGIN_THUMB_CACHE))       return _("Thumbnail caches");
   if (!strcmp (type, GIMV_PLUGIN_IMAGEVIEW_EMBEDER)) return _("Movie and audio players");
   if (!strcmp (type, GIMV_PLUGIN_THUMBVIEW_EMBEDER)) return _("Thumbnail views");
   return type;
}


static void
help_plugin_info_append (const gchar *type, GList *list)
{
   GString *str;
   GList *node;

   if (!plugin_info)
      plugin_info = g_strdup ("");

   if (!list) return;

   str = g_string_new (plugin_info);
   g_string_append_printf (str, "[%s]\n\n", help_plugin_type_label (type));

   for (node = list; node; node = g_list_next (node)) {
      GModule *module = node->data;
      const gchar *name   = gimv_plugin_get_name (module);
      const gchar *author = gimv_plugin_get_author (module);
      const gchar *desc   = gimv_plugin_get_description (module);
      gchar *exts = gimv_plugin_get_extensions (module);

      g_string_append_printf (str, _("Plugin Name: %s\n"
                                     "Version: %s\n"
                                     "Author: %s\n"),
                              name ? _(name) : "",
                              gimv_plugin_get_version_string (module),
                              author ? _(author) : "");
      if (desc && *desc)
         g_string_append_printf (str, _("Description: %s\n"), _(desc));
      if (exts)
         g_string_append_printf (str, _("Extensions: %s\n"), exts);
      g_string_append_c (str, '\n');
      g_free (exts);
   }

   g_free (plugin_info);
   plugin_info = g_string_free (str, FALSE);
}


/******************************************************************************
 *
 *   Public functions.
 *
 ******************************************************************************/
GtkWidget *
gimvhelp_create_menu (GtkWidget *window)
{
   GtkWidget *menu = NULL, *html_submenu, *text_submenu;
   GimvMenuItem *menuitem = NULL;
   guint n_menu_items;
   gchar *dir, manual[MAX_PATH_LEN];

   n_menu_items = sizeof (gimvhelp_menu_items)
      / sizeof (gimvhelp_menu_items[0]) - 1;
   menu = menu_create_items(window, gimvhelp_menu_items,
                            n_menu_items, "<HelpSubMenu>", NULL);

   dir = get_doc_dir_name (NULL, DOC_HTML_DIR);
   html_submenu = get_dirlist_sub_menu (window, dir, (gpointer) cb_open_html,
                                        &html_filelist);
   if (html_submenu)
      menu_set_submenu (menu, "/Document/HTML", html_submenu);
   g_free (dir);
   dir = NULL;

   dir = get_doc_dir_name (NULL, DOC_TEXT_DIR);
   text_submenu = get_dirlist_sub_menu (window, dir, (gpointer) cb_open_text,
                                        &text_filelist);
   if (text_submenu)
      menu_set_submenu (menu, "/Document/Plain Text", text_submenu);
   g_free (dir);
   dir = NULL;

   dir = get_doc_dir_name (NULL, DOC_HTML_DIR);
   g_snprintf (manual, MAX_PATH_LEN, "%s/%s", dir, GIMV_MANUAL_FILE);
   g_free (dir);
   if (!file_exists(manual)) {
      menuitem  = gimv_menu_get_item (menu, "/Manual");
      if (menuitem)
         gimv_menu_item_set_sensitive (menuitem, FALSE);
   }

   return menu;
}


GtkWidget *
gimvhelp_create_info_widget (void)
{
   GtkWidget *vbox, *hbox1, *hbox2, *tmpvbox;
   GtkWidget *label;
   GtkWidget *button, *radio;
   GtkWidget *scrolledwin, *text;
   GtkWidget *frame, *frame_vbox;
   GtkWidget *logo;
   gchar buf[BUF_SIZE] /* , alt_string[BUF_SIZE] */;
   struct utsname utsbuf;

   /* create info string */
   if (!system_info) {
      uname(&utsbuf);
      g_snprintf (buf, BUF_SIZE,
                  _("Operating System: %s %s %s\n"
                    "GTK version: %d.%d.%d\n"
                    /* "libpng version : %s\n" */),
                  utsbuf.sysname, utsbuf.release, utsbuf.machine,
                  gtk_get_major_version (), gtk_get_minor_version (),
                  gtk_get_micro_version ()
                  /*, png_get_header_ver (NULL)*/);
#if 0
#ifdef ENABLE_MNG
      g_snprintf (alt_string, sizeof (alt_string) / sizeof (gchar),
                  _("libmng version: %s\n"),
                  mng_version_text ());
      strncat (buf, alt_string, BUF_SIZE - strlen (buf));
#endif
      g_snprintf (alt_string, sizeof (alt_string) / sizeof (gchar),
                  _("gdk-pixbuf version: %s\n"),
                  gdk_pixbuf_version);
      strncat (buf, alt_string, BUF_SIZE - strlen (buf));
#ifdef ENABLE_SVG
      g_snprintf (alt_string, sizeof (alt_string) / sizeof (gchar),
                  _("librsvg version: %s\n"),
                  librsvg_version);
      strncat (buf, alt_string, BUF_SIZE - strlen (buf));
#endif
#ifdef ENABLE_XINE
      /*
      g_snprintf (alt_string, sizeof (alt_string) / sizeof (gchar),
                  _("Xine version : %s\n"),
                  xine_get_str_version());
      strncat (buf, alt_string, BUF_SIZE - strlen (buf));
      */
#endif
#endif
      system_info = g_strdup (buf);
   }

   if (!plugin_info) {
      gint idx;
      const gchar *type;

      for (idx = 0; (type = gimv_plugin_type_get (idx)) != NULL; idx++)
         help_plugin_info_append (type, gimv_plugin_get_list (type));
   }

   /* create content widget */
   vbox = gimv_vbox_new (FALSE, 0);

   frame = gtk_frame_new (NULL);
   gimv_container_set_border_width (GTK_WIDGET (frame), 0);
   gimv_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
   gtk_widget_show (frame);

   frame_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (frame), 5);
   gimv_container_add (GTK_WIDGET (frame), frame_vbox);
   gtk_widget_show (frame_vbox);

   logo = gimv_icon_stock_get_widget ("gimageview");
   gimv_box_pack_start (GTK_BOX (frame_vbox), 
                       logo, FALSE, TRUE, 5);
   gimv_icon_stock_free_icon ("gimageview");
   gtk_widget_show (logo);

   tmpvbox = frame_vbox;

   /* Program name & Copyright */ 
   label = gtk_label_new (_(GIMV_PROG_VERSION));
   gimv_box_pack_start (GTK_BOX (tmpvbox), 
                       label, FALSE, FALSE, 0);
   gtk_widget_show (label);

   /* Web Site Button */
   hbox1 = gimv_hbox_new (TRUE, 0);
   gimv_box_pack_start (GTK_BOX (tmpvbox), 
                       hbox1, FALSE, FALSE, 0);
   gtk_widget_show (hbox1);
   hbox2 = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (hbox1), 
                       hbox2, TRUE, FALSE, 0);
   gtk_widget_show (hbox2);

   label = gtk_label_new (_("Website: "));
   gimv_box_pack_start (GTK_BOX (hbox2), 
                       label, FALSE, FALSE, 0);
   gtk_widget_show (label);

   button = gtk_button_new ();
   gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
   label = gtk_label_new (GIMV_PROG_URI);
   gimv_container_add (GTK_WIDGET (button), label);
   gtk_widget_show (label);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_progurl_clicked),
                       GIMV_PROG_URI);
   gimv_box_pack_start (GTK_BOX (hbox2), 
                       button, FALSE, FALSE, 0);
   gtk_widget_show (button);

   /* Infomation Text Box */
   scrolledwin = gimv_scrolled_window_new (NULL, NULL);
   info_win.scrolled_win = scrolledwin;
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrolledwin), TRUE);
   gimv_box_pack_start (GTK_BOX (vbox),
                       scrolledwin, TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (scrolledwin), 5);
   gtk_widget_show (scrolledwin);

   text = gtk_text_view_new ();
   gimv_container_add (GTK_WIDGET (scrolledwin), text);
   info_win.text_box = text;
   set_copyleft_str ();
   gtk_widget_show (text);

   /* Radio Button */
   hbox1 = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), 
                       hbox1, FALSE, FALSE, 0);
   gtk_widget_show (hbox1);
   hbox2 = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (hbox1), 
                       hbox2, TRUE, FALSE, 0);
   gtk_widget_show (hbox2);

   radio = gimv_radio_button_new_with_label (NULL, _("License"));
   g_signal_connect (G_OBJECT (radio), "toggled",
                       G_CALLBACK (cb_gimv_info_change_text), NULL);
   gimv_box_pack_start (GTK_BOX (hbox2), radio, FALSE, FALSE, 0);
   gtk_widget_show (radio);

   radio = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio), _("Authors"));
   g_signal_connect (G_OBJECT (radio), "toggled",
                       G_CALLBACK (cb_gimv_info_change_text),
                       _(authors));
   gimv_box_pack_start (GTK_BOX (hbox2), radio, FALSE, FALSE, 0);
   gtk_widget_show (radio);

   radio = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio), _("System Info"));
   g_signal_connect (G_OBJECT (radio), "toggled",
                       G_CALLBACK (cb_gimv_info_change_text), system_info);
   gimv_box_pack_start (GTK_BOX (hbox2), radio, FALSE, FALSE, 0);
   gtk_widget_show (radio);

   radio = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio), _("Plugin Info"));
   g_signal_connect (G_OBJECT (radio), "toggled",
                       G_CALLBACK (cb_gimv_info_change_text), plugin_info);
   gimv_box_pack_start (GTK_BOX (hbox2), radio, FALSE, FALSE, 0);
   gtk_widget_show (radio);

   return vbox;
}


void
gimvhelp_open_info_window (void)
{
   GtkWidget *widget, *window, *button;
   gchar buf[BUF_SIZE];

   /* window */
   window = gtk_dialog_new ();
   info_win.window = window;
   gimv_container_set_border_width (GTK_WIDGET (gimv_dialog_get_vbox (GTK_WIDGET (window))), 0);
   /* GTK4: window position (GTK_WIN_POS_CENTER) can't be set */
   g_snprintf (buf, BUF_SIZE, _("About %s"), GIMV_PROG_NAME);
   /* GTK4: gtk_window_set_wmclass () removed */
   gtk_window_set_title (GTK_WINDOW (window), buf); 
   gtk_window_set_default_size (GTK_WINDOW (window), 500, 400);

   /* main content */
   widget = gimvhelp_create_info_widget ();
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (window))), 
                       widget, TRUE, TRUE, 0);
   gtk_widget_show (widget);

   /* OK Button */
   button = gtk_button_new_with_label (_("OK"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                       button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_gimv_info_win_ok_button),
                       window);
   gtk_window_set_default_widget (GTK_WINDOW (window), button);
   gtk_widget_show (button);

   gimv_window_set_default_transient (GTK_WINDOW (window));
   gtk_widget_show (window);
   gimv_icon_stock_set_window_icon (window, "gimv_icon");

   gtk_widget_grab_focus (button);

   gimv_grab_add (window);
}
