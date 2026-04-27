add_executable(rsc
    main.cpp
    InitCommand.cpp
    LibCommand.cpp
    ListCommand.cpp
    TagCommand.cpp
    TrackCommand.cpp
)
target_link_libraries(rsc PRIVATE rs CLI11::CLI11)

if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rsc PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};--config=${CLANG_TIDY_STRICT_CONFIG};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()
