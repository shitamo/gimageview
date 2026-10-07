project('gimageview', 'c',
  version : '0.2.27-gtk4',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.60.0',
  default_options : ['c_std=gnu11', 'warning_level=1', 'buildtype=debugoptimized'])

package = 'gimageview'
cc = meson.get_compiler('c')

prefix     = get_option('prefix')
datadir    = prefix / get_option('datadir')
libdir     = prefix / get_option('libdir')
localedir  = prefix / get_option('localedir')
gimv_datadir = datadir / package
gimv_docdir  = datadir / 'doc' / package
# documents opened from the Help menu (html/<lang>, text/<lang>): not in
# /usr/share/doc, which a program must not depend on (Debian policy 12.3)
gimv_helpdir = gimv_datadir / 'doc'
gimv_icondir = gimv_datadir / 'pixmaps'
plugindir    = libdir / package

gtk_dep   = dependency('gtk4', version : '>= 4.10')
glib_dep  = dependency('glib-2.0', version : '>= 2.72')
gmod_dep  = dependency('gmodule-2.0')
gio_dep   = dependency('gio-2.0')
pixbuf_dep = dependency('gdk-pixbuf-2.0')
m_dep     = cc.find_library('m', required : false)
zlib_dep  = dependency('zlib', required : false)
png_dep   = dependency('libpng', required : false)
jpeg_dep  = dependency('libjpeg', required : get_option('jpeg'))
bz2_dep   = dependency('bzip2', required : false)
if not bz2_dep.found()
  bz2_dep = cc.find_library('bz2', has_headers : ['bzlib.h'], required : false)
endif
mng_dep   = dependency('libmng', required : get_option('mng'))
rsvg_dep  = dependency('librsvg-2.0', required : get_option('svg'))
wmf_dep   = dependency('libwmf', required : get_option('wmf'))
xine_dep  = dependency('libxine', version : '>= 1.2', required : get_option('xine'))
gst_dep   = dependency('gstreamer-1.0', version : '>= 1.16', required : get_option('gstreamer'))
gst_video_dep = dependency('gstreamer-video-1.0', version : '>= 1.16', required : get_option('gstreamer'))
gst_app_dep   = dependency('gstreamer-app-1.0', version : '>= 1.16', required : get_option('gstreamer'))
have_gst = gst_dep.found() and gst_video_dep.found() and gst_app_dep.found()
libarchive_dep = dependency('libarchive', version : '>= 3.3', required : get_option('libarchive'))
exif_dep  = dependency('libexif', version : '>= 0.6.16', required : get_option('exif'))

conf = configuration_data()
conf.set_quoted('PACKAGE', package)
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE', package)
conf.set_quoted('LOCALEDIR', localedir)
conf.set_quoted('DATADIR', gimv_datadir)
conf.set_quoted('DOCDIR', gimv_helpdir)
conf.set_quoted('ICONDIR', gimv_icondir)
conf.set_quoted('PLUGINDIR', plugindir)
conf.set_quoted('ARCHIVER_PLUGIN_DIR', 'archiver')
conf.set_quoted('IMAGE_LOADER_PLUGIN_DIR', 'image_loader')
conf.set_quoted('IMAGE_SAVER_PLUGIN_DIR', 'image_saver')
conf.set_quoted('IO_STREAM_PLUGIN_DIR', 'io_stream')
conf.set_quoted('IMAGE_VIEW_PLUGIN_DIR', 'image_view')
conf.set_quoted('THUMBNAIL_VIEW_PLUGIN_DIR', 'thumbnail_view')
conf.set_quoted('THUMBNAIL_PLUGIN_DIR', 'thumbnail')

conf.set('ENABLE_NLS', get_option('nls') ? 1 : false)
conf.set('HAVE_GETTEXT', 1)
conf.set('HAVE_ICONV', 1)
conf.set('HAVE_GDK_PIXBUF', 1)
conf.set('USE_GTK2', 1)            # kept for any leftover checks; GTK4 is a superset of the GTK2 path
conf.set('USE_NORMAL_PANED', 1)
conf.set('ENABLE_TREEVIEW', 1)
conf.set('ENABLE_EXIF', exif_dep.found() ? 1 : false)
conf.set('ENABLE_SPLASH', get_option('splash') ? 1 : false)
conf.set('ENABLE_JPEG', jpeg_dep.found() ? 1 : false)
conf.set('HAVE_PROGRESSIVE_JPEG', jpeg_dep.found() ? 1 : false)
conf.set('ENABLE_MNG', mng_dep.found() ? 1 : false)
conf.set('ENABLE_SVG', rsvg_dep.found() ? 1 : false)
conf.set('HAVE_LIBRSVG', rsvg_dep.found() ? 1 : false)
conf.set('ENABLE_WMF', wmf_dep.found() ? 1 : false)
conf.set('HAVE_BZLIB', bz2_dep.found() ? 1 : false)
conf.set('BZAPI_NEEDS_PREFIX', bz2_dep.found() ? 1 : false)
conf.set('ENABLE_XINE', xine_dep.found() ? 1 : false)
conf.set('ENABLE_MPLAYER', get_option('mplayer') ? 1 : false)
conf.set('ENABLE_GSTREAMER', have_gst ? 1 : false)
conf.set('ENABLE_MOVIE', (get_option('mplayer') or xine_dep.found() or have_gst) ? 1 : false)

# GTK4 port: Susie (Windows DLL) plugin support and lib/dllloader removed
foreach h : ['dirent.h', 'dlfcn.h', 'errno.h', 'fcntl.h', 'limits.h', 'locale.h',
             'malloc.h', 'stdint.h', 'stdlib.h', 'string.h', 'strings.h',
             'sys/param.h', 'sys/time.h', 'unistd.h', 'langinfo.h', 'libintl.h']
  if cc.has_header(h)
    conf.set('HAVE_' + h.underscorify().to_upper(), 1)
  endif
endforeach
foreach f : ['strdup', 'strcasecmp', 'strsep', 'strstr', 'strtoul', 'mkdir',
             'setlocale', 'nl_langinfo', 'setenv', 'putenv', 'getcwd', 'floor', 'pow', 'sqrt']
  if cc.has_function(f, dependencies : m_dep)
    conf.set('HAVE_' + f.to_upper(), 1)
  endif
endforeach
conf.set('HAVE_BIND_TEXTDOMAIN_CODESET', cc.has_function('bind_textdomain_codeset') ? 1 : false)
conf.set('HAVE_LANGINFO_CODESET', cc.has_header_symbol('langinfo.h', 'CODESET') ? 1 : false)
conf.set('HAVE_LC_MESSAGES', 1)
conf.set('HAVE_SETLOCALE', 1)

configure_file(output : 'config.h', configuration : conf)

add_project_arguments(
  '-DHAVE_CONFIG_H',
  # GtkTreeView/GtkComboBox/GtkDialog & friends are deprecated since 4.10 but
  # remain fully supported for the whole GTK 4 series.
  '-DGDK_DISABLE_DEPRECATION_WARNINGS',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_10',
  cc.get_supported_arguments([
    '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-pointer-sign',
    '-Wno-deprecated-declarations', '-Wno-missing-field-initializers',
    '-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
    '-Werror=int-conversion', '-Werror=return-type',
  ]),
  language : 'c')

top_inc = include_directories('.')
src_inc = include_directories('src')

common_deps = [gtk_dep, glib_dep, gmod_dep, gio_dep, pixbuf_dep, m_dep]

subdir('src')
subdir('plugins')
subdir('etc')
subdir('po')
if import('fs').is_dir('doc')
  subdir('doc')
endif
