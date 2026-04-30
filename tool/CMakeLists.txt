add_executable(rsc
    main.cpp
    InitCommand.cpp
    LibCommand.cpp
    ListCommand.cpp
    TagCommand.cpp
    TrackCommand.cpp
)
target_link_libraries(rsc PRIVATE rs CLI11::CLI11)
rockstudio_apply_clang_tidy(rsc STRICT)
