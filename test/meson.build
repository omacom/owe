unit_test = executable('owe-unit-test',
  files('unit.c') + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc])

test('unit', unit_test)

ipc_clients_test = executable('owe-ipc-clients-test',
  files('ipc_clients.c', '../src/common/common_ipc.c'), include_directories : common_inc)
test('ipc-clients', ipc_clients_test)
test('benchmark', find_program('python3'), args : files('benchmark.py'))

supervisor_test = executable('owe-supervisor-test',
  files('supervisor.c', '../src/common/common_ipc.c', '../src/common/log.c',
    '../src/common/json.c', '../src/common/xdg.c') + yyjson_sources,
  include_directories : [common_inc, yyjson_inc, include_directories('../src/daemon')])
test('supervisor', supervisor_test)

power_test = executable('owe-power-test',
  files('power.c', '../src/common/log.c'),
  include_directories : [common_inc, include_directories('../src/daemon')],
  dependencies : libsystemd_dep)
test('power', power_test)

egl_context_test = executable('owe-egl-context-test',
  files('egl_context.c', '../src/common/log.c'),
  include_directories : [common_inc, include_directories('../src/render')],
  dependencies : [egl_dep, epoxy_dep, wayland_egl],
  c_args : ['-ffunction-sections', '-fdata-sections'],
  link_args : ['-Wl,--gc-sections'])
test('egl-context', egl_context_test, env : ['LIBGL_ALWAYS_SOFTWARE=1'])

wayland_lifecycle_test = executable('owe-wayland-lifecycle-test',
  files('wayland_lifecycle.c', '../src/common/log.c') +
    [layer_shell_client_h, layer_shell_c, xdg_shell_c, viewporter_client_h, viewporter_c,
     fractional_scale_client_h, fractional_scale_c],
  include_directories : [common_inc, include_directories('..', '../src/render')],
  dependencies : [wayland_client, wayland_egl],
  c_args : ['-ffunction-sections', '-fdata-sections'],
  link_args : ['-Wl,--gc-sections'])
test('wayland-lifecycle', wayland_lifecycle_test)

fake_ffmpeg = executable('fake-ffmpeg', 'fake_ffmpeg.c')
regression = executable('owe-regression-test',
  files('regression.c', '../src/daemon/transcode.c', '../src/daemon/supervisor.c', '../src/daemon/watch.c') + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc, include_directories('../src/daemon')],
  dependencies : threads_dep)
test('regression', regression, args : [fake_ffmpeg.full_path()], depends : fake_ffmpeg, timeout : 30)

media_test = executable('owe-media-test',
  files('media.c', '../src/daemon/transcode.c') + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc, include_directories('../src/daemon')],
  dependencies : threads_dep)
test('media', media_test, timeout : 45)

decode_test = executable('owe-decode-test',
  files('decode.c', '../src/common/log.c', '../src/common/owe_spawn.c'),
  include_directories : [common_inc, include_directories('../src/render')],
  dependencies : [avformat_dep, avcodec_dep, avutil_dep, swscale_dep, threads_dep],
  c_args : ['-ffunction-sections', '-fdata-sections'],
  link_args : ['-Wl,--gc-sections'])
test('decode', decode_test, timeout : 45)
test('uninstall', find_program('bash'),
  args : [files('uninstall.sh'), meson.project_source_root() / 'packaging/uninstall.sh'])

test('daemon', find_program('python3'),
  args : [files('daemon.py'), owed.full_path()], timeout : 30)

feed_test = executable('owe-feed-test',
  files('feed.c', '../src/common/common_ipc.c', '../src/common/xdg.c', '../src/common/log.c'),
  include_directories : [common_inc, include_directories('../src/render')],
  dependencies : epoxy_dep)
test('feed', feed_test)

lock_policy_test = executable('owe-lock-policy-test',
  files('lock_policy.c', '../src/daemon/policy.c') + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc, include_directories('../src/daemon')],
  c_args : ['-ffunction-sections', '-fdata-sections'],
  link_args : ['-Wl,--gc-sections'])
test('lock-policy', lock_policy_test)

render_feed_test = executable('owe-render-feed-test',
  files('render_feed.c', '../src/common/common_ipc.c'),
  include_directories : [common_inc, yyjson_inc, include_directories('../src/render')],
  c_args : ['-ffunction-sections', '-fdata-sections'],
  link_args : ['-Wl,--gc-sections'])
test('render-feed', render_feed_test)

# This test needs an explicitly started daemon and a video wallpaper.
executable('owe-live-test', files('live.c') + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc])
