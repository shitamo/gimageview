/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * [Rename...] of several files: chosen by what is wanted, each with an
 * example (numbers, the date taken, a text replaced, a text added), the
 * new names shown at once beside the names now.
 *
 * The extension is kept (made lower case if asked).  Nothing is
 * overwritten: a name of a file there, or the same new name for two
 * files, is shown and the files are not renamed until it is changed.  A
 * name taken by another file renamed too is fine when that one goes
 * first (1 -> 2 after 2 -> 3); names going round (a swap) are not.
 * Once renamed, [Undo] gives the names back.
 *
 * Renamed here, a file keeps what gimv knows of it: its thumbnail cache,
 * comment (with the tags given by hand) and bookmark go with it
 * (gimv_thumb_view_rename_extras), and the index follows the rename it
 * sees.  The date of the file does not change.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE   /* renameat2 () */
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gimageview.h"
#include "charset.h"
#include "fileutil.h"
#include "gimv_image_info.h"
#include "gimv_rename_files.h"
#include "gimv_thumb.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"
#include "gtkutils.h"
#include "prefs.h"

#ifdef ENABLE_INDEX
#  include "gimv_index.h"
#endif
#ifdef ENABLE_EXIF
#  include "gimv_exif.h"
#endif


typedef enum {
   MODE_NUMBER,
   MODE_DATE,
   MODE_REPLACE,
   MODE_ADD,
   MODE_N
} RenameMode;

typedef enum {
   ORDER_LIST,          /* as the thumbnails are */
   ORDER_DATE,          /* the date taken */
   ORDER_NAME
} RenameOrder;

typedef enum {
   DATE_FROM_TAKEN,     /* the index or EXIF */
   DATE_FROM_NAME,      /* a date in the file name: none taken */
   DATE_FROM_FILE       /* the modification time: none of them */
} DateFrom;

#define STATE_OK     GIMV_RENAME_OK
#define STATE_SAME   GIMV_RENAME_SAME
#define STATE_BAD    GIMV_RENAME_BAD
#define STATE_EXISTS GIMV_RENAME_EXISTS
#define STATE_TWICE  GIMV_RENAME_TWICE
#define STATE_ROUND  GIMV_RENAME_ROUND
typedef GimvRenameState RowState;

typedef struct RenameDialog_Tag RenameDialog;

typedef struct {
   GimvThumb   *thumb;
   gchar       *path;        /* now (the bytes on the disk) */
   gchar       *dir;
   gchar       *name;        /* the name now, shown (UTF-8) */
   gchar       *stem, *ext;  /* name = stem + ext; ext "" or ".jpg" */
   gchar        date[20];    /* "YYYYMMDDhhmmss", "" unknown */
   DateFrom     date_from;
   guint        list_pos;    /* in the thumbnail list */

   gchar       *new_name;    /* UTF-8; NULL: not a name */
   gchar       *new_path;    /* the bytes on the disk */
   RowState     state;
   gboolean     done;        /* renamed (Undo gives it back) */

   GtkWidget   *new_label, *state_label;
} Row;

struct RenameDialog_Tag {
   GimvThumbView *tv;        /* weak */
   GPtrArray     *rows;      /* in the list order */
   RenameMode     mode;

   GtkWidget     *window;
   GtkWidget     *mode_button[MODE_N];
   GtkWidget     *stack;
   /* numbers */
   GtkWidget     *num_name, *num_start, *num_digits, *num_order;
   /* the date */
   GtkWidget     *date_format, *date_text;
   /* replace */
   GtkWidget     *rep_find, *rep_with, *rep_regex, *rep_case;
   /* add */
   GtkWidget     *add_before, *add_after;

   GtkWidget     *lower_ext;
   GtkWidget     *summary;
   GtkWidget     *rename_button, *close_button;
   gboolean       renamed;   /* the names changed: the button undoes it */
   gboolean       bad_regex; /* the expression to find is wrong */
   gboolean       updating;
};


/******************************************************************************
 *  the rows
 ******************************************************************************/

static void
row_free (Row *r)
{
   g_object_unref (r->thumb);
   g_free (r->path);
   g_free (r->dir);
   g_free (r->name);
   g_free (r->stem);
   g_free (r->ext);
   g_free (r->new_name);
   g_free (r->new_path);
   g_free (r);
}


/* "YYYY-MM-DD hh:mm:ss" or "YYYY:MM:DD hh:mm:ss": the 14 digits; FALSE: no date */
static gboolean
date_digits (const gchar *text, gchar out[20])
{
   guint n = 0;

   for (; text && *text && n < 14; text++)
      if (g_ascii_isdigit (*text)) out[n++] = *text;
   out[n] = '\0';
   if (n < 8) {
      out[0] = '\0';
      return FALSE;
   }
   while (n < 14) out[n++] = '0';
   out[n] = '\0';
   return TRUE;
}


/* a date in a file name, as the indexer reads one (IMG_20190503_101500.jpg,
   2019-05-03 10.15.00.png, ...) */
