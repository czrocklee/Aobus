add_executable(aobus
    main.cpp
    InitCommand.cpp
    LibCommand.cpp
    ListCommand.cpp
    TagCommand.cpp
    TrackCommand.cpp
)
target_link_libraries(aobus PRIVATE app_runtime ao CLI11::CLI11 mimalloc)
aobus_apply_clang_tidy(aobus STRICT)
