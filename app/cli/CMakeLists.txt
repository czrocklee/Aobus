add_executable(aobus
    main.cpp
    DumpUtils.h
    InitCommand.cpp
    LibCommand.cpp
    ListCommand.cpp
    TagCommand.cpp
    TrackCommand.cpp
)
target_link_libraries(aobus PRIVATE ao_app_runtime ao CLI11::CLI11 mimalloc)
