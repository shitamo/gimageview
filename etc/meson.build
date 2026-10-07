i18n = import('i18n')
if get_option('nls')
  i18n.merge_file(input : 'gimageview.desktop.in', output : 'gimageview.desktop',
    type : 'desktop', po_dir : '../po', install : true,
    install_dir : datadir / 'applications')
else
  configure_file(input : 'gimageview.desktop.in', output : 'gimageview.desktop',
    copy : true, install : true, install_dir : datadir / 'applications')
endif
install_data('gimv.png', install_dir : datadir / 'icons' / 'hicolor' / '48x48' / 'apps')
install_data('gimv.png', install_dir : datadir / 'pixmaps')
install_data(['gimv.css', 'mplayerrc'], install_dir : gimv_datadir)
