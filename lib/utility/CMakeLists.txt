# ao_utility
add_library(ao_utility STATIC
    Base64.cpp
    MappedFile.cpp
    ThreadName.cpp
    Uuid.cpp
    Xxh3.cpp
)
if(WIN32)
    target_sources(ao_utility PRIVATE AtomicFileWindows.cpp)
else()
    target_sources(ao_utility PRIVATE AtomicFile.cpp)
endif()
target_include_directories(ao_utility PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_utility PUBLIC PkgRapidYaml Boost::headers gsl-lite::gsl-lite)
# xxhash is an implementation detail of Xxh3.cpp; keep it out of the public
# link interface so ao/utility/Xxh3.h consumers never see <xxhash.h>.
target_link_libraries(ao_utility PRIVATE PkgXXHash)
