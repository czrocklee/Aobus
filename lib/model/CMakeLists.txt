# rs_model
add_library(rs_model STATIC
    AllTrackIdsList.cpp
    FilteredTrackIdList.cpp
    ManualTrackIdList.cpp
    SmartListEngine.cpp
    TrackIdList.cpp
)
target_include_directories(rs_model PUBLIC ${RS_PUBLIC_INCLUDE_DIR})
target_link_libraries(rs_model PUBLIC rs_library rs_expr)
rockstudio_apply_clang_tidy(rs_model STRICT)
