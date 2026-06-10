add_library(ao_fleet STATIC
    src/Engine.cpp
    src/Model.cpp
    src/ProcessRunner.cpp
    src/RouteStore.cpp
    src/Serialization.cpp
    src/Substrate.cpp
)

target_include_directories(ao_fleet PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(ao_fleet PUBLIC ao_async Boost::headers Boost::filesystem Boost::process PkgRapidYaml)
set_source_files_properties(src/Engine.cpp src/ProcessRunner.cpp PROPERTIES COMPILE_OPTIONS "-Wno-mismatched-new-delete")

add_executable(aobus-fleet src/Main.cpp)
target_link_libraries(aobus-fleet PRIVATE ao_fleet CLI11::CLI11)

add_executable(ao_fleet_test test/FleetTest.cpp)
target_link_libraries(ao_fleet_test PRIVATE ao_fleet Catch2::Catch2WithMain)
target_compile_definitions(ao_fleet_test PRIVATE AOBUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ao_fleet_test COMMAND ao_fleet_test)
