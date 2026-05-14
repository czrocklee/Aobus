cmake_minimum_required(VERSION 3.20)
project(AobusClangTidyPlugin)

find_package(LLVM REQUIRED CONFIG)
list(APPEND CMAKE_MODULE_PATH ${LLVM_DIR})
include(AddLLVM)

find_package(Clang REQUIRED CONFIG)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

add_library(AobusLintPlugin MODULE
  ControlBlockSpacingCheck.cpp
  AobusLintModule.cpp
)

set_target_properties(AobusLintPlugin PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

target_link_libraries(AobusLintPlugin PRIVATE
  clangTidy
  clangTidyReadabilityModule
  clangAST
  clangASTMatchers
  clangBasic
  clangLex
  LLVMSupport
)