# app/common: Shared application logic
add_library(app_common STATIC
    common/AppConfig.cpp
    common/AppConfig.h
)
target_include_directories(app_common PUBLIC "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(app_common PRIVATE ao PkgGLIBMM)
aobus_apply_clang_tidy(app_common STRICT)

# Frontends
add_subdirectory(linux-gtk)
add_subdirectory(cli)

# Placeholders for future platforms
# add_subdirectory(windows-winui)
# add_subdirectory(tui)