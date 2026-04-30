# rs_utility
add_library(rs_utility STATIC
    MappedFile.cpp
    Log.cpp
)
target_include_directories(rs_utility PUBLIC ${RS_PUBLIC_INCLUDE_DIR})
target_link_libraries(rs_utility PUBLIC spdlog::spdlog)
rockstudio_apply_clang_tidy(rs_utility STRICT)
