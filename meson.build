project(
  'qq-xwayland-screencast-preload',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')

dbus_dep = dependency('dbus-1')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
gio_dep = dependency('gio-2.0')
pipewire_dep = dependency('libpipewire-0.3')
spa_dep = dependency('libspa-0.2')
threads_dep = dependency('threads')
x11_dep = dependency('x11')
xext_dep = dependency('xext')
dl_dep = cc.find_library('dl')

common_c_args = [
  '-D_GNU_SOURCE',
  '-D_FILE_OFFSET_BITS=64',
]

preload = shared_library(
  'qq-xwayland-screencast-preload',
  [
    'src/capture.c',
    'src/frame.c',
    'src/hooks.c',
    'src/portal.c',
    'src/screenshot.c',
  ],
  c_args: common_c_args,
  dependencies: [
    dbus_dep,
    dl_dep,
    gdk_pixbuf_dep,
    gio_dep,
    pipewire_dep,
    spa_dep,
    threads_dep,
    x11_dep,
    xext_dep,
  ],
  gnu_symbol_visibility: 'hidden',
  install: false,
)

frame_test = executable(
  'test-frame',
  [
    'src/frame.c',
    'tests/test-frame.c',
  ],
  c_args: common_c_args,
  include_directories: include_directories('src'),
  dependencies: [spa_dep, x11_dep],
)
test('frame conversion', frame_test)

screenshot_test = executable(
  'test-screenshot',
  [
    'src/frame.c',
    'src/portal.c',
    'src/screenshot.c',
    'tests/test-screenshot.c',
  ],
  c_args: common_c_args,
  include_directories: include_directories('src'),
  dependencies: [
    dbus_dep,
    gdk_pixbuf_dep,
    gio_dep,
    spa_dep,
    threads_dep,
    x11_dep,
  ],
)
test('screenshot conversion', screenshot_test)

screenshot_portal_test = executable(
  'test-screenshot-portal',
  [
    'src/frame.c',
    'src/portal.c',
    'src/screenshot.c',
    'tests/test-screenshot-portal.c',
  ],
  c_args: common_c_args,
  include_directories: include_directories('src'),
  dependencies: [
    dbus_dep,
    gdk_pixbuf_dep,
    gio_dep,
    spa_dep,
    threads_dep,
    x11_dep,
  ],
)
dbus_run_session = find_program('dbus-run-session')
test(
  'screenshot portal protocol',
  dbus_run_session,
  args: ['--', screenshot_portal_test],
  timeout: 30,
)

fake_broadcast_core = shared_module(
  'broadcast-core',
  'tests/fake-broadcast-core.c',
  c_args: common_c_args,
  name_prefix: '',
  dependencies: [x11_dep, xext_dep],
)

fake_wrapper = shared_module(
  'wrapper',
  'tests/fake-wrapper.c',
  c_args: common_c_args,
  name_prefix: '',
  name_suffix: 'node',
  dependencies: [x11_dep],
)

hook_test = executable(
  'test-hooks',
  'tests/test-hooks.c',
  c_args: common_c_args,
  dependencies: [dl_dep, x11_dep],
)

xvfb_run = find_program('xvfb-run')
hook_test_env = environment()
hook_test_env.set('LD_PRELOAD', preload.full_path())
hook_test_env.set('XDG_SESSION_TYPE', 'x11')
hook_test_env.set('QQ_PRELOAD_DISABLE_PORTAL', '1')
test(
  'preload hooks',
  xvfb_run,
  args: [
    '-a',
    hook_test,
    fake_broadcast_core.full_path(),
    fake_wrapper.full_path(),
  ],
  env: hook_test_env,
  timeout: 30,
)
