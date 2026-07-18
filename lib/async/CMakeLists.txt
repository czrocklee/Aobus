# ao_async: coroutine runtime primitives shared by app services and tools.
add_library(ao_async STATIC
    LoopExecutor.cpp
    OperationCancelled.cpp
    Parallel.cpp
    QueuedExecutorBase.cpp
    Runtime.cpp
    LifetimeScope.cpp
)

target_include_directories(ao_async PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_async PUBLIC ao_utility Boost::headers)

# GCC 15 with ThreadSanitizer misclassifies Boost.Asio's correctly paired
# coroutine-frame aligned_alloc/free path after inlining. Propagate the
# workaround from the async boundary because the diagnostic is emitted while
# compiling coroutine consumers, not ao_async itself.
if(AOBUS_ENABLE_TSAN
    AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  target_compile_options(ao_async PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-mismatched-new-delete>)
endif()
