daemon_sources = files(
  'main.c',
  'watch.c',
  'hypr.c',
  'power.c',
  'transcode.c',
  'policy.c',
  'daemon_ipc.c',
  'supervisor.c',
  'shell.c',
)

owed = executable('owed',
  daemon_sources + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc],
  dependencies : [wayland_client, libsystemd_dep, threads_dep],
  install : true)
