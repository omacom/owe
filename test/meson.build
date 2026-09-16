unit_test = executable('owe-unit-test',
  files('unit.c') + common_sources + yyjson_sources,
  include_directories : [common_inc, yyjson_inc])

test('unit', unit_test)
