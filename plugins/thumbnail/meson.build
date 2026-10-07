dir = plugindir / 'thumbnail'
foreach p : [['gimv_thumb', 'gimv_thumb.c'], ['gimv_nautilus_thumb', 'nautilus.c'],
             ['gimv_nautilus-2.0_thumb', 'nautilus-2.0.c'], ['gimv_konqueror_thumb', 'konqueror.c'],
             ['gimv_konq_old_thumb', 'konqueror-old.c'], ['gimv_gqview_thumb', 'gqview.c'],
             ['gimv_ee_thumb', 'ee.c'], ['gimv_xvpics_thumb', 'xvpics.c']]
  shared_module(p[0], p[1], dependencies : common_deps, install_dir : dir, kwargs : plugin_kwargs)
endforeach
