add_library(aobus-cli-lib STATIC
    CliRuntime.cpp
    CliRuntime.h
    CommandError.h
    DryRunFlag.h
    DumpOutput.h
    InitCommand.cpp
    InitCommand.h
    LibCommand.cpp
    LibCommand.h
    ListCommand.cpp
    ListCommand.h
    Output.h
    QueryHelp.cpp
    QueryHelp.h
    Run.cpp
    Run.h
    ScanCommand.cpp
    ScanCommand.h
    ScanOutput.cpp
    ScanOutput.h
    TagCommand.cpp
    TagCommand.h
    TrackCommand.cpp
    TrackCommand.h
    TrackSelection.cpp
    TrackSelection.h
)
target_include_directories(aobus-cli-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aobus-cli-lib PUBLIC ao_app_runtime ao CLI11::CLI11 Boost::headers)
add_dependencies(aobus-cli-lib ao_frontend_library_path_boundary_check)

add_executable(aobus
    main.cpp
)
target_link_libraries(aobus PRIVATE aobus-cli-lib mimalloc)