static gboolean
name_date (const gchar *name, gchar out[20])
{
   static GRegex *re = NULL;
   GMatchInfo *mi = NULL;
   gboolean found = FALSE;

   if (!re)
      re = g_regex_new ("(?<![0-9])((?:19|20)[0-9]{2})[-_.]?([0-9]{2})[-_.]?([0-9]{2})"
                        "(?:(?:[-_ T.]| at )?([0-9]{2})[-_.:h]?([0-9]{2})[-_.:m]?([0-9]{2}))?"
                        "(?![0-9])", G_REGEX_OPTIMIZE, 0, NULL);
   if (!re || !name) return FALSE;
   g_regex_match (re, name, 0, &mi);
   while (g_match_info_matches (mi) && !found) {
      gint n[7] = { 0 }, i;
      for (i = 1; i <= 6; i++) {
         gchar *v = g_match_info_fetch (mi, i);
         n[i] = v && *v ? atoi (v) : 0;
         g_free (v);
      }
      if (n[2] >= 1 && n[2] <= 12 && n[3] >= 1 && n[3] <= 31
          && n[4] < 24 && n[5] < 60 && n[6] < 60)
      {
         g_snprintf (out, 20, "%04d%02d%02d%02d%02d%02d", n[1], n[2], n[3], n[4], n[5], n[6]);
         found = TRUE;
      }
      g_match_info_next (mi, NULL);
   }
   g_match_info_free (mi);
   return found;
}


/* the date the image was taken: the index (the comment's date, EXIF, XMP,
   the name), else EXIF, else a date in the name, else the date of the file */
static void
row_read_date (Row *r)
{
   r->date_from = DATE_FROM_TAKEN;
#ifdef ENABLE_INDEX
   {
      const gchar *d = gimv_index_date_of (r->path);
      if (d && date_digits (d, r->date)) return;
   }
#endif
#ifdef ENABLE_EXIF
   {
      ExifData *ed = gimv_exif_data_new_from_file (r->path);
      if (ed) {
         static const ExifTag tags[] = {
            EXIF_TAG_DATE_TIME_ORIGINAL, EXIF_TAG_DATE_TIME_DIGITIZED, EXIF_TAG_DATE_TIME,
         };
         guint i;
         for (i = 0; i < G_N_ELEMENTS (tags) && !r->date[0]; i++) {
            ExifEntry *e = exif_data_get_entry (ed, tags[i]);
            gchar buf[64];
            if (!e) continue;
            exif_entry_get_value (e, buf, sizeof (buf));
            date_digits (buf, r->date);
         }
         exif_data_unref (ed);
         if (r->date[0]) return;
      }
   }
#endif
   r->date_from = DATE_FROM_NAME;
   if (name_date (r->name, r->date)) return;
   r->date_from = DATE_FROM_FILE;
   {
      struct stat st;
      struct tm tm;
      time_t t;
      if (!stat (r->path, &st) && (t = st.st_mtime, localtime_r (&t, &tm)))
         strftime (r->date, sizeof (r->date), "%Y%m%d%H%M%S", &tm);
   }
}


static Row *
row_new (GimvThumb *thumb, guint list_pos)
{
   const gchar *path = gimv_image_info_get_path (thumb->info);
   const gchar *base = fileutil_basename (path), *dot;
   Row *r = g_new0 (Row, 1);

   r->thumb = g_object_ref (thumb);
   r->path = g_strdup (path);
   r->dir = g_path_get_dirname (path);
   r->list_pos = list_pos;
   r->name = charset_to_internal (base, conf.charset_filename,
                                  conf.charset_auto_detect_fn,
                                  conf.charset_filename_mode);
   if (!r->name) r->name = charset_escape_bytes (base, -1);

   dot = strrchr (r->name, '.');
   if (dot && dot != r->name && dot[1]) {
      r->stem = g_strndup (r->name, dot - r->name);
      r->ext = g_strdup (dot);
   } else {
      r->stem = g_strdup (r->name);
      r->ext = g_strdup ("");
   }
   row_read_date (r);
   return r;
}


/******************************************************************************
 *  the new names
 ******************************************************************************/

static const gchar *
entry_text (GtkWidget *entry)
{
   return gtk_editable_get_text (GTK_EDITABLE (entry));
}


static gint
cmp_list (gconstpointer a, gconstpointer b)
{
   const Row *x = *(Row **) a, *y = *(Row **) b;
   return (gint) x->list_pos - (gint) y->list_pos;
}


static gint
cmp_date (gconstpointer a, gconstpointer b)
{
   const Row *x = *(Row **) a, *y = *(Row **) b;
   gint c = strcmp (x->date, y->date);
   return c ? c : cmp_list (a, b);
}


static gint
cmp_name (gconstpointer a, gconstpointer b)
{
   const Row *x = *(Row **) a, *y = *(Row **) b;
   gint c = g_utf8_collate (x->name, y->name);
   return c ? c : cmp_list (a, b);
}


/* the stem in the mode chosen (no extension); NULL: not made (a bad
   expression) */
