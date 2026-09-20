if smpl_features_enabled
  hardening_python = find_program('python3')
  hardening_runner = files('run-isolated-regression.py')

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
    # xvfb-run places its Xauthority file in the build tree, not global /tmp.
    'TMPDIR': meson.current_build_dir(),
  }
  dbus_run_session = find_program('dbus-run-session', required: false)
  xvfb_run = find_program('xvfb-run', required: false)

  if dbus_run_session.found() and xvfb_run.found()
    copy_integrity_mkdir = find_program('mkdir')
    copy_integrity_clean_env = find_program('env')
    copy_integrity_index = 0
    foreach scenario : [
      'copy', 'verified-copy', 'private-source', 'chunked-copy',
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
      # Prepare HOME before starting either launcher so D-Bus activated
      # services inherit the same isolation as the test. Short indexed paths
      # also leave enough room for Unix sockets in XDG_RUNTIME_DIR.
      copy_integrity_profile = meson.current_build_dir() / 'ci' / copy_integrity_index.to_string()
      copy_integrity_index += 1
      run_command(copy_integrity_mkdir, '-p', '-m', '0700',
        copy_integrity_profile,
        copy_integrity_profile / 'cache',
        copy_integrity_profile / 'config',
        copy_integrity_profile / 'data',
        copy_integrity_profile / 'run',
        check: true,
      )
      copy_integrity_env = environment(copy_integrity_base_env)
      copy_integrity_env.set('NEMO_TEST_PROFILE', copy_integrity_profile)
      copy_integrity_env.set('HOME', copy_integrity_profile)
      copy_integrity_env.set('XDG_CACHE_HOME', copy_integrity_profile / 'cache')
      copy_integrity_env.set('XDG_CONFIG_HOME', copy_integrity_profile / 'config')
      copy_integrity_env.set('XDG_DATA_HOME', copy_integrity_profile / 'data')
      copy_integrity_env.set('XDG_RUNTIME_DIR', copy_integrity_profile / 'run')
      copy_integrity_unset_args = []
      foreach variable : [
        'DISPLAY', 'WAYLAND_DISPLAY', 'AT_SPI_BUS_ADDRESS', 'XAUTHORITY',
        'DBUS_SESSION_BUS_ADDRESS', 'DBUS_STARTER_ADDRESS', 'DBUS_STARTER_BUS_TYPE',
      ]
        copy_integrity_env.unset(variable)
        copy_integrity_unset_args += ['-u', variable]
      endforeach
      # Start Xvfb first: the private bus and its services need its DISPLAY too.
      test('copy-integrity-' + scenario, copy_integrity_clean_env,
        args: copy_integrity_unset_args + [
          'GDK_BACKEND=x11', 'NEMO_TEST_ISOLATED=1', xvfb_run.full_path(), '-a',
          dbus_run_session.full_path(), '--', copy_integrity, scenario,
        ],
        env: copy_integrity_env,
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
