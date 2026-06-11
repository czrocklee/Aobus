add_library(ao_fleet STATIC
    Engine.cpp
    Model.cpp
    ProcessRunner.cpp
    RouteStore.cpp
    Serialization.cpp
    Substrate.cpp
)

target_include_directories(ao_fleet PUBLIC
    "${CMAKE_SOURCE_DIR}/tool"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(ao_fleet PUBLIC ao_async Boost::headers Boost::filesystem Boost::process PkgRapidYaml)
set_source_files_properties(Engine.cpp ProcessRunner.cpp PROPERTIES COMPILE_OPTIONS "-Wno-mismatched-new-delete")

add_executable(aobus-fleet Main.cpp)
target_link_libraries(aobus-fleet PRIVATE ao_fleet CLI11::CLI11)