static void
make_stems (RenameDialog *rd, gchar **stems)
{
   guint i, n = rd->rows->len;

   rd->bad_regex = FALSE;
   switch (rd->mode) {
   case MODE_NUMBER: {
      GPtrArray *order = g_ptr_array_new ();
      guint start = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (rd->num_start));
      guint digits = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (rd->num_digits));
      guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (rd->num_order));

      for (i = 0; i < n; i++) g_ptr_array_add (order, g_ptr_array_index (rd->rows, i));
      g_ptr_array_sort (order, sel == ORDER_DATE ? cmp_date
                               : sel == ORDER_NAME ? cmp_name : cmp_list);
      for (i = 0; i < n; i++) {
         Row *r = g_ptr_array_index (order, i);
         guint j;
         for (j = 0; j < n && g_ptr_array_index (rd->rows, j) != r; j++) ;
         stems[j] = g_strdup_printf ("%s%0*u", entry_text (rd->num_name), digits, start + i);
      }
      g_ptr_array_free (order, TRUE);
      break;
   }

   case MODE_DATE: {
      guint fmt = gtk_drop_down_get_selected (GTK_DROP_DOWN (rd->date_format));
      const gchar *after = entry_text (rd->date_text);
      GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

      for (i = 0; i < n; i++) {
         Row *r = g_ptr_array_index (rd->rows, i);
         const gchar *d = r->date;
         gchar *base;
         guint k;

         if (!d[0]) {
            stems[i] = NULL;
            continue;
         }
         switch (fmt) {
         case 1:  /* 20261125_203012 */
            base = g_strdup_printf ("%.8s_%.6s", d, d + 8);
            break;
         case 2:  /* 2026-11-25 */
            base = g_strdup_printf ("%.4s-%.2s-%.2s", d, d + 4, d + 6);
            break;
         default: /* 2026-11-25_203012 */
            base = g_strdup_printf ("%.4s-%.2s-%.2s_%.6s", d, d + 4, d + 6, d + 8);
            break;
         }
         /* the same time again: _2, _3 ... in the order of the list */
         k = GPOINTER_TO_UINT (g_hash_table_lookup (seen, base)) + 1;
         g_hash_table_replace (seen, g_strdup (base), GUINT_TO_POINTER (k));
         if (k > 1)
            stems[i] = g_strdup_printf ("%s_%u%s", base, k, after);
         else
            stems[i] = g_strconcat (base, after, NULL);
         g_free (base);
      }
      g_hash_table_destroy (seen);
      break;
   }

   case MODE_REPLACE: {
      const gchar *find = entry_text (rd->rep_find), *with = entry_text (rd->rep_with);
      gboolean regex = gtk_check_button_get_active (GTK_CHECK_BUTTON (rd->rep_regex));
      gboolean cased = gtk_check_button_get_active (GTK_CHECK_BUTTON (rd->rep_case));
      GRegex *re = NULL;

      if (*find) {
         gchar *pattern = regex ? g_strdup (find) : g_regex_escape_string (find, -1);
         re = g_regex_new (pattern, cased ? 0 : G_REGEX_CASELESS, 0, NULL);
         g_free (pattern);
         rd->bad_regex = !re;
      }
      for (i = 0; i < n; i++) {
         Row *r = g_ptr_array_index (rd->rows, i);
         if (!*find)
            stems[i] = g_strdup (r->stem);
         else if (!re)
            stems[i] = NULL;
         else if (regex)
            stems[i] = g_regex_replace (re, r->stem, -1, 0, with, 0, NULL);
         else
            stems[i] = g_regex_replace_literal (re, r->stem, -1, 0, with, 0, NULL);
      }
      if (re) g_regex_unref (re);
      break;
   }

   case MODE_ADD:
   default:
      for (i = 0; i < n; i++) {
         Row *r = g_ptr_array_index (rd->rows, i);
         stems[i] = g_strconcat (entry_text (rd->add_before), r->stem,
                                 entry_text (rd->add_after), NULL);
      }
      break;
   }
}


static gboolean
good_name (const gchar *name)
{
   return name && *name && name[0] != '.' && !strchr (name, '/')
          && strlen (name) <= 255;
}


/******************************************************************************
 *  the plan: what keeps each file from being renamed, and renaming in an
 *  order where no name is taken (the dialog and the tests)
 ******************************************************************************/

/* the file whose path now is path: which one; -1: none */
static gint
now_at (const gchar * const *paths, guint n, const gchar *path)
{
   guint i;
   for (i = 0; i < n; i++)
      if (!strcmp (paths[i], path)) return i;
   return -1;
}


void
gimv_rename_plan_check (const gchar * const *paths, const gchar * const *new_paths,
                        guint n, GimvRenameState *states)
{
   GHashTable *count = g_hash_table_new (g_str_hash, g_str_equal);
   gboolean again = TRUE;
   guint i;

   for (i = 0; i < n; i++)
      if (new_paths[i] && strcmp (new_paths[i], paths[i]))
         g_hash_table_replace (count, (gpointer) new_paths[i],
                               GUINT_TO_POINTER (GPOINTER_TO_UINT (g_hash_table_lookup (count, new_paths[i])) + 1));

   for (i = 0; i < n; i++) {
      struct stat st;

      if (!new_paths[i])
         states[i] = GIMV_RENAME_BAD;
      else if (!strcmp (new_paths[i], paths[i]))
         states[i] = GIMV_RENAME_SAME;
      else if (GPOINTER_TO_UINT (g_hash_table_lookup (count, new_paths[i])) > 1)
         states[i] = GIMV_RENAME_TWICE;
      else if (!lstat (new_paths[i], &st) && now_at (paths, n, new_paths[i]) < 0)
         states[i] = GIMV_RENAME_EXISTS;
      else
         states[i] = GIMV_RENAME_OK;   /* free, or of a file renamed too */
   }

   /* names going round (a swap): every file of the ring */
   {
      gboolean *round = g_new0 (gboolean, n);
      for (i = 0; i < n; i++) {
         guint next = i, steps = 0;
         if (states[i] != GIMV_RENAME_OK) continue;
         for (;;) {
            gint j = now_at (paths, n, new_paths[next]);
            if (j < 0 || states[j] != GIMV_RENAME_OK || ++steps > n) break;
            next = j;
            if (next == i) {
               round[i] = TRUE;
               break;
            }
         }
      }
      for (i = 0; i < n; i++) if (round[i]) states[i] = GIMV_RENAME_ROUND;
      g_free (round);
   }

   /* a file waiting for another: the chain ends at a file that is not
      renamed (it stays there); again till none changes */
   while (again) {
      again = FALSE;
      for (i = 0; i < n; i++) {
         guint next = i, steps = 0;

         if (states[i] != GIMV_RENAME_OK) continue;
         for (;;) {
            gint j = now_at (paths, n, new_paths[next]);
            if (j < 0) break;                    /* a free name */
            next = j;
            if (next == i || ++steps > n) break;   /* not after the above */
            if (states[next] != GIMV_RENAME_OK) {
               states[i] = GIMV_RENAME_EXISTS;
               again = TRUE;
               break;
            }
         }
      }
   }
   g_hash_table_destroy (count);
}


