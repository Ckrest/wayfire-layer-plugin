project(
    'wayfire-layer-plugin',
    'cpp',
    version: '0.1',
    default_options: [
        'cpp_std=c++17',
        'warning_level=2',
        'werror=false',
    ],
)

wayfire = dependency('wayfire')
wlroots = dependency('wlroots')
wfconfig = dependency('wf-config')

add_project_arguments(['-DWLR_USE_UNSTABLE'], language: ['cpp'])

install_dir = wayfire.get_variable(pkgconfig: 'plugindir')
metadata_dir = wayfire.get_variable(pkgconfig: 'metadatadir')

# Plugin shared library
shared_module(
    'topmost-scene',
    'topmost-scene.cpp',
    dependencies: [wayfire, wlroots, wfconfig],
    install: true,
    install_dir: install_dir,
)

# Plugin metadata
install_data(
    'topmost-scene.xml',
    install_dir: metadata_dir,
)
