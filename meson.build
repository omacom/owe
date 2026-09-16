project('owe', 'c',
  version : '0.1.0',
  license : 'MIT',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
  ])

cc = meson.get_compiler('c')

add_project_arguments('-D_GNU_SOURCE', language : 'c')

wayland_client = dependency('wayland-client')
wayland_egl = dependency('wayland-egl')
egl_dep = dependency('egl')
gles_dep = dependency('glesv2')
gl_dep = dependency('gl', required : true)
epoxy_dep = dependency('epoxy')
mpv_dep = dependency('mpv', version : '>= 0.35')
avformat_dep = dependency('libavformat')
avcodec_dep = dependency('libavcodec')
avutil_dep = dependency('libavutil')
swscale_dep = dependency('libswscale')
libsystemd_dep = dependency('libsystemd')
threads_dep = dependency('threads')

wayland_scanner = find_program('wayland-scanner')
wayland_protos = dependency('wayland-protocols', required : false)

layer_shell_xml = files('protocols/wlr-layer-shell-unstable-v1.xml')
idle_notify_xml = files('protocols/ext-idle-notify-v1.xml')
xdg_shell_xml = files('protocols/xdg-shell.xml')

xdg_shell_c = custom_target('xdg-shell-code',
  input : xdg_shell_xml,
  output : 'xdg-shell-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

layer_shell_client_h = custom_target('layer-shell-client-header',
  input : layer_shell_xml,
  output : 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

layer_shell_c = custom_target('layer-shell-code',
  input : layer_shell_xml,
  output : 'wlr-layer-shell-unstable-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

idle_notify_client_h = custom_target('idle-notify-client-header',
  input : idle_notify_xml,
  output : 'ext-idle-notify-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

idle_notify_c = custom_target('idle-notify-code',
  input : idle_notify_xml,
  output : 'ext-idle-notify-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

yyjson_sources = files('third_party/yyjson/yyjson.c')
yyjson_inc = include_directories('third_party/yyjson')

common_sources = files(
  'src/common/log.c',
  'src/common/xdg.c',
  'src/common/strutil.c',
  'src/common/config.c',
  'src/common/common_ipc.c',
  'src/common/owe_spawn.c',
)

common_inc = include_directories('src/common')

subdir('src/render')
subdir('src/daemon')
subdir('src/cli')
subdir('test')
