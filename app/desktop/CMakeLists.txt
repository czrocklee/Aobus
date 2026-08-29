# ao_desktop_launch: shared pure desktop lifecycle rules and process mechanism.
# Toolkit registration, windows, dispatchers, stores, and diagnostics stay in
# the GTK and WinUI composition roots.
add_library(ao_desktop_launch STATIC
    DetachedProcessLauncher.cpp
    LibraryPath.cpp
    LibraryStartupPlanner.cpp
    LibrarySuccessorProtocol.cpp
    LibrarySwitch.cpp
)
target_include_directories(ao_desktop_launch PUBLIC "${CMAKE_SOURCE_DIR}/app/include")
target_link_libraries(ao_desktop_launch PUBLIC ao_utility)
target_link_libraries(ao_desktop_launch PRIVATE Boost::process)
