# rs_model
add_library(rs_model STATIC
    AllTrackIdsList.cpp
    FilteredTrackIdList.cpp
    ManualTrackIdList.cpp
    SmartListEngine.cpp
    TrackIdList.cpp
)
target_include_directories(rs_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(rs_model PUBLIC rs_library rs_expr)

if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rs_model PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};--config=${CLANG_TIDY_STRICT_CONFIG};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()