/* src to dest; never over a file there */
static gboolean
rename_no_replace (const gchar *src, const gchar *dest)
{
#ifdef RENAME_NOREPLACE
   if (renameat2 (AT_FDCWD, src, AT_FDCWD, dest, RENAME_NOREPLACE) == 0)
      return TRUE;
   if (errno != EINVAL && errno != ENOSYS) return FALSE;
#endif
   {
      struct stat st;
      if (!lstat (dest, &st)) {
         errno = EEXIST;
         return FALSE;
      }
      return rename (src, dest) == 0;
   }
}


guint
gimv_rename_plan_run (const gchar * const *from, const gchar * const *to, guint n,
                      const gboolean *wanted, gboolean *done,
                      GimvRenameDoneFunc func, gpointer data)
{
   gboolean *left = g_new0 (gboolean, n), moved;
   guint count = 0, i;

   for (i = 0; i < n; i++) {
      left[i] = wanted[i];
      done[i] = FALSE;
   }
   do {
      moved = FALSE;
      for (i = 0; i < n; i++) {
         struct stat st;

         if (!left[i] || !lstat (to[i], &st)) continue;   /* waits for it */
         left[i] = FALSE;
         moved = TRUE;
         if (rename_no_replace (from[i], to[i])) {
            done[i] = TRUE;
            count++;
         }
         if (func) func (i, done[i] ? 0 : errno, data);
      }
   } while (moved);
   /* still waiting: the name stayed taken */
   for (i = 0; i < n; i++)
      if (left[i] && func) func (i, EEXIST, data);
   g_free (left);
   return count;
}


static const gchar *
state_text (RenameDialog *rd, Row *r)
{
   switch (r->state) {
   case STATE_SAME:   return _("No change");
   case STATE_BAD:    return rd->bad_regex ? _("The expression is wrong")
                                           : _("Not a file name");
   case STATE_EXISTS: return _("A file of this name is there");
   case STATE_TWICE:  return _("Another file gets this name");
   case STATE_ROUND:  return _("The names go round (rename in two steps)");
   default:
      if (rd->mode == MODE_DATE && r->date_from == DATE_FROM_FILE)
         return _("No date taken: the date of the file");
      if (rd->mode == MODE_DATE && r->date_from == DATE_FROM_NAME)
         return _("No date taken: the date in the name");
      return "";
   }
}


static void
set_summary (RenameDialog *rd, const gchar *text)
{
   gtk_label_set_text (GTK_LABEL (rd->summary), text);
   gtk_widget_set_tooltip_text (rd->summary, text);
}


