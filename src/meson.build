livecaptions_sources = [
  'main.c',
  'livecaptions-window.c',
  'livecaptions-welcome.c',
  'livecaptions-settings.c',
  'livecaptions-application.c',
  'audiocap.c',
  'audiocap-pa.c',
  'audiocap-pw.c',
  'asrproc.c',
  'line-gen.c',
  'profanity-filter.c',
  'window-helper.c',
  'history.c',
  'livecaptions-history-window.c',
  'dbus-interface.c',
  'spectrum.c'
]

cc = meson.get_compiler('c')

livecaptions_deps = [
  dependency('libadwaita-1', version: '>= 1.0'),
  #dependency('libpipewire-0.3', version: '>=0.3.41'),
  dependency('libpulse'),
  dependency('x11'),

  cc.find_library('m', required: false),
  april_lib
]

sherpa_onnx_dir = get_option('sherpa_onnx_dir')
if sherpa_onnx_dir != ''
  livecaptions_deps += declare_dependency(
    dependencies: cc.find_library('sherpa-onnx-c-api', dirs: [sherpa_onnx_dir / 'lib']),
    compile_args: ['-DLIVE_CAPTIONS_SHERPA', '-I' + (sherpa_onnx_dir / 'include')],
  )
endif

gnome = import('gnome')

livecaptions_sources += gnome.compile_resources('livecaptions-resources',
  'livecaptions.gresource.xml',
  c_name: 'livecaptions'
)

executable('livecaptions', livecaptions_sources,
  dependencies: livecaptions_deps,
  install: true,
)
