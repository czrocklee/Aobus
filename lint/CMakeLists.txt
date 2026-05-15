cmake_minimum_required(VERSION 3.20)
project(AobusClangTidyPlugin)

find_package(LLVM REQUIRED CONFIG)
list(APPEND CMAKE_MODULE_PATH ${LLVM_DIR})
include(AddLLVM)

find_package(Clang REQUIRED CONFIG)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

add_library(AobusLintPlugin MODULE
  check/CApiGlobalQualificationCheck.cpp
  check/ConcreteFinalCheck.cpp
  check/ControlBlockSpacingCheck.cpp
  check/ForbidNodiscardCheck.cpp
  check/ForbidTrailingReturnCheck.cpp
  check/IdentifierNamingExtensionsCheck.cpp
  check/LocalInitializationStyleCheck.cpp
  check/MemberInitializerBracesCheck.cpp
  check/OptionalNamingAndUsageCheck.cpp
  check/StdCLibraryQualificationCheck.cpp
  check/ThreadingPolicyCheck.cpp
  check/UnusedSuppressionStyleCheck.cpp
  check/LambdaParamsCheck.cpp
  AobusLintModule.cpp
)

set_target_properties(AobusLintPlugin PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(AobusLintPlugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(AobusLintPlugin PRIVATE
  clangTidy
  clangTidyReadabilityModule
  clangAST
  clangASTMatchers
  clangBasic
  clangLex
  LLVMSupport
)