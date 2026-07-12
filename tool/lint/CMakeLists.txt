cmake_minimum_required(VERSION 3.20)
project(AobusClangTidyPlugin)

if(WIN32)
  include(${CMAKE_SOURCE_DIR}/cmake/LlvmSdk.cmake)
  aobus_configure_llvm_sdk()
  find_package(LLVM ${AOBUS_LLVM_SDK_VERSION} EXACT REQUIRED CONFIG
    PATHS "${LLVM_DIR}" NO_DEFAULT_PATH)
else()
  find_package(LLVM 22.1 REQUIRED CONFIG)
endif()
list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)

if(WIN32)
  find_package(Clang ${AOBUS_LLVM_SDK_VERSION} EXACT REQUIRED CONFIG
    PATHS "${Clang_DIR}" NO_DEFAULT_PATH)
else()
  find_package(Clang 22.1 REQUIRED CONFIG)
endif()

if(WIN32)
  # LLVM's official Windows package leaks its release-builder DIA SDK path.
  # Replace only that non-relocatable library with the active VS environment.
  find_library(AOBUS_DIA_GUIDS_LIBRARY NAMES diaguids
    HINTS "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64"
    REQUIRED)
  get_target_property(AOBUS_PDB_LINK_LIBRARIES LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
  list(FILTER AOBUS_PDB_LINK_LIBRARIES EXCLUDE REGEX
    "[/\\\\]DIA SDK[/\\\\].*[/\\\\]diaguids\\.lib$")
  list(PREPEND AOBUS_PDB_LINK_LIBRARIES "${AOBUS_DIA_GUIDS_LIBRARY}")
  set_target_properties(LLVMDebugInfoPDB PROPERTIES
    INTERFACE_LINK_LIBRARIES "${AOBUS_PDB_LINK_LIBRARIES}")
endif()

add_definitions(${LLVM_DEFINITIONS})

set(AOBUS_LINT_PLUGIN_SOURCES
  check/AsyncCancellationGuardCheck.cpp
  check/CApiGlobalQualificationCheck.cpp
  check/ConcreteFinalCheck.cpp
  check/ControlBlockSpacingCheck.cpp
  check/IncludeConventionCheck.cpp
  check/NodiscardUsageCheck.cpp
  check/ForbidTrailingReturnCheck.cpp
  check/ForbidRawThrowCheck.cpp
  check/IdentifierNamingExtensionsCheck.cpp
  check/ImplicitBoolConversionInInitCheck.cpp
  check/LocalInitializationStyleCheck.cpp
  check/BracedInitializationCheck.cpp
  check/OptionalNamingAndUsageCheck.cpp
  check/RedundantNamespaceQualificationCheck.cpp
  check/RedundantUsingDirectiveCheck.cpp
  check/SpdxLicenseHeaderCheck.cpp
  check/StdCLibraryQualificationCheck.cpp
  check/ThreadingPolicyCheck.cpp
  check/UnusedSuppressionStyleCheck.cpp
  check/UseIfInitStatementCheck.cpp
  check/LambdaParamsCheck.cpp
  check/PointerNamingConventionCheck.cpp
  check/ChronoNamingConventionCheck.cpp
  check/MemberOrderCheck.cpp
  check/UseEraseIfCheck.cpp
  check/UseStdNumbersCheck.cpp
  check/StrictForwardDeclarationCheck.cpp
  check/UseRangesContainsCheck.cpp
  check/UseRangesProjectionCheck.cpp
  check/UseRangesAnyOfCheck.cpp
  check/UseRangesMinMaxCheck.cpp
  check/UseStartsWithCheck.cpp
  check/UseCtadCheck.cpp
  check/UseStdToArrayCheck.cpp
  AobusLintModule.cpp
)

set(AOBUS_LINT_PLUGIN_LINK_LIBS
  clangTidy
  clangTidyUtils
  clangTidyReadabilityModule
  clangAST
  clangASTMatchers
  clangBasic
  clangLex
  LLVMSupport
)

if(WIN32)
  # Official Windows clang-tidy binaries do not export the C++ symbols needed
  # by an out-of-tree DLL. Link the checks and clangTidyMain into one executable
  # instead, so every module registers in the same in-process registry.
  # LLVM disables exceptions for this executable; remove CMake's default
  # enable flag before llvm_update_compile_flags adds the matching MSVC flag.
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_executable(AobusClangTidy
    AobusClangTidyMain.cpp
    ${AOBUS_LINT_PLUGIN_SOURCES}
  )
  target_link_libraries(AobusClangTidy PRIVATE
    clangTidyMain
    ${AOBUS_LINT_PLUGIN_LINK_LIBS}
  )
  set_property(TARGET AobusClangTidy PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
  llvm_update_compile_flags(AobusClangTidy)
  # Template instantiations in LLVM's ADT/bit.h expose compile-time unreachable
  # branches as C4702 even though the SDK headers are marked external.
  target_compile_options(AobusClangTidy PRIVATE /wd4702)
  set(AOBUS_LINT_TARGET AobusClangTidy)
else()
  # Keep the established Linux linkage and libAobusLintPlugin.so path.
  add_library(AobusLintPlugin MODULE ${AOBUS_LINT_PLUGIN_SOURCES})
  target_link_libraries(AobusLintPlugin PRIVATE ${AOBUS_LINT_PLUGIN_LINK_LIBS})
  # This module is loaded by the externally provided clang-tidy executable.
  # Instrumenting it would require that unsanitized host to preload the matching
  # sanitizer runtime before loading the plugin.
  if(AOBUS_ENABLE_ASAN OR AOBUS_ENABLE_TSAN)
    target_compile_options(AobusLintPlugin PRIVATE -fno-sanitize=all)
    target_link_options(AobusLintPlugin PRIVATE -fno-sanitize=all)
  endif()
  set(AOBUS_LINT_TARGET AobusLintPlugin)
endif()

set_target_properties(${AOBUS_LINT_TARGET} PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(${AOBUS_LINT_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${AOBUS_LINT_TARGET} SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS}
)
target_compile_options(${AOBUS_LINT_TARGET} PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-null-dereference>
)
