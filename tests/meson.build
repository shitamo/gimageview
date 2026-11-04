# The tests: meson test -C _build
#
# Each test is a GLib test program (TAP output).  The tests of known bugs
# not fixed yet are reported as TODO; GIMV_TEST_KNOWN_BUGS=1 runs them.

test_env = environment()
test_env.set('TZ', 'UTC')
test_env.set('LC_ALL', 'C')
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
test_env.set('GIMV_TEST_BUILD_ROOT', meson.project_build_root())
test_env.set('GIMV_TEST_GIMV', gimv_exe.full_path())
# no settings, caches or GIO modules of the user
test_env.set('GIO_USE_VFS', 'local')
test_env.set('GSETTINGS_BACKEND', 'memory')
if sqlite_dep.found()
  test_env.set('GIMV_TEST_INDEXER', gimv_indexer_exe.full_path())
endif

test_kwargs = {
  'env' : test_env,
  'protocol' : 'tap',
  'args' : ['--tap'],
  # some load the plugins of the build tree
  'depends' : gimv_plugins,
}

test_inc = [top_inc, src_inc, index_inc, include_directories('.')]

# src/fileutil.c (GLib only)
test('fileutil',
  executable('test-fileutil', 'test_fileutil.c',
    objects : gimv_exe.extract_objects('fileutil.c'),
    include_directories : test_inc,
    dependencies : [glib_dep]),
  kwargs : test_kwargs)

# src/gimv_orient.c (gdk-pixbuf only)
test('orient',
  executable('test-orient', 'test_orient.c',
    objects : gimv_exe.extract_objects('gimv_orient.c'),
    include_directories : test_inc,
    dependencies : [glib_dep, pixbuf_dep]),
  kwargs : test_kwargs)

# gimv without its main (): for the plugins, which take their symbols
# from the program that loads them
gimv_test_core = static_library('gimv-test-core', gimv_main_source,
  objects : gimv_exe.extract_objects(gimv_sources),
  c_args : ['-Dmain=gimv_test_unused_main'],
  include_directories : [top_inc, src_inc, index_inc],
  dependencies : common_deps + [exif_dep, sqlite_dep, jpeg_dep])

core_test_kwargs = {
  'link_whole' : gimv_test_core,
  'export_dynamic' : true,
  'include_directories' : test_inc,
  'dependencies' : common_deps + [exif_dep, sqlite_dep, jpeg_dep],
}

test('image-loaders',
  executable('test-image-loaders', 'test_image_loaders.c', 'gimv_test_core.c',
    kwargs : core_test_kwargs),
  kwargs : test_kwargs)
