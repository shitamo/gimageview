dir = plugindir / 'io_stream'
if zlib_dep.found()
  shared_module('gimv_gzip_io', 'gimv_gzip_io.c',
    dependencies : common_deps + [zlib_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
if bz2_dep.found()
  shared_module('gimv_bz_io', 'gimv_bz_io.c',
    dependencies : common_deps + [bz2_dep], install_dir : dir, kwargs : plugin_kwargs)
endif
