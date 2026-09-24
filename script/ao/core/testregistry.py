"""Shared test source suffixes; CMake owns executable-membership validation.

See cmake/AobusTestSources.cmake. A source path appearing in CMake text is not
registration evidence, so tooling must not infer ownership with a text scan.
"""

TEST_SOURCE_SUFFIXES = ("Test.cpp", "Test.mm")
