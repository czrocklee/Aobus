# ao_utility
add_library(ao_utility STATIC
    MappedFile.cpp
    Log.cpp
)
target_include_directories(ao_utility PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_utility PUBLIC spdlog::spdlog)
aobus_apply_clang_tidy(ao_utility STRICT)
