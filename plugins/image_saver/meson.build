dir = plugindir / 'image_saver'
if png_dep.found()
  shared_module('gimv_png_saver', 'png_saver.c',
    dependencies : common_deps + [png_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
foreach n : ['pnm', 'xvpics']
  shared_module('gimv_' + n + '_saver', n + '_saver.c',
    dependencies : common_deps, install_dir : dir, kwargs : plugin_kwargs)
endforeach
