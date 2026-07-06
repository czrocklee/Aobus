# ao_utility
add_library(ao_utility STATIC
    AtomicFile.cpp
    Base64.cpp
    MappedFile.cpp
    Uuid.cpp
    Xxh3.cpp
)
target_include_directories(ao_utility PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_utility PUBLIC PkgRapidYaml Boost::headers)
# xxhash is an implementation detail of Xxh3.cpp; keep it out of the public
# link interface so ao/utility/Xxh3.h consumers never see <xxhash.h>.
target_link_libraries(ao_utility PRIVATE PkgXXHash)
