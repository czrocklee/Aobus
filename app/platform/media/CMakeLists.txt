add_library(ao_system_media_linux STATIC
    linux/MprisArtUrlCache.cpp
    linux/MprisArtUrlSession.cpp
    linux/MprisBridge.cpp
    linux/MprisBusSession.cpp
    linux/MprisPlaybackEndpoint.cpp
)

target_include_directories(ao_system_media_linux PUBLIC "${CMAKE_SOURCE_DIR}/app/platform")
target_link_libraries(ao_system_media_linux
    PUBLIC
        ao_app_runtime
        ao_app_uimodel
        PkgGIOMM
        PkgGLIBMM
    PRIVATE
        ao
)
