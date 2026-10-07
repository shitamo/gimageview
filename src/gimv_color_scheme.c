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
 */

/*
 *  GTK4 port: light / dark color scheme of the user interface.
 *
 *  GTK draws with the theme named by the desktop (gtk-theme-name, from
 *  XSETTINGS on X11).  GTK 4 only reads themes that have a gtk-4.0 (or
 *  gtk-4.N) directory; for other themes it uses its built-in light Adwaita,
 *  so with a dark desktop theme made only for GTK 2/3 (e.g. Greybird-dark)
 *  GTK 4 programs are light.
 *
 *  conf.color_scheme:
 *    GIMV_COLOR_SCHEME_DESKTOP: the desktop theme.  If GTK 4 cannot read it
 *      and its name contains "dark", the dark Adwaita is used.
 *    GIMV_COLOR_SCHEME_LIGHT: the light variant: the theme without the
 *      "-dark" suffix if GTK 4 can read it, otherwise Adwaita.
 *    GIMV_COLOR_SCHEME_DARK: the dark variant of the theme (gtk-dark.css),
 *      or the dark Adwaita if the theme has none.
 *
 *  The color scheme is set with GtkSettings:gtk-interface-color-scheme
 *  (GTK 4.20 or later) or gtk-application-prefer-dark-theme (older GTK).
 *  The environment variable GTK_THEME overrides all of this in GTK, so
 *  nothing is changed when it is set.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <gtk/gtk.h>

#include "gimv_color_scheme.h"
#include "prefs.h"

/* values of GtkInterfaceColorScheme (GTK 4.20) */
#define SCHEME_DEFAULT 1
#define SCHEME_DARK    2
#define SCHEME_LIGHT   3

static gboolean initialized   = FALSE;
static gchar   *initial_theme = NULL;   /* gtk-theme-name before any change */
static gboolean theme_set     = FALSE;  /* gtk-theme-name was set here */
static gboolean scheme_set    = FALSE;  /* the color scheme was set here */
static gboolean applying      = FALSE;


static gboolean
has_property (GtkSettings *settings, const gchar *name)
{
   return g_object_class_find_property (G_OBJECT_GET_CLASS (settings), name)
      != NULL;
}


/* the theme name given by the desktop */
static gchar *
desktop_theme_name (void)
{
   GdkDisplay *display = gdk_display_get_default ();
   GValue value = G_VALUE_INIT;
   gchar *name = NULL;

   g_value_init (&value, G_TYPE_STRING);
   if (display && gdk_display_get_setting (display, "gtk-theme-name", &value)
       && g_value_get_string (&value) && *g_value_get_string (&value))
   {
      name = g_value_dup_string (&value);
   }
   g_value_unset (&value);

   if (!name)
      name = g_strdup (initial_theme ? initial_theme : "Adwaita");

   return name;
}


/* the same directories as GTK 4 (_gtk_css_find_theme ()) */
static gboolean
theme_dir_has_css (const gchar *base, const gchar *file)
{
   gint minor = gtk_get_minor_version ();
   gint i;

   if (!g_file_test (base, G_FILE_TEST_IS_DIR)) return FALSE;

   if (minor % 2) minor++;
   for (i = minor; i >= 0; i -= 2) {
      gchar sub[32];
      gchar *path;
      gboolean found;

      g_snprintf (sub, sizeof (sub), "gtk-4.%d", i);
      path = g_build_filename (base, sub, file, NULL);
      found = g_file_test (path, G_FILE_TEST_EXISTS);
      g_free (path);
      if (found) return TRUE;
   }

   return FALSE;
}


static gboolean
theme_has_gtk4_css (const gchar *name, const gchar *file)
{
   const gchar * const *dirs;
   gchar *base;
   gboolean found;
   gint i;

   base = g_build_filename (g_get_user_data_dir (), "themes", name, NULL);
   found = theme_dir_has_css (base, file);
   g_free (base);
   if (found) return TRUE;

   base = g_build_filename (g_get_home_dir (), ".themes", name, NULL);
   found = theme_dir_has_css (base, file);
   g_free (base);
   if (found) return TRUE;

   dirs = g_get_system_data_dirs ();
   for (i = 0; dirs[i]; i++) {
      base = g_build_filename (dirs[i], "themes", name, NULL);
      found = theme_dir_has_css (base, file);
      g_free (base);
      if (found) return TRUE;
   }

   return FALSE;
}


/* GTK 4 can read the theme (built-in or installed with a gtk-4.x directory) */
static gboolean
theme_is_gtk4 (const gchar *name)
{
   static const gchar *builtin[] = {
      "Default", "Adwaita", "Adwaita-dark", "HighContrast",
      "HighContrastInverse", NULL
   };
   gint i;

   if (!name || !*name) return TRUE;
   for (i = 0; builtin[i]; i++)
      if (!strcmp (name, builtin[i])) return TRUE;

   return theme_has_gtk4_css (name, "gtk.css");
}


