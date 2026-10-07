dir = plugindir / 'image_view'
if get_option('mplayer')
  shared_module('gimv_mplayer',
    ['gimv_mplayer.c', 'image_view_mplayer.c', 'mplayer_image_loader.c', 'prefs_mplayer.c'],
    dependencies : common_deps, install_dir : dir, kwargs : plugin_kwargs)
endif
if xine_dep.found()
  shared_module('gimv_xine',
    ['gimv_xine.c', 'gimv_xine_priv.c', 'gimv_xine_post.c', 'image_view_xine.c',
     'xine_image_loader.c', 'prefs_xine.c'],
    dependencies : common_deps + [xine_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
if have_gst
  shared_module('gimv_gst', ['image_view_gst.c'],
    dependencies : common_deps + [gst_dep, gst_video_dep, gst_app_dep],
    install_dir : dir, kwargs : plugin_kwargs)
endif
