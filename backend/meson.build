src_common += files(
    'Backend.cpp',
    'BackendManager.cpp',
    'ClientBackend.cpp',
    'GenericBackend.cpp',
    'IntelBackend.cpp',
    'SdmBackend.cpp',
    'sdm/SdmAtomicStateManager.cpp',
    'sdm/SdmCompositionPlanner.cpp',
    'sdm/SdmDebugCallback.cpp',
    'sdm/SdmHotplugHandler.cpp',
    'sdm/SdmToConnectorMapper.cpp',
    'sdm/SnapAllocHandle.cpp',
    'sdm/sdm_error.cpp',
    'sdm/stubs/sdm_stubs.cpp',
)

inc_include += [
    include_directories('sdm'),
    include_directories('sdm/stubs/include', is_system : true),
]
