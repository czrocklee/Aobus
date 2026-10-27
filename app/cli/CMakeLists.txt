add_library(aobus-cli-lib STATIC
    BooleanFlag.h
    CliRuntime.cpp
    CliRuntime.h
    CommandError.cpp
    CommandError.h
    CommandRegistrations.h
    DryRunFlag.h
    DumpOutput.cpp
    DumpOutput.h
    InitCommand.cpp
    LibCommand.cpp
    ListCommand.cpp
    ListCommand.h
    Output.cpp
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
    TrackCommand.cpp
    TrackSelection.cpp
    TrackSelection.h
)
target_include_directories(aobus-cli-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aobus-cli-lib PUBLIC ao_app_runtime ao CLI11::CLI11 Boost::headers)

# GCC 16.2 gcov misdiagnoses move_only_function's no-op empty-manager branch
# as a read of its uninitialized swap temporary. Keep this exception source-local.
# The portal `ao coverage` supplies --coverage through CMAKE_CXX_FLAGS.
if(LINUX
    AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL 16.2.0
    AND CMAKE_CXX_FLAGS MATCHES "(^|[ ;])--coverage($|[ ;])")
  set_property(SOURCE ScanCommand.cpp
    APPEND PROPERTY COMPILE_OPTIONS -Wno-maybe-uninitialized)
endif()

add_executable(aobus
    main.cpp
)
aobus_enable_windows_utf8_manifest(aobus)
target_link_libraries(aobus PRIVATE aobus-cli-lib mimalloc)