/* the new names again, and what keeps each from being renamed */
static void
update_rows (RenameDialog *rd)
{
   guint i, n = rd->rows->len, ok = 0, problems = 0;
   gchar **stems = g_new0 (gchar *, n);
   gboolean lower = gtk_check_button_get_active (GTK_CHECK_BUTTON (rd->lower_ext));

   if (rd->renamed) return;
   rd->updating = TRUE;
   make_stems (rd, stems);

   /* the names */
   for (i = 0; i < n; i++) {
      Row *r = g_ptr_array_index (rd->rows, i);
      gchar *ext = lower ? g_utf8_strdown (r->ext, -1) : g_strdup (r->ext);

      g_clear_pointer (&r->new_name, g_free);
      g_clear_pointer (&r->new_path, g_free);
      if (stems[i]) {
         gchar *name = g_strconcat (stems[i], ext, NULL);
         if (good_name (name)) {
            gchar *bytes = charset_internal_to_locale (name);
            if (bytes && good_name (bytes)) {
               r->new_name = name;
               r->new_path = g_build_filename (r->dir, bytes, NULL);
               name = NULL;
            }
            g_free (bytes);
         }
         g_free (name);
      }
      g_free (ext);
   }

   /* what keeps each from being renamed */
   {
      const gchar **paths = g_new (const gchar *, n), **new_paths = g_new (const gchar *, n);
      GimvRenameState *states = g_new (GimvRenameState, n);
      for (i = 0; i < n; i++) {
         Row *r = g_ptr_array_index (rd->rows, i);
         paths[i] = r->path;
         new_paths[i] = r->new_path;
      }
      gimv_rename_plan_check (paths, new_paths, n, states);
      for (i = 0; i < n; i++) ((Row *) g_ptr_array_index (rd->rows, i))->state = states[i];
      g_free (paths);
      g_free (new_paths);
      g_free (states);
   }

   for (i = 0; i < n; i++) {
      Row *r = g_ptr_array_index (rd->rows, i);

      gtk_label_set_text (GTK_LABEL (r->new_label), r->new_name ? r->new_name : "—");
      gtk_label_set_text (GTK_LABEL (r->state_label), state_text (rd, r));
      if (r->state == STATE_OK || r->state == STATE_SAME) {
         gtk_widget_remove_css_class (r->state_label, "error");
         gtk_widget_add_css_class (r->state_label, "dim-label");
      } else {
         gtk_widget_remove_css_class (r->state_label, "dim-label");
         gtk_widget_add_css_class (r->state_label, "error");
      }
      if (r->state == STATE_SAME) gtk_widget_add_css_class (r->new_label, "dim-label");
      else gtk_widget_remove_css_class (r->new_label, "dim-label");

      if (r->state == STATE_OK) ok++;
      else if (r->state != STATE_SAME) problems++;
   }

   {
      guint all = ok + problems;   /* those to rename, once the names are right */
      gchar *label = g_strdup_printf (ngettext ("_Rename %u File", "_Rename %u Files", all), all);
      gtk_button_set_label (GTK_BUTTON (rd->rename_button), label);
      g_free (label);
   }
   /* nothing is renamed while a name is wrong: all or none, as shown */
   gtk_widget_set_sensitive (rd->rename_button, ok > 0 && problems == 0);
   if (problems) {
      gchar *text = g_strdup_printf (ngettext ("%u name to change first (in red).",
                                               "%u names to change first (in red).",
                                               problems), problems);
      set_summary (rd, text);
      g_free (text);
   } else {
      set_summary (rd, ok ? _("The extensions are kept; nothing is overwritten.")
                             : _("No name changes."));
   }

   for (i = 0; i < n; i++) g_free (stems[i]);
   g_free (stems);
   rd->updating = FALSE;
}


static void
cb_changed (GtkWidget *widget, RenameDialog *rd)
{
   if (!rd->updating) update_rows (rd);
}


static void
cb_notify (GObject *object, GParamSpec *pspec, RenameDialog *rd)
{
   if (!rd->updating) update_rows (rd);
}


static void
cb_mode_toggled (GtkToggleButton *button, RenameDialog *rd)
{
   guint i;

   if (!gtk_toggle_button_get_active (button)) return;
   for (i = 0; i < MODE_N; i++) {
      if (rd->mode_button[i] == GTK_WIDGET (button)) {
         static const gchar *pages[] = { "number", "date", "replace", "add" };
         rd->mode = i;
         gtk_stack_set_visible_child_name (GTK_STACK (rd->stack), pages[i]);
      }
   }
   update_rows (rd);
}


/******************************************************************************
 *  renaming
 ******************************************************************************/

typedef struct {
   RenameDialog *rd;
   gboolean      back;
   GString      *failed;
} RunData;


static void
cb_one_done (guint i, gint error, gpointer data)
{
   RunData *d = data;
   Row *r = g_ptr_array_index (d->rd->rows, i);
   const gchar *from = d->back ? r->new_path : r->path;
   const gchar *to = d->back ? r->path : r->new_path;

   if (!error) {
      /* the cache, comment and bookmark go with it */
      gimv_thumb_view_rename_extras_from (r->thumb, from, to);
      return;
   }
   g_string_append_printf (d->failed, "%s → %s: %s\n",
                           d->back ? r->new_name : r->name,
                           d->back ? r->name : r->new_name, g_strerror (error));
}


/* the rows renamed (back: given their names back) */
static guint
rename_rows (RenameDialog *rd, gboolean back, GString *failed)
{
   guint n = rd->rows->len, i, count;
   const gchar **from = g_new (const gchar *, n), **to = g_new (const gchar *, n);
   gboolean *wanted = g_new (gboolean, n), *done = g_new (gboolean, n);
   RunData d = { rd, back, failed };

   for (i = 0; i < n; i++) {
      Row *r = g_ptr_array_index (rd->rows, i);
      from[i] = back ? r->new_path : r->path;
      to[i] = back ? r->path : r->new_path;
      wanted[i] = back ? r->done : r->state == STATE_OK;
   }
   count = gimv_rename_plan_run (from, to, n, wanted, done, cb_one_done, &d);
   for (i = 0; i < n; i++) {
      Row *r = g_ptr_array_index (rd->rows, i);
      if (!back) r->done = done[i];
      else if (done[i]) r->done = FALSE;
   }
   g_free (from);
   g_free (to);
   g_free (wanted);
   g_free (done);
   return count;
}


static void
show_failed (RenameDialog *rd, GString *failed)
{
   gchar *msg;

   if (!failed->len) return;
   msg = g_strdup_printf (_("These files could not be renamed:\n\n%s"), failed->str);
   gtkutil_message_dialog (_("Error!"), msg, GTK_WINDOW (rd->window));
   g_free (msg);
}


static void
set_inputs_sensitive (RenameDialog *rd, gboolean on)
{
   guint i;

   for (i = 0; i < MODE_N; i++) gtk_widget_set_sensitive (rd->mode_button[i], on);
   gtk_widget_set_sensitive (rd->stack, on);
   gtk_widget_set_sensitive (rd->lower_ext, on);
}


