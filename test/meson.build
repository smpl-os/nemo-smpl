if smpl_features_enabled
  hardening_python = find_program('python3')
  hardening_runner = files('run-isolated-regression.py')

  test('MTP empty folder state', hardening_python,
    args: hardening_runner + [hardening_python.full_path(), files('test-mtp-empty.py')[0]],
    suite: 'mtp-state',
    timeout: 60,
  )

  test('Icon fallback inventory', hardening_python,
    args: files('test-icon-inventory.py'),
    suite: 'icons',
  )
  icon_fallback_test = executable('test-icon-fallback',
    'test-icon-fallback.c',
    include_directories: [rootInclude],
    dependencies: [gtk, nemo_private],
    c_args: nemo_definitions,
    link_args: ['-Wl,--wrap=gtk_icon_theme_has_icon'],
  )
  test('Embedded icon fallbacks', hardening_python,
    args: hardening_runner + [icon_fallback_test],
    suite: 'icons',
    timeout: 60,
  )

  test('Sidebar async hardening', hardening_python,
    args: files('test-sidebar-async.py'),
    suite: 'hardening',
    timeout: 60,
  )

  ui_regressions = [
    executable('test-nemo-file-hierarchy',
      'test-nemo-file-hierarchy.c',
      include_directories: [rootInclude],
      dependencies: [gtk, nemo_private],
    ),
    executable('test-nemo-paged-viewer',
      ['test-nemo-paged-viewer.c', '../src/nemo-preview-utils.c'],
      include_directories: [rootInclude],
      dependencies: nemo_deps,
    ),
    executable('test-nemo-preview-pane-async',
      [
        'test-nemo-preview-pane-async.c',
        '../src/nemo-image-viewer.c',
        '../src/nemo-paged-viewer.c',
        '../src/nemo-preview-utils.c',
      ],
      include_directories: [rootInclude],
      dependencies: nemo_deps,
      c_args: nemo_definitions,
    ),
    executable('test-nemo-preview-async',
      [
        'test-nemo-preview-async.c',
        '../src/nemo-paged-viewer.c',
        '../src/nemo-dir-analyzer.c',
      ],
      include_directories: [rootInclude],
      dependencies: nemo_deps,
      c_args: nemo_definitions,
    ),
    executable('test-nemo-properties-volume',
      [
        'test-nemo-properties-volume.c',
        '../src/nemo-error-reporting.c',
        '../src/nemo-desktop-item-properties.c',
      ],
      include_directories: [rootInclude],
      dependencies: [gtk, nemo_private],
      c_args: nemo_definitions + ['-ffunction-sections', '-fdata-sections'],
      link_args: ['-Wl,--gc-sections'],
    ),
  ]
  foreach regression : ui_regressions
    test(regression.name(), hardening_python,
      args: hardening_runner + [regression],
      suite: 'hardening',
      timeout: 60,
      is_parallel: false,
    )
  endforeach
endif


test('Search Engine test',
  executable('test-nemo-search-engine',
    [ 'test-nemo-search-engine.c' ],
    include_directories: [ rootInclude, ],
    dependencies: [ gtk, nemo_private ],
  ),
  args: []
)

test('Directory Async test',
  executable('test-nemo-directory-async',
    [ 'test-nemo-directory-async.c' ],
    include_directories: [ rootInclude, ],
    dependencies: [ gtk, nemo_private ],
  ),
  args: []
)

test('Copy test',
  executable('test-nemo-copy',
    [ 'test-copy.c', 'test.c' ],
    include_directories: [ rootInclude, ],
    dependencies: [ gtk, nemo_private ],
    c_args: nemo_definitions,
  ),
  args: []
)

