project('flowtable_dpdk', 'c',
    version : '1.0.0',
    # default_options : ['warning_level=3', 'werror=true']
)

# Tự động check và lấy toàn bộ thư viện DPDK hệ thống
dpdk_dep = dependency('libdpdk', required : true)

# Khai báo thư mục chứa file header (.h)
inc = include_directories('include')

# Gom tất cả các file module lại để compile 
sources = files(
    'src/main.c',
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