static void
cb_rename (GtkWidget *widget, RenameDialog *rd)
{
   GString *failed = g_string_new (NULL);
   guint done, i;

   if (!rd->renamed) {
      update_rows (rd);
      done = rename_rows (rd, FALSE, failed);
      if (rd->tv && done) gimv_thumb_view_refresh_list (rd->tv);
      show_failed (rd, failed);
      if (!done) {
         g_string_free (failed, TRUE);
         update_rows (rd);
         return;
      }
      /* once: the names can be given back */
      rd->renamed = TRUE;
      set_inputs_sensitive (rd, FALSE);
      for (i = 0; i < rd->rows->len; i++) {
         Row *r = g_ptr_array_index (rd->rows, i);
         gtk_label_set_text (GTK_LABEL (r->state_label), r->done ? _("Renamed") : "");
         gtk_widget_remove_css_class (r->state_label, "error");
      }
      {
         gchar *text = g_strdup_printf (ngettext ("%u file renamed.  [Undo] gives the "
                                                  "name back.",
                                                  "%u files renamed.  [Undo] gives the "
                                                  "names back.", done), done);
         set_summary (rd, text);
         g_free (text);
      }
      gtk_button_set_label (GTK_BUTTON (rd->rename_button), _("_Undo"));
      gtk_widget_remove_css_class (rd->rename_button, "suggested-action");
      gtk_widget_set_sensitive (rd->rename_button, TRUE);
      gtk_widget_grab_focus (rd->close_button);
   } else {
      done = rename_rows (rd, TRUE, failed);
      if (rd->tv && done) gimv_thumb_view_refresh_list (rd->tv);
      show_failed (rd, failed);
      gtk_window_destroy (GTK_WINDOW (rd->window));
   }
   g_string_free (failed, TRUE);
}


static void
cb_close (GtkWidget *widget, RenameDialog *rd)
{
   gtk_window_destroy (GTK_WINDOW (rd->window));
}


static void
cb_destroy (GtkWidget *widget, RenameDialog *rd)
{
   if (rd->tv) g_object_remove_weak_pointer (G_OBJECT (rd->tv), (gpointer *) &rd->tv);
   g_ptr_array_unref (rd->rows);
   g_free (rd);
}


/******************************************************************************
 *  the dialog
 ******************************************************************************/

static GtkWidget *
left_label (const gchar *text, const gchar *css)
{
   GtkWidget *label = gtk_label_new (text);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   if (css) gtk_widget_add_css_class (label, css);
   return label;
}


static GtkWidget *
form_grid (void)
{
   GtkWidget *grid = gtk_grid_new ();
   gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
   gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
   return grid;
}


static GtkWidget *
form_row (GtkWidget *grid, gint row, const gchar *mnemonic, GtkWidget *widget)
{
   GtkWidget *label = gtk_label_new_with_mnemonic (mnemonic);
   gtk_label_set_xalign (GTK_LABEL (label), 1.0);
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), widget);
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   gtk_grid_attach (GTK_GRID (grid), widget, 1, row, 1, 1);
   return widget;
}


static GtkWidget *
form_entry (RenameDialog *rd, const gchar *text)
{
   GtkWidget *entry = gtk_entry_new ();
   gtk_widget_set_hexpand (entry, TRUE);
   if (text) gtk_editable_set_text (GTK_EDITABLE (entry), text);
   g_signal_connect (entry, "changed", G_CALLBACK (cb_changed), rd);
   return entry;
}


/* a button of a way to rename: what it does, and an example under it */
static GtkWidget *
mode_button (const gchar *title, const gchar *example)
{
   GtkWidget *button = gtk_toggle_button_new ();
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
   GtkWidget *label = gtk_label_new_with_mnemonic (title);

   gtk_label_set_mnemonic_widget (GTK_LABEL (label), button);
   gtk_widget_add_css_class (label, "heading");
   gtk_box_append (GTK_BOX (box), label);
   label = gtk_label_new (example);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   gtk_box_append (GTK_BOX (box), label);
   gtk_button_set_child (GTK_BUTTON (button), box);
   gtk_widget_set_hexpand (button, TRUE);
   return button;
}


static GtkWidget *
page_number (RenameDialog *rd, const gchar *default_name, guint n)
{
   GtkWidget *grid = form_grid ();
   static const gchar *orders[] = { NULL, NULL, NULL, NULL };
   guint digits = 3;

   while (n >= 1000 && digits < 9) { n /= 10; digits++; }
   orders[0] = _("As in the list");
   orders[1] = _("By date taken");
   orders[2] = _("By name");

   rd->num_name = form_row (grid, 0, _("_Name:"), form_entry (rd, default_name));
   rd->num_start = gtk_spin_button_new_with_range (0, 999999, 1);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (rd->num_start), 1);
   gtk_widget_set_halign (rd->num_start, GTK_ALIGN_START);
   g_signal_connect (rd->num_start, "value-changed", G_CALLBACK (cb_changed), rd);
   form_row (grid, 1, _("_First number:"), rd->num_start);
   rd->num_digits = gtk_spin_button_new_with_range (1, 9, 1);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (rd->num_digits), digits);
   gtk_widget_set_halign (rd->num_digits, GTK_ALIGN_START);
   g_signal_connect (rd->num_digits, "value-changed", G_CALLBACK (cb_changed), rd);
   form_row (grid, 2, _("_Digits:"), rd->num_digits);
   rd->num_order = gtk_drop_down_new_from_strings (orders);
   gtk_widget_set_halign (rd->num_order, GTK_ALIGN_START);
   g_signal_connect (rd->num_order, "notify::selected", G_CALLBACK (cb_notify), rd);
   form_row (grid, 3, _("_Order:"), rd->num_order);
   return grid;
}