if smpl_features_enabled
  copy_integrity_schemas = custom_target('copy-integrity-schemas',
    input: '../libnemo-private/org.nemo.gschema.xml',
    output: 'gschemas.compiled',
    command: [
      find_program('glib-compile-schemas'),
      '--strict',
      '--targetdir', '@OUTDIR@',
      meson.project_source_root() / 'libnemo-private',
    ],
  )

  copy_integrity = executable('test-copy-integrity',
    'test-copy-integrity.c',
    include_directories: [ rootInclude ],
    dependencies: [ gtk, nemo_private ],
    c_args: nemo_definitions,
    link_args: [
      '-Wl,--wrap=open',
      '-Wl,--wrap=open64',
      '-Wl,--wrap=read',
      '-Wl,--wrap=close',
      '-Wl,--wrap=remove',
      '-Wl,--wrap=fsync',
      '-Wl,--wrap=syncfs',
      '-Wl,--wrap=sync_file_range',
      '-Wl,--wrap=g_file_query_info',
      '-Wl,--wrap=g_file_get_path',
      '-Wl,--wrap=g_file_is_native',
      '-Wl,--wrap=g_file_read',
      '-Wl,--wrap=g_file_copy',
      '-Wl,--wrap=g_file_create',
      '-Wl,--wrap=g_file_make_directory',
      '-Wl,--wrap=g_file_make_symbolic_link',
      '-Wl,--wrap=g_output_stream_write_all',
      '-Wl,--wrap=g_output_stream_close',
      '-Wl,--wrap=g_unix_input_stream_new',
      '-Wl,--wrap=g_input_stream_close',
      '-Wl,--wrap=g_input_stream_read',
      '-Wl,--wrap=g_file_copy_attributes',
      '-Wl,--wrap=g_file_move',
      '-Wl,--wrap=g_file_delete',
    ],
  )

  copy_integrity_base_env = {
    'GSETTINGS_SCHEMA_DIR': meson.current_build_dir(),
    'GSETTINGS_BACKEND': 'memory',
    'GIO_USE_VFS': 'local',
    'GDK_BACKEND': 'x11',
    'LC_ALL': 'C',
    'NO_AT_BRIDGE': '1',
    'TMPDIR': meson.current_build_dir(),
  }
  dbus_run_session = find_program('dbus-run-session', required: false)
  xvfb_run = find_program('xvfb-run', required: false)

  if dbus_run_session.found() and xvfb_run.found()
    foreach scenario : [
      'copy', 'verified-copy', 'private-source', 'chunked-copy',
      'crossfs-copy', 'zero-byte-copy', 'zero-advertised-size-copy',
      'short-read-copy', 'short-read-move', 'readback-short-read', 'pull-short-read',
      'metadata-no-version-move', 'source-changed-move', 'cancel-verification',
      'metadata-preserved', 'default-permissions', 'default-permissions-move',
      'pull-copy', 'pull-chunked-copy', 'pull-read-unsupported', 'pull-syncfs-eio', 'pull-syncfs-eintr',
      'pull-fsync-eio', 'pull-verify-unavailable', 'pull-move-unavailable',
      'pull-failure-cleanup', 'pull-cancel-cleanup',
      'attributes-unsupported', 'attributes-denied', 'attributes-eio',
      'attributes-enospc', 'symlink-attributes-eio',
      'samefs-move', 'fallback-move', 'real-crossfs-move',
      'replace-corrupt', 'skip-all', 'metadata-unknown', 'readback-unavailable',
      'writer-eio', 'writer-enospc', 'writer-eintr', 'final-range-eio',
      'range-einval', 'range-enosys', 'range-eopnotsupp',
      'directory-fsync', 'directory-open', 'cancel-before-write',
      'cancel-during-write', 'cancel-after-publish', 'cancel-before-delete',
      'long-name', 'broken-symlink', 'fifo-symlink', 'folder-copy', 'folder-move',
      'folder-new-copy', 'folder-new-move', 'folder-parent-fsync', 'folder-partial-move',
      'merge-all', 'replace-all', 'rename-conflict', 'stage-collision',
      'cleanup-failure', 'file-over-folder', 'folder-over-file',
      'move-file-over-folder', 'move-folder-over-file',
      'fallback-file-over-folder', 'fallback-folder-over-file',
      'publish-directory-race', 'publish-directory-race-copy', 'folder-target-race',
      'nested-move-file-over-folder', 'nested-move-folder-over-file',
      'metadata-optional-absent', 'metadata-optional-absent-move', 'metadata-type-missing',
    ]
      test('copy-integrity-' + scenario, hardening_python,
        args: hardening_runner + [copy_integrity, scenario],
        env: copy_integrity_base_env,
        depends: copy_integrity_schemas,
        suite: 'copy-integrity',
        protocol: 'tap',
        timeout: 30,
        is_parallel: false,
      )
    endforeach
  else
    warning('Copy integrity tests need dbus-run-session and xvfb-run')
  endif
endif
