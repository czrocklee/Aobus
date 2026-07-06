# ao_async: coroutine runtime primitives shared by app services and tools.
add_library(ao_async STATIC
    OperationCancelled.cpp
    Parallel.cpp
    QueuedExecutor.cpp
    Runtime.cpp
    LifetimeScope.cpp
)

target_include_directories(ao_async PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_async PUBLIC Boost::headers)
