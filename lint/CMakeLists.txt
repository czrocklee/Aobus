cmake_minimum_required(VERSION 3.20)
project(AobusClangTidyPlugin)

find_package(LLVM REQUIRED CONFIG)
list(APPEND CMAKE_MODULE_PATH ${LLVM_DIR})
include(AddLLVM)

find_package(Clang REQUIRED CONFIG)

add_definitions(${LLVM_DEFINITIONS})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

add_library(AobusLintPlugin MODULE
  check/CApiGlobalQualificationCheck.cpp
  check/ConcreteFinalCheck.cpp
  check/ControlBlockSpacingCheck.cpp
  check/IncludeConventionCheck.cpp
  check/ForbidNodiscardCheck.cpp
  check/ForbidTrailingReturnCheck.cpp
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
  check/ManagedPointerSuffixCheck.cpp
  check/MemberOrderCheck.cpp
  check/UseEraseIfCheck.cpp
  check/UseStdNumbersCheck.cpp
  check/UseRangesContainsCheck.cpp
  check/UseRangesProjectionCheck.cpp
  check/UseRangesAnyOfCheck.cpp
  check/UseRangesMinMaxCheck.cpp
  check/UseStartsWithCheck.cpp
  check/UseCtadCheck.cpp
  AobusLintModule.cpp
)

set_target_properties(AobusLintPlugin PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(AobusLintPlugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(AobusLintPlugin PRIVATE -Wno-null-dereference)

target_link_libraries(AobusLintPlugin PRIVATE
  clangTidy
  clangTidyUtils
  clangTidyReadabilityModule
  clangAST
  clangASTMatchers
  clangBasic
  clangLex
  LLVMSupport
)