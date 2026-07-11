project('flowtable_dpdk', 'c',
    version : '1.0.0',
    # default_options : ['warning_level=3', 'werror=true']
)

add_project_arguments('-DALLOW_EXPERIMENTAL_API', language: 'c')

# Tự động check và lấy toàn bộ thư viện DPDK hệ thống
dpdk_dep = dependency('libdpdk', required : true)

# Khai báo thư mục chứa file header (.h)
inc = include_directories('include')

# Gom tất cả các file module lại để compile 
sources = files(
    'src/main.c',
    'src/app_init.c',
    'src/flow_table.c',
    'src/dispatcher.c',
    'src/spi_engine.c',
    'src/worker.c',
    'src/stats.c'
)

# Sinh ra file thực thi cuối cùng
executable('flowcore',
    sources,
    include_directories : inc,
    dependencies : dpdk_dep,
    install : false
)

functional_test_exe = executable('flowcore_functional_tests',
    'tests/flowcore_functional_tests.c',
    c_args : [
        '-DPROJECT_ROOT="' + meson.project_source_root() + '"',
        '-DFLOWCORE_BIN="' + meson.project_build_root() + '/flowcore"',
    ],
    install : false
)

test('flowcore-functional', functional_test_exe)

flow_slot_lifecycle_test_exe = executable('flow_slot_lifecycle_tests',
    'tests/flow_slot_lifecycle_tests.c',
    include_directories : inc,
    dependencies : dpdk_dep,
    install : false
)

test('flow-slot-lifecycle', flow_slot_lifecycle_test_exe)