static gboolean
theme_name_is_dark (const gchar *name)
{
   gchar *lower;
   gboolean dark;

   if (!name) return FALSE;
   if (!strcmp (name, "HighContrastInverse")) return TRUE;

   lower = g_ascii_strdown (name, -1);
   dark = strstr (lower, "dark") != NULL;
   g_free (lower);

   return dark;
}


/* "Foo-dark" / "Foo_Dark" -> "Foo", NULL if the name has no such suffix */
static gchar *
theme_name_strip_dark (const gchar *name)
{
   gsize len = strlen (name);

   if (len > 5
       && (name[len - 5] == '-' || name[len - 5] == '_')
       && !g_ascii_strcasecmp (name + len - 4, "dark"))
   {
      return g_strndup (name, len - 5);
   }

   return NULL;
}


static void
set_theme_name (GtkSettings *settings, const gchar *name)
{
   gchar *current = NULL;

   g_object_get (settings, "gtk-theme-name", &current, NULL);
   if (g_strcmp0 (current, name))
      g_object_set (settings, "gtk-theme-name", name, NULL);
   g_free (current);
   theme_set = TRUE;
}


/* scheme: SCHEME_DEFAULT (as the desktop), SCHEME_DARK or SCHEME_LIGHT */
static void
set_scheme (GtkSettings *settings, gint scheme)
{
   const gchar *prop;

   if (has_property (settings, "gtk-interface-color-scheme"))
      prop = "gtk-interface-color-scheme";
   else
      prop = "gtk-application-prefer-dark-theme";

   if (scheme == SCHEME_DEFAULT) {
      /* give the property back to the desktop */
      if (scheme_set) {
         GdkDisplay *display = gdk_display_get_default ();
         GValue value = G_VALUE_INIT;
         GParamSpec *pspec;

         gtk_settings_reset_property (settings, prop);
         pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (settings),
                                               prop);
         g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
         if (display && gdk_display_get_setting (display, prop, &value))
            g_object_set_property (G_OBJECT (settings), prop, &value);
         g_value_unset (&value);
         scheme_set = FALSE;
      }
      return;
   }

   if (!strcmp (prop, "gtk-interface-color-scheme")) {
      gint current;
      g_object_get (settings, prop, &current, NULL);
      if (current != scheme)
         g_object_set (settings, prop, scheme, NULL);
   } else {
      gboolean dark = scheme == SCHEME_DARK, current;
      g_object_get (settings, prop, &current, NULL);
      if (!current != !dark)
         g_object_set (settings, prop, dark, NULL);
   }
   scheme_set = TRUE;
}


static void
color_scheme_update (void)
{
   GtkSettings *settings = gtk_settings_get_default ();
   gchar *desktop, *theme = NULL;
   gint scheme = SCHEME_DEFAULT;

   if (!settings || applying) return;
   if (g_getenv ("GTK_THEME") && *g_getenv ("GTK_THEME")) return;

   applying = TRUE;

   desktop = desktop_theme_name ();

   switch (conf.color_scheme) {
   case GIMV_COLOR_SCHEME_LIGHT:
      if (theme_name_is_dark (desktop)) {
         gchar *base = theme_name_strip_dark (desktop);
         if (base && theme_is_gtk4 (base) && !theme_name_is_dark (base))
            theme = base;
         else {
            g_free (base);
            theme = g_strdup ("Adwaita");
         }
      }
      scheme = SCHEME_LIGHT;
      break;
   case GIMV_COLOR_SCHEME_DARK:
      scheme = SCHEME_DARK;
      break;
   case GIMV_COLOR_SCHEME_DESKTOP:
   default:
      if (!theme_is_gtk4 (desktop) && theme_name_is_dark (desktop))
         scheme = SCHEME_DARK;
      break;
   }

   /* the theme name: changed only for the light scheme, given back to the
      desktop's name otherwise */
   if (theme)
      set_theme_name (settings, theme);
   else if (theme_set)
      set_theme_name (settings, desktop);

   set_scheme (settings, scheme);

   g_free (theme);
   g_free (desktop);

   applying = FALSE;
}


/* the desktop changed its theme (XSETTINGS) */
static void
cb_display_setting_changed (GdkDisplay *display, const gchar *name,
                            gpointer data)
{
   if (!strcmp (name, "gtk-theme-name")
       || !strcmp (name, "gtk-interface-color-scheme")
       || !strcmp (name, "gtk-application-prefer-dark-theme"))
   {
      color_scheme_update ();
   }
}


void
gimv_color_scheme_apply (void)
{
   GtkSettings *settings = gtk_settings_get_default ();
   GdkDisplay *display = gdk_display_get_default ();

   if (!settings || !display) return;

   if (!initialized) {
      g_object_get (settings, "gtk-theme-name", &initial_theme, NULL);
      g_signal_connect_after (display, "setting-changed",
                              G_CALLBACK (cb_display_setting_changed), NULL);
      initialized = TRUE;
   }

   color_scheme_update ();
}
