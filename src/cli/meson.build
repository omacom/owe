owe_cli = executable('owe',
  files('main.c') + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc],
  c_args : ['-DOWE_VERSION="' + meson.project_version() + '"'],
  install : true)
