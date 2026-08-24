# ao_utility
add_library(ao_utility STATIC
    AudioCodecText.cpp
    Base64.cpp
    ByteView.cpp
    Fatal.cpp
    FromChars.cpp
    Hash128.cpp
    MappedFile.cpp
    Path.cpp
    Reflect.cpp
    RymlAdapter.cpp
    ScopedRegistration.cpp
    Serialization.cpp
    Sha256.cpp
    String.cpp
    StringArena.cpp
    ThreadName.cpp
    Uuid.cpp
    Xxh3.cpp
)
if(WIN32)
    target_sources(ao_utility PRIVATE
        AtomicFileWindows.cpp
        FileAllocationWindows.cpp
        PlatformDirectoriesWindows.cpp)
    target_link_libraries(ao_utility PRIVATE Advapi32)
else()
    target_sources(ao_utility PRIVATE
        AtomicFilePosix.cpp
        FileAllocationPosix.cpp
        PlatformDirectoriesPosix.cpp)
endif()
target_include_directories(ao_utility PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_utility PUBLIC PkgRapidYaml Boost::headers gsl-lite::gsl-lite)
# xxhash is an implementation detail of Xxh3.cpp; keep it out of the public
# link interface so ao/utility/Xxh3.h consumers never see <xxhash.h>.
target_link_libraries(ao_utility PRIVATE FastFloat::fast_float PkgXXHash)

# Keep ICU behind the Unicode-text facade so its API and dependency contract do
# not leak into the general utility library. Current applications consume the
# facade through library/query/runtime text semantics.
add_library(ao_unicode_text STATIC UnicodeText.cpp)
target_include_directories(ao_unicode_text PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_unicode_text PRIVATE ICU::uc ICU::data)
