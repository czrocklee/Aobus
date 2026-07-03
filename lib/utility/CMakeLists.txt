# ao_utility
add_library(ao_utility STATIC
    AtomicFile.cpp
    Base64.cpp
    MappedFile.cpp
    Uuid.cpp
)
target_include_directories(ao_utility PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_utility PUBLIC PkgRapidYaml Boost::headers)
