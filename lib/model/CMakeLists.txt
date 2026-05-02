# ao_model
add_library(ao_model STATIC
    TrackIdList.cpp
    AllTrackIdsList.cpp
    ManualTrackIdList.cpp
    SmartListEngine.cpp
    FilteredTrackIdList.cpp
)
target_include_directories(ao_model PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_model PUBLIC ao_library ao_query)
aobus_apply_clang_tidy(ao_model STRICT)
