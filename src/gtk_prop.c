/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * gtk_prop.c
 *
 * Copyright (C) 1999 Rasca, Berlin
 * EMail: thron@gmx.de
 *
 * Olivier Fourdan (fourdan@xfce.org)
 * Heavily modified as part of the Xfce project (http://www.xfce.org)
 *
 * 2001/11/13
 * Takuro Ashie (ashie@homa.ne.jp)
 * Modified as part of GImageView project
 *    (http://gtkmmviewer.sourceforge.net)
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: gtk_prop.c,v 1.12 2004/09/21 08:44:32 makeinu Exp $
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"
#include <gtk/gtk.h>
#include "intl.h"
#include "gtk_prop.h"

#ifdef DMALLOC
#  include "dmalloc.h"
#endif

/* FIXME? */
#include "prefs.h"
#include "charset.h"
/* END FIXME? */


#define box_pack_start(box,w) \
	gimv_box_pack_start(GTK_BOX(box),w,TRUE,FALSE,0)
#define box_pack_end(box,w) \
	gimv_box_pack_end(GTK_BOX(box),w,TRUE,FALSE,0)

#define X_PAD 8
#define Y_PAD 1
#define TBL_XOPT GIMV_EXPAND

typedef struct
{
   GtkWidget *top;
   GtkWidget *user;
   GtkWidget *group;
   fprop *prop;
   int result;
   int type;
} dlg;

static dlg dl;


/*
 */
static GtkWidget *
label_new (const char *text, GtkJustification j_type)
{
   GtkWidget *label;
   label = gtk_label_new (text);
   gtk_label_set_justify (GTK_LABEL (label), j_type);
   gimv_misc_set_alignment (label, 0.0, 0.5);
   /* j_type == GTK_JUSTIFY_RIGHT? 1.0 : 0.0, 0.5); */
   return (label);
}


/*
 */
static void
on_cancel (GtkWidget * btn, gpointer * data)
{
   if ((int) ((long) data) != DLG_RC_DESTROY) {
      dl.result = (int) ((long) data);
      gimv_widget_destroy (dl.top);
   }
   gimv_main_quit ();
}


/*
 */
static void
on_ok (GtkWidget * ok, gpointer * data)
{
   const char *val;
   struct passwd *pw;
   struct group *gr;

   val = gtk_editable_get_text (GTK_EDITABLE (dl.user));
   if (val) {
      pw = getpwnam (val);
      if (pw) {
         dl.prop->uid = pw->pw_uid;
      }
   }
   val = gtk_editable_get_text (GTK_EDITABLE (dl.group));
   if (val) {
      gr = getgrnam (val);
      if (gr) {
         dl.prop->gid = gr->gr_gid;
      }
   }
   gimv_widget_destroy (dl.top);

   dl.result = (int) ((long) data);
   gimv_main_quit ();
}


/*
 */
static void
cb_perm (GtkWidget * toggle, void *data)
{
   int bit = (int) ((long) data);
   if (gimv_toggle_get_active (GTK_WIDGET (toggle)))
      dl.prop->mode |= (mode_t) bit;
   else
      dl.prop->mode &= (mode_t) ~ bit;
}


/*
 */
static gint
on_key_press (GtkWidget * w, GimvEventKey * event, void *data)
{
   if (event->keyval == GDK_KEY_Escape) {
      on_cancel ((GtkWidget *) data, (gpointer) ((long) DLG_RC_CANCEL));
      return (TRUE);
   }
   return (FALSE);
}


/*
 * create a modal dialog for properties and handle it
 */
gint
dlg_prop (const gchar *path, fprop * prop, gint flags)
{
   GtkWidget *ok = NULL, *cancel = NULL, *label, *skip, *all, *notebook, *table;
   GtkWidget *owner[4], *perm[15], *info[12];
   struct tm *t;
   struct passwd *pw;
   struct group *gr;
   char buf[PATH_MAX + 1];
   GList *g_user = NULL;
   GList *g_group = NULL, *g_tmp;
   int n, len;
#ifndef LINE_MAX
#define LINE_MAX	1024
#endif
   char line[LINE_MAX + 1];


   dl.result = 0;
   dl.prop = prop;
   dl.top = gtk_dialog_new ();
   gtk_window_set_title (GTK_WINDOW (dl.top), _("Properties"));
   gimv_window_set_default_transient (GTK_WINDOW (dl.top));
   g_signal_connect (G_OBJECT (dl.top), "destroy",
                       G_CALLBACK (on_cancel),
                       (gpointer) ((long) DLG_RC_DESTROY));
   gtk_window_set_modal (GTK_WINDOW (dl.top), TRUE);
   /* GTK4: gtk_window_set_position (GTK_WIN_POS_MOUSE) is not available */

   notebook = gtk_notebook_new ();
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (dl.top))), notebook,
                       TRUE, TRUE, 0);

   /* ok and cancel buttons */
   ok = gtk_button_new_with_label (_("OK"));
   cancel = gtk_button_new_with_label (_("Cancel"));

   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (dl.top))), ok,
                       TRUE, FALSE, 0);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (dl.top))),
                       cancel, TRUE, FALSE, 0);

   g_signal_connect (G_OBJECT (ok), "clicked",
                       G_CALLBACK (on_ok),
                       (gpointer) ((long) DLG_RC_OK));
   g_signal_connect (G_OBJECT (cancel), "clicked",
                       G_CALLBACK (on_cancel),
                       (gpointer) ((long) DLG_RC_CANCEL));
   gtk_window_set_default_widget (GTK_WINDOW (dl.top), ok);

   if (flags & GTK_PROP_MULTI) {
      skip = gtk_button_new_with_label (_("Skip"));
      all = gtk_button_new_with_label (_("All"));
      gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (dl.top))), skip,
                          TRUE, FALSE, 0);
      gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (dl.top))), all,
                          TRUE, FALSE, 0);
      g_signal_connect (G_OBJECT (skip), "clicked",
                          G_CALLBACK (on_cancel),
                          (gpointer) ((long) DLG_RC_SKIP));
      g_signal_connect (G_OBJECT (all), "clicked",
                          G_CALLBACK (on_ok),
                          (gpointer) ((long) DLG_RC_ALL));
   }


   /* date and size page */
   label = gtk_label_new (_("Info"));
   table = gimv_table_new (6, 2, FALSE);
   gtk_notebook_append_page (GTK_NOTEBOOK (notebook), table, label);

   n = 0;
   info[n] = label_new (_("Name:"), GTK_JUSTIFY_RIGHT);
   gimv_table_attach (GTK_WIDGET (table), info[n], 0, 1, n, n + 1, TBL_XOPT, 0, X_PAD, Y_PAD);
   {
      gchar *path_internal;
      path_internal = charset_to_internal (path,
                                           conf.charset_filename,
                                           conf.charset_auto_detect_fn,
                                           conf.charset_filename_mode);
      info[n + 1] = label_new (path_internal, GTK_JUSTIFY_LEFT);
      g_free (path_internal);
   }
   gimv_table_attach (GTK_WIDGET (table), info[n + 1], 1, 2, n, n + 1, TBL_XOPT, 0, 0, 0);
   n += 2;

   if (!(flags & GTK_PROP_NOT_DETECT_TYPE)) {
      pid_t pid;
      int out_fd[2];

      if (pipe (out_fd) == 0) {
         pid = fork ();
         if (pid > 0) {
            char *p;

            close (out_fd[1]);
            len = read (out_fd[0], line, LINE_MAX);
            if (n > 0) {
               line[len] = '\0';
               len = strlen(line);
               if (line[len - 1] == '\n')
                  line[len - 1] = '\0';
            }
            waitpid (pid, NULL, WUNTRACED);
            close (out_fd[0]);

            if ((p = strstr (line, ": ")) != NULL) {
               p += 2;
               info[n + 1] = label_new (p, GTK_JUSTIFY_LEFT);
               info[n] = label_new (_("Type:"), GTK_JUSTIFY_RIGHT);
               gimv_table_attach (GTK_WIDGET (table), info[n], 0, 1, n, n + 1, TBL_XOPT, 0, X_PAD, Y_PAD);
               gimv_table_attach (GTK_WIDGET (table), info[n + 1], 1, 2, n, n + 1, TBL_XOPT, 0, 0, 0);
               n += 2;
            }
         } else if (pid == 0) {
            gchar **argv = g_new0 (gchar *, 3);

            argv[0] = g_strdup ("file");
            argv[1] = g_strdup (path);
            argv[2] = NULL;

            close (out_fd[0]);
            dup2 (out_fd[1], STDOUT_FILENO);

            execvp (argv[0], argv);
         }
      }
   }

   sprintf (buf, _("%ld Bytes"), (unsigned long) prop->size);
   info[n + 1] = label_new (buf, GTK_JUSTIFY_LEFT);
   info[n] = label_new (_("Size:"), GTK_JUSTIFY_RIGHT);
   gimv_table_attach (GTK_WIDGET (table), info[n], 0, 1, n, n + 1, TBL_XOPT, 0, X_PAD, Y_PAD);
   gimv_table_attach (GTK_WIDGET (table), info[n + 1], 1, 2, n, n + 1, TBL_XOPT, 0, 0, 0);
   n += 2;

   t = localtime (&prop->atime);
   sprintf (buf, "%04d/%02d/%02d  %02d:%02d",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min);
   info[n + 1] = gtk_label_new (buf);
   info[n] = gtk_label_new (_("Access Time:"));
   gimv_table_attach (GTK_WIDGET (table), info[n], 0, 1, n, n + 1, TBL_XOPT, 0, X_PAD, Y_PAD);
   gimv_table_attach (GTK_WIDGET (table), info[n + 1], 1, 2, n, n + 1, TBL_XOPT, 0, 0, 0);
   n += 2;

   t = localtime (&prop->mtime);
   sprintf (buf, "%02d/%02d/%02d  %02d:%02d",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min);
   info[n + 1] = gtk_label_new (buf);
   info[n] = gtk_label_new (_("Modification Time:"));
   gimv_table_attach (GTK_WIDGET (table), info[n], 0, 1, n, n + 1, TBL_XOPT, 0, X_PAD, Y_PAD);
   gimv_table_attach (GTK_WIDGET (table), info[n + 1], 1, 2, n, n + 1, TBL_XOPT, 0, 0, 0);
   n += 2;

   t = localtime (&prop->ctime);
   sprintf (buf, "%04d/%02d/%02d  %02d:%02d",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min);
   info[n + 1] = gtk_label_new (buf);
   info[n] = gtk_label_new (_("Change Time:"));
   gimv_table_attach (GTK_WIDGET (table), info[n], 0, 1, n, n + 1, TBL_XOPT, 0, X_PAD, Y_PAD);
   gimv_table_attach (GTK_WIDGET (table), info[n + 1], 1, 2, n, n + 1, TBL_XOPT, 0, 0, 0);
   n += 2;

   /* permissions page */
   if (!(flags & GTK_PROP_STALE_LINK)) {
      label = gtk_label_new (_("Permissions"));
      table = gimv_table_new (3, 5, FALSE);
      gtk_notebook_append_page (GTK_NOTEBOOK (notebook), table, label);

      perm[0] = gtk_label_new (_("Owner:"));
      perm[1] = gtk_check_button_new_with_label (_("Read"));
      if (prop->mode & S_IRUSR)
         gimv_toggle_set_active (perm[1], TRUE);
      g_signal_connect (G_OBJECT (perm[1]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IRUSR));
      perm[2] = gtk_check_button_new_with_label (_("Write"));
      if (prop->mode & S_IWUSR)
         gimv_toggle_set_active (perm[2], TRUE);
      g_signal_connect (G_OBJECT (perm[2]), "toggled",
                          G_CALLBACK (cb_perm), 
                          (gpointer) ((long) S_IWUSR));
      perm[3] = gtk_check_button_new_with_label (_("Execute"));
      if (prop->mode & S_IXUSR)
         gimv_toggle_set_active (perm[3], TRUE);
      g_signal_connect (G_OBJECT (perm[3]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IXUSR));
      perm[4] = gtk_check_button_new_with_label (_("Set UID"));
      if (prop->mode & S_ISUID)
         gimv_toggle_set_active (perm[4], TRUE);
      g_signal_connect (G_OBJECT (perm[4]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_ISUID));

      gimv_table_attach (GTK_WIDGET (table), perm[0], 0, 1, 0, 1, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[1], 1, 2, 0, 1, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[2], 2, 3, 0, 1, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[3], 3, 4, 0, 1, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[4], 4, 5, 0, 1, 0, 0, X_PAD, 0);

      perm[5] = gtk_label_new (_("Group:"));
      perm[6] = gtk_check_button_new_with_label (_("Read"));
      if (prop->mode & S_IRGRP)
         gimv_toggle_set_active (perm[6], TRUE);
      g_signal_connect (G_OBJECT (perm[6]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IRGRP));
      perm[7] = gtk_check_button_new_with_label (_("Write"));
      if (prop->mode & S_IWGRP)
         gimv_toggle_set_active (perm[7], TRUE);
      g_signal_connect (G_OBJECT (perm[7]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IWGRP));
      perm[8] = gtk_check_button_new_with_label (_("Execute"));
      if (prop->mode & S_IXGRP)
         gimv_toggle_set_active (perm[8], TRUE);
      g_signal_connect (G_OBJECT (perm[8]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IXGRP));
      perm[9] = gtk_check_button_new_with_label (_("Set GID"));
      if (prop->mode & S_ISGID)
         gimv_toggle_set_active (perm[9], TRUE);
      g_signal_connect (G_OBJECT (perm[9]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_ISGID));
      gimv_table_attach (GTK_WIDGET (table), perm[5], 0, 1, 1, 2, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[6], 1, 2, 1, 2, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[7], 2, 3, 1, 2, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[8], 3, 4, 1, 2, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[9], 4, 5, 1, 2, 0, 0, X_PAD, 0);

      perm[10] = gtk_label_new (_("Others:"));
      perm[11] = gtk_check_button_new_with_label (_("Read"));
      if (prop->mode & S_IROTH)
         gimv_toggle_set_active (perm[11], TRUE);
      g_signal_connect (G_OBJECT (perm[11]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IROTH));
      perm[12] = gtk_check_button_new_with_label (_("Write"));
      if (prop->mode & S_IWOTH)
         gimv_toggle_set_active (perm[12], TRUE);
      g_signal_connect (G_OBJECT (perm[12]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IWOTH));
      perm[13] = gtk_check_button_new_with_label (_("Execute"));
      if (prop->mode & S_IXOTH)
         gimv_toggle_set_active (perm[13], TRUE);
      g_signal_connect (G_OBJECT (perm[13]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_IXOTH));
      perm[14] = gtk_check_button_new_with_label (_("Sticky"));
      if (prop->mode & S_ISVTX)
         gimv_toggle_set_active (perm[14], TRUE);
      g_signal_connect (G_OBJECT (perm[14]), "toggled",
                          G_CALLBACK (cb_perm),
                          (gpointer) ((long) S_ISVTX));
      gimv_table_attach (GTK_WIDGET (table), perm[10], 0, 1, 2, 3, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[11], 1, 2, 2, 3, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[12], 2, 3, 2, 3, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[13], 3, 4, 2, 3, 0, 0, X_PAD, 0);
      gimv_table_attach (GTK_WIDGET (table), perm[14], 4, 5, 2, 3, 0, 0, X_PAD, 0);
   }

   if (flags & GTK_PROP_EDITABLE) {
      gint i, n_perms;
      n_perms = sizeof (perm) / sizeof (GtkWidget *);
      for (i = 0; i < n_perms; i++) {
         gtk_widget_set_sensitive (perm[i], FALSE);
      }
   }

   /* owner/group page */
   while ((pw = getpwent ()) != NULL) {
      g_user = g_list_append (g_user, g_strdup (pw->pw_name));
   }
   g_user = g_list_sort (g_user, (GCompareFunc) strcmp);
   endpwent ();

   while ((gr = getgrent ()) != NULL) {
      g_group = g_list_append (g_group, g_strdup (gr->gr_name));
   }
   endgrent ();
   g_group = g_list_sort (g_group, (GCompareFunc) strcmp);

   label = gtk_label_new (_("Owner"));
   table = gimv_table_new (2, 2, FALSE);
   gtk_notebook_append_page (GTK_NOTEBOOK (notebook), table, label);

   pw = getpwuid (prop->uid);
   sprintf (buf, "%s", pw ? pw->pw_name : _("unknown"));
   owner[1] = gimv_combo_new ();
   dl.user = GTK_WIDGET (gimv_combo_get_entry (GTK_WIDGET (owner[1])));
   if (g_user)
      gimv_combo_set_popdown_strings (GTK_WIDGET (owner[1]), g_user);
   gtk_editable_set_text (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (owner[1]))), buf);
   owner[0] = label_new (_("Owner:"), GTK_JUSTIFY_RIGHT);
   gimv_table_attach (GTK_WIDGET (table), owner[0], 0, 1, 0, 1, 0, 0, X_PAD, Y_PAD);
   gimv_table_attach (GTK_WIDGET (table), owner[1], 1, 2, 0, 1, 0, 0, X_PAD, 0);

   gr = getgrgid (prop->gid);
   sprintf (buf, "%s", gr ? gr->gr_name : _("unknown"));
   owner[3] = gimv_combo_new ();
   dl.group = GTK_WIDGET (gimv_combo_get_entry (GTK_WIDGET (owner[3])));
   if (g_group)
      gimv_combo_set_popdown_strings (GTK_WIDGET (owner[3]), g_group);
   gtk_editable_set_text (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (owner[3]))), buf);
   owner[2] = label_new (_("Group:"), GTK_JUSTIFY_RIGHT);
   gimv_table_attach (GTK_WIDGET (table), owner[2], 0, 1, 1, 2, 0, 0, X_PAD, Y_PAD);
   gimv_table_attach (GTK_WIDGET (table), owner[3], 1, 2, 1, 2, 0, 0, X_PAD, 0);

   if (flags & GTK_PROP_EDITABLE) {
      gint i, n_owners;
      n_owners = sizeof (owner) / sizeof (GtkWidget *);
      for (i = 0; i < n_owners; i++) {
         gtk_widget_set_sensitive (owner[i], FALSE);
      }
   }

   gimv_event_connect (GTK_WIDGET (dl.top), GIMV_EVENT_KEY_PRESS, G_CALLBACK (on_key_press), (gpointer) cancel);
   gimv_widget_show_all (dl.top);

   gimv_main ();

   /* free the lists */
   g_tmp = g_user;
   while (g_tmp) {
      g_free (g_tmp->data);
      g_tmp = g_tmp->next;
   }
   g_list_free (g_user);
   g_tmp = g_group;
   while (g_tmp) {
      g_free (g_tmp->data);
      g_tmp = g_tmp->next;
   }
   g_list_free (g_group);
   return (dl.result);
}


gboolean
dlg_prop_from_image_info (GimvImageInfo *info, gint flags)
{
   fprop prop;
   gint rc = DLG_RC_CANCEL;
   const gchar *path;
   gboolean retval = FALSE;

   g_return_val_if_fail (info, FALSE);

   path = gimv_image_info_get_path (info);

   prop.mode  = info->st.st_mode;
   prop.uid   = info->st.st_uid;
   prop.gid   = info->st.st_gid;
   prop.ctime = info->st.st_ctime;
   prop.mtime = info->st.st_mtime;
   prop.atime = info->st.st_atime;
   prop.size  = info->st.st_size;

   rc = dlg_prop (path, &prop, 0);

   switch (rc) {
   case DLG_RC_OK:
   case DLG_RC_ALL:
      if (prop.mode != info->st.st_mode) {
         chmod (path, prop.mode);
         retval = TRUE;
      }
      if ((prop.uid != info->st.st_uid) || (prop.gid != info->st.st_gid)) {
         chown (path, prop.uid, prop.gid);
         retval = TRUE;
      }
   default:
      break;
   }

   return retval;
}
