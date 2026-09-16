render_sources = files(
  'main.c',
  'wayland.c',
  'egl.c',
  'mpv.c',
  'still.c',
  'render_ipc.c',
)

owe_render = executable('owe-render',
  render_sources + common_sources + yyjson_sources + [layer_shell_client_h, layer_shell_c, xdg_shell_c],
  include_directories : [common_inc, yyjson_inc],
  dependencies : [wayland_client, wayland_egl, egl_dep, gles_dep, gl_dep, epoxy_dep, mpv_dep, avformat_dep, avcodec_dep, avutil_dep, swscale_dep, threads_dep],
  install : true)
