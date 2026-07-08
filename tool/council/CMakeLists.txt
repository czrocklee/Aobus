add_library(ao_council STATIC
    Engine.cpp
    CouncilSchema.cpp
    ProcessRunner.cpp
    Serialization.cpp
    Substrate.cpp
)

target_include_directories(ao_council PUBLIC
    "${CMAKE_SOURCE_DIR}/tool"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(ao_council PUBLIC ao_async ao_utility Boost::headers Boost::filesystem Boost::process PkgRapidYaml)
set_source_files_properties(Engine.cpp ProcessRunner.cpp PROPERTIES COMPILE_OPTIONS "-Wno-mismatched-new-delete")

add_executable(aobus-council Main.cpp)
target_link_libraries(aobus-council PRIVATE ao_council CLI11::CLI11)