static GtkWidget *
page_date (RenameDialog *rd)
{
   GtkWidget *grid = form_grid ();
   static const gchar *formats[] = { "2026-11-25_203012", "20261125_203012",
                                     "2026-11-25", NULL };
   GtkWidget *label;

   rd->date_format = gtk_drop_down_new_from_strings (formats);
   gtk_widget_set_halign (rd->date_format, GTK_ALIGN_START);
   g_signal_connect (rd->date_format, "notify::selected", G_CALLBACK (cb_notify), rd);
   form_row (grid, 0, _("_Form:"), rd->date_format);
   rd->date_text = form_row (grid, 1, _("Text _after it:"), form_entry (rd, NULL));
   gtk_entry_set_placeholder_text (GTK_ENTRY (rd->date_text), _("(none)"));
   /* two lines, not wrapped: a label wrapped as the window is measured
      makes GTK warn ("Trying to measure GtkWindow ... for height") */
   label = left_label (_("The date the image was taken (the index, EXIF), else a date in the "
                         "name or the date of the file.\nImages of the same time get _2, _3 ..."),
                       "dim-label");
   gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 2, 1);
   return grid;
}


static GtkWidget *
page_replace (RenameDialog *rd)
{
   GtkWidget *grid = form_grid (), *expander, *box;

   rd->rep_find = form_row (grid, 0, _("_Find:"), form_entry (rd, NULL));
   rd->rep_with = form_row (grid, 1, _("Replace _with:"), form_entry (rd, NULL));
   gtk_entry_set_placeholder_text (GTK_ENTRY (rd->rep_with), _("(nothing: taken out)"));

   /* for those who want them */
   expander = gtk_expander_new_with_mnemonic (_("_More"));
   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
   rd->rep_regex = gtk_check_button_new_with_mnemonic (_("Regular e_xpression"));
   g_signal_connect (rd->rep_regex, "toggled", G_CALLBACK (cb_changed), rd);
   gtk_box_append (GTK_BOX (box), rd->rep_regex);
   rd->rep_case = gtk_check_button_new_with_mnemonic (_("Match _case"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (rd->rep_case), TRUE);
   g_signal_connect (rd->rep_case, "toggled", G_CALLBACK (cb_changed), rd);
   gtk_box_append (GTK_BOX (box), rd->rep_case);
   gtk_expander_set_child (GTK_EXPANDER (expander), box);
   gtk_grid_attach (GTK_GRID (grid), expander, 0, 2, 2, 1);
   return grid;
}


static GtkWidget *
page_add (RenameDialog *rd)
{
   GtkWidget *grid = form_grid ();

   rd->add_before = form_row (grid, 0, _("_Before the name:"), form_entry (rd, NULL));
   rd->add_after = form_row (grid, 1, _("_After the name:"), form_entry (rd, NULL));
   gtk_entry_set_placeholder_text (GTK_ENTRY (rd->add_after), _("(before the extension)"));
   return grid;
}


void
gimv_rename_files_dialog (GimvThumbView *tv, GList *thumbs)
{
   RenameDialog *rd;
   GtkWidget *vbox, *hbox, *scroll, *grid, *frame;
   GList *node;
   gchar *default_name;
   guint i;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));

   rd = g_new0 (RenameDialog, 1);
   rd->tv = tv;
   g_object_add_weak_pointer (G_OBJECT (tv), (gpointer *) &rd->tv);
   rd->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);
   for (node = thumbs; node; node = g_list_next (node)) {
      GimvThumb *thumb = node->data;
      if (!GIMV_IS_THUMB (thumb) || !thumb->info
          || gimv_image_info_is_dir (thumb->info)
          || (thumb->info->flags & GIMV_IMAGE_INFO_ARCHIVE_MEMBER_FLAG)
          || !gimv_image_info_get_path (thumb->info))
         continue;
      g_ptr_array_add (rd->rows, row_new (thumb, g_list_index (tv->thumblist, thumb)));
   }
   if (!rd->rows->len) {
      cb_destroy (NULL, rd);
      return;
   }
   g_ptr_array_sort (rd->rows, cmp_list);

   /* the folder's name to start with */
   {
      Row *r = g_ptr_array_index (rd->rows, 0);
      gchar *base = g_path_get_basename (r->dir);
      gchar *name = charset_to_internal (base, conf.charset_filename,
                                         conf.charset_auto_detect_fn,
                                         conf.charset_filename_mode);
      default_name = g_strconcat (name && *name && strcmp (name, "/") ? name : _("image"),
                                  "_", NULL);
      g_free (name);
      g_free (base);
   }

   rd->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (rd->window), _("Rename Files"));
   gtk_window_set_transient_for (GTK_WINDOW (rd->window), GTK_WINDOW (tv->tw));
   gtk_window_set_modal (GTK_WINDOW (rd->window), TRUE);
   gtk_window_set_default_size (GTK_WINDOW (rd->window), 760, 600);
   g_signal_connect (rd->window, "destroy", G_CALLBACK (cb_destroy), rd);
   {
      GtkEventController *keys = gtk_shortcut_controller_new ();
      gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
         gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                           gtk_named_action_new ("window.close")));
      gtk_widget_add_controller (rd->window, keys);
   }

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (rd->window), vbox);

   /* the ways, as what is wanted */
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   rd->mode_button[MODE_NUMBER] = mode_button (_("_Numbers"), _("trip_001.jpg, trip_002.jpg"));
   rd->mode_button[MODE_DATE] = mode_button (_("_Date Taken"), "2026-11-25_203012.jpg");
   rd->mode_button[MODE_REPLACE] = mode_button (_("_Replace Text"), _("IMG_ → trip_"));
   rd->mode_button[MODE_ADD] = mode_button (_("_Add Text"), _("cover_ + name"));
   for (i = 0; i < MODE_N; i++) {
      if (i) gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (rd->mode_button[i]),
                                          GTK_TOGGLE_BUTTON (rd->mode_button[0]));
      g_signal_connect (rd->mode_button[i], "toggled", G_CALLBACK (cb_mode_toggled), rd);
      gtk_box_append (GTK_BOX (hbox), rd->mode_button[i]);
   }
   gtk_box_append (GTK_BOX (vbox), hbox);

   rd->stack = gtk_stack_new ();
   gtk_stack_set_vhomogeneous (GTK_STACK (rd->stack), FALSE);
   gtk_stack_add_named (GTK_STACK (rd->stack), page_number (rd, default_name, rd->rows->len), "number");
   gtk_stack_add_named (GTK_STACK (rd->stack), page_date (rd), "date");
   gtk_stack_add_named (GTK_STACK (rd->stack), page_replace (rd), "replace");
   gtk_stack_add_named (GTK_STACK (rd->stack), page_add (rd), "add");
   frame = gtk_frame_new (NULL);
   gtk_widget_set_margin_bottom (rd->stack, 0);
   {
      GtkWidget *pad = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_margin_start (pad, 10);
      gtk_widget_set_margin_end (pad, 10);
      gtk_widget_set_margin_top (pad, 10);
      gtk_widget_set_margin_bottom (pad, 10);
      gtk_box_append (GTK_BOX (pad), rd->stack);
      gtk_frame_set_child (GTK_FRAME (frame), pad);
   }
   gtk_box_append (GTK_BOX (vbox), frame);
   g_free (default_name);

   rd->lower_ext = gtk_check_button_new_with_mnemonic (_("Make the extensions _lower case (.JPG → .jpg)"));
   g_signal_connect (rd->lower_ext, "toggled", G_CALLBACK (cb_changed), rd);
   gtk_box_append (GTK_BOX (vbox), rd->lower_ext);

   /* the names now and new, at once */
   scroll = gtk_scrolled_window_new ();
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_box_append (GTK_BOX (vbox), scroll);
   grid = gtk_grid_new ();
   gtk_grid_set_column_spacing (GTK_GRID (grid), 16);
   gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
   gtk_widget_set_margin_start (grid, 6);
   gtk_widget_set_margin_end (grid, 6);
   gtk_widget_set_margin_top (grid, 6);
   gtk_widget_set_margin_bottom (grid, 6);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), grid);
   gtk_grid_attach (GTK_GRID (grid), left_label (_("Name now"), "heading"), 0, 0, 1, 1);
   gtk_grid_attach (GTK_GRID (grid), left_label (_("New name"), "heading"), 2, 0, 1, 1);
   for (i = 0; i < rd->rows->len; i++) {
      Row *r = g_ptr_array_index (rd->rows, i);
      gtk_grid_attach (GTK_GRID (grid), left_label (r->name, NULL), 0, i + 1, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), left_label ("→", "dim-label"), 1, i + 1, 1, 1);
      r->new_label = left_label (NULL, NULL);
      gtk_grid_attach (GTK_GRID (grid), r->new_label, 2, i + 1, 1, 1);
      r->state_label = left_label (NULL, "dim-label");
      gtk_grid_attach (GTK_GRID (grid), r->state_label, 3, i + 1, 1, 1);
   }

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_box_append (GTK_BOX (vbox), hbox);
   rd->summary = left_label (NULL, "dim-label");
   /* not wrapped (see page_date): cut short, whole in its tooltip */
   gtk_label_set_ellipsize (GTK_LABEL (rd->summary), PANGO_ELLIPSIZE_END);
   gtk_widget_set_hexpand (rd->summary, TRUE);
   gtk_box_append (GTK_BOX (hbox), rd->summary);
   rd->close_button = gtk_button_new_with_mnemonic (_("_Close"));
   g_signal_connect (rd->close_button, "clicked", G_CALLBACK (cb_close), rd);
   gtk_box_append (GTK_BOX (hbox), rd->close_button);
   rd->rename_button = gtk_button_new_with_mnemonic ("");
   gtk_widget_add_css_class (rd->rename_button, "suggested-action");
   g_signal_connect (rd->rename_button, "clicked", G_CALLBACK (cb_rename), rd);
   gtk_box_append (GTK_BOX (hbox), rd->rename_button);

   gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rd->mode_button[MODE_NUMBER]), TRUE);
   update_rows (rd);
   gtk_window_present (GTK_WINDOW (rd->window));
   gtk_widget_grab_focus (rd->num_name);
}
