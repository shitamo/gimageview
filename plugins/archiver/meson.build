# Archives are read with libarchive (the lha/rar/tar/zip plugins of 0.2.27 ran
# the external programs and parsed their output).
if libarchive_dep.found()
  shared_module('gimv_libarchive_extarc', 'libarchive-ext.c',
    dependencies : common_deps + [libarchive_dep],
    install_dir : plugindir / 'archiver',
    kwargs : plugin_kwargs)
endif
