gimv_sources = files(
  'gimv_gtk4_compat.c', 'gimv_object.c', 'gimv_dnd_compat.c',
  'menu.c',
  'gimv_dlist.c', 'gimv_elist.c', 'gimv_cell_pixmap.c', 'gimv_marshal.c',
  'gimv_scrolled.c', 'gimv_zalbum.c', 'gimv_zlist.c',
  'argparse.c', 'auto_completion.c', 'charset.c', 'cursors.c',
  'dirview2.c', 'dither.c', 'dnd.c', 'exif_view.c', 'fileload.c', 'fileutil.c',
  'fr-archive.c', 'fr-command.c', 'fr-process.c', 'gfileutil.c', 'gimageview.c',
  'gimv_anim.c', 'gimv_comment.c', 'gimv_comment_view.c', 'gimv_dupl_finder.c',
  'gimv_dupl_win.c', 'gimv_icon_stock.c', 'gimv_color_scheme.c', 'gimv_image.c', 'gimv_image_info.c',
  'gimv_image_loader.c', 'gimv_image_saver.c', 'gimv_image_view.c',
  'gimv_image_win.c', 'gimv_io.c', 'gimv_io_file.c', 'gimv_io_mem.c',
  'gimv_mime_types.c', 'gimv_nav_win.c', 'gimv_plugin.c', 'gimv_prefs.c',
  'gimv_prefs_ui_utils.c', 'gimv_prefs_win.c', 'gimv_print.c', 'gimv_slideshow.c', 'gimv_thumb.c',
  'gimv_thumb_cache.c', 'gimv_thumb_view.c', 'gimv_thumb_win.c',
  'gtk_prop.c', 'gtkutils.c', 'help.c', 'pixbuf_utils.c',
  'prefs.c', 'text_viewer.c', 'utils.c', 'gimv_xpm.c',
  'compare_filesize.c', 'compare_md5.c', 'compare_similar.c', 'image_view_draw.c',
  'japanese.c', 'thumbnail_view_album.c',
  'prefs_ui/prefs_ui_common.c', 'prefs_ui/prefs_ui_etc.c',
  'prefs_ui/prefs_ui_imagewin.c', 'prefs_ui/prefs_ui_movie.c',
  'prefs_ui/prefs_ui_progs.c', 'prefs_ui/prefs_ui_thumbalbum.c',
  'prefs_ui/prefs_ui_thumbwin.c', 'prefs_ui/prefs_ui_plugin.c')

index_inc = include_directories('indexer')
index_db_src = files('indexer/gimv_index_db.c')

if sqlite_dep.found()
  gimv_sources += files('gimv_index.c', 'gimv_index_view.c', 'gimv_index_tags.c', 'gimv_index_notice.c', 'gimv_index_newimg.c', 'gimv_index_missing.c', 'prefs_ui/prefs_ui_index.c') + index_db_src
  executable('gimv-indexer', files('indexer/gimv_indexer.c') + index_db_src,
    include_directories : [top_inc, index_inc],
    dependencies : [glib_dep, sqlite_dep, exif_dep],
    install : true,
    install_dir : get_option('libexecdir') / 'gimageview')
endif

gimv_exe = executable('gimv', gimv_sources,
  include_directories : [top_inc, src_inc, index_inc],
  dependencies : common_deps + [exif_dep, sqlite_dep],
  export_dynamic : true,
  install : true)
