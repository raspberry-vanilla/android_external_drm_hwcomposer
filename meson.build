project(
    'drm_hwcomposer',
    ['c', 'cpp'],
    version : '3',
    license : 'APACHE-2.0',
    meson_version : '>= 0.56',
    default_options : ['buildtype=debugoptimized', 'b_ndebug=if-release', 'c_std=c11', 'cpp_std=c++17', 'cpp_rtti=false']
)

inc_include = [include_directories('.')]

src_common = files(
    'backend/Backend.cpp',
    'backend/BackendManager.cpp',
    'backend/ClientBackend.cpp',
    'backend/GenericBackend.cpp',
    'backend/IntelBackend.cpp',
    'compositor/CompositionPlanner.cpp',
    'compositor/FlatteningController.cpp',
    'compositor/FlatteningEventAtomReporter.cpp',
    'compositor/FrameTimeHistory.cpp',
    'compositor/GenericCompositionPlanner.cpp',
    'compositor/GenericLayerMapperCompositionPlanner.cpp',
    'compositor/HdcpController.cpp',
    'compositor/IntelMappingValidator.cpp',
    'compositor/LayerToPlaneJoiningPlan.cpp',
    'compositor/PresentedCompositionCache.cpp',
    'compositor/ShortCircuitor.cpp',
    'compositor/mapper/CursorLayerMapper.cpp',
    'compositor/mapper/ForceClientCompositionLayerMapper.cpp',
    'compositor/mapper/LayerCachingMapper.cpp',
    'compositor/mapper/LeftoverLayerMapper.cpp',
    'compositor/mapper/MapperUtils.cpp',
    'compositor/mapper/UnderlayMapper.cpp',
    'hwc/HwcDisplayConfigs.cpp',
    'hwc/HwcDisplay.cpp',
    'hwc/HwcLayer.cpp',
    'stats/Stats.cpp',
    'stats/CompositionStatsAtomReporter.cpp',
    'stats/CountActiveDisplaysReporter.cpp',
    'stats/StatsPoller.cpp',
    'stats/DisplayConfigurationResultReporter.cpp',
    'stats/DisplayHotplugConnectModeDetectedAtomReporter.cpp',
    'stats/DisplayRefreshRatesChangedAtomReporter.cpp',
    'utils/BacklightController.cpp',
    'utils/ColorUtil.cpp',
    'utils/fd.cpp',
    'utils/LibdisplayEdidWrapper.cpp',
    'utils/SysfsBacklightController.cpp',
    'utils/properties.cpp',
    'early_animation/EarlyBootAnimation.cpp',
)

deps = [
    dependency('base'),
    dependency('cutils'),
    dependency('drm'),
    dependency('hardware'),
    dependency('hidlbase'),
    dependency('log'),
    dependency('sync'),
    dependency('ui'),
    dependency('utils'),
    dependency('aidlcommonsupport'),
    dependency('android.hardware.graphics.composer@2.1-resources'),
    dependency('android.hardware.graphics.composer@2.2-resources'),
]

android_sdk_version = get_option('android_sdk_version')
message('ANDROID_SDK_VERSION = ' + android_sdk_version)

cpp_compiler = meson.get_compiler('cpp')

common_cpp_flags = [
    '-DUSE_IMAPPER4_METADATA_API',
]

if cpp_compiler.get_define('__ANDROID_API__') == ''
  common_cpp_flags += '-D__ANDROID_API__=' + android_sdk_version
endif

if cpp_compiler.has_argument('-Wno-error=invalid-specialization')
  common_cpp_flags += '-Wno-error=invalid-specialization'
endif

dep_libdisplay_info = dependency('display_info', required : false)
if dep_libdisplay_info.found()
  common_cpp_flags += '-DHAS_LIBDISPLAY_INFO'
  deps += dep_libdisplay_info
endif

dep_liblz4 = dependency('liblz4', required : false)
if not dep_liblz4.found()
  dep_liblz4 = dependency('lz4', required : false)
endif
if dep_liblz4.found()
  common_cpp_flags += '-DHAS_LZ4'
  deps += dep_liblz4
endif

hwc2_cpp_flags = [
    '-DHWC2_INCLUDE_STRINGIFICATION',
    '-DHWC2_USE_CPP11',
]

subdir('drm')
subdir('bufferinfo')
subdir('hwc')

drmhwc_common = static_library(
    'drm_hwcomposer_common',
    src_common,
# TODO remove hwc2 flags from common code (backends needs rework)
    cpp_args : common_cpp_flags + hwc2_cpp_flags,
    dependencies : deps,
)

subdir('hwc3')
