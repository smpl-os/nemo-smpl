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
