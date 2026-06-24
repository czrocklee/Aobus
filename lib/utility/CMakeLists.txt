# ao_utility
add_library(ao_utility STATIC
    AtomicFile.cpp
    Base64.cpp
    MappedFile.cpp
)
target_include_directories(ao_utility PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
