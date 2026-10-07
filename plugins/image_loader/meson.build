dir = plugindir / 'image_loader'
foreach n : ['bmp', 'mag', 'pcx', 'pnm', 'ras', 'tga', 'wbmp', 'xbm', 'xcf', 'xpm', 'xvpics']
  shared_module('gimv_' + n + '_loader', n + '.c',
    dependencies : common_deps, install_dir : dir, kwargs : plugin_kwargs)
endforeach
shared_module('gimv_pixbuf_loader', ['pixbuf_anim.c', 'pixbuf_loader.c'],
  dependencies : common_deps, install_dir : dir, kwargs : plugin_kwargs)
if png_dep.found()
  shared_module('gimv_png_loader', 'png_loader.c',
    dependencies : common_deps + [png_dep, zlib_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
if jpeg_dep.found()
  shared_module('gimv_jpeg_loader', 'jpeg_loader.c',
    dependencies : common_deps + [jpeg_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
if mng_dep.found()
  shared_module('gimv_mng_loader', 'mng.c',
    dependencies : common_deps + [mng_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
if rsvg_dep.found()
  shared_module('gimv_rsvg_loader', 'svg.c',
    dependencies : common_deps + [rsvg_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
if wmf_dep.found()
  shared_module('gimv_wmf_loader', 'wmf.c',
    dependencies : common_deps + [wmf_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
