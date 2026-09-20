gresources = gnome.compile_resources(
  'nemo-resources', 'nemo.gresource.xml',
  source_dir: '.',
  c_name: 'nemo',
  install_header: false,
  install: false
)

if smpl_features_enabled
  gresources += gnome.compile_resources(
    'nemo-default-icons', 'nemo-default-icons.gresource.xml',
    source_dir: '.',
    c_name: 'nemo_default_icons',
    install_header: false,
    install: false,
  )
  install_subdir('default-icons/licenses',
    install_dir: nemoDataPath / 'default-icons',
  )
  install_data('default-icons/README.md', 'default-icons/origins.json',
    install_dir: nemoDataPath / 'default-icons',
  )
endif

layout_editor_gresources = gnome.compile_resources(
  'nemo-action-layout-editor-resources', 'nemo-action-layout-editor.gresource.xml',
  source_dir: '.',
  gresource_bundle: true,
  install: true,
  install_dir: nemoDataPath
)
