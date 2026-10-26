"""Shared test source conventions; CMake owns executable-membership validation.

See cmake/AobusTestSources.cmake. A source path appearing in CMake text is not
registration evidence, so tooling must not infer ownership with a text scan.

The registration filename convention and the audit's content-discovery
universe are separate contracts. CMake finalization expects exactly the
declared *Test.cpp/mm and *Benchmark.cpp sources, while the lexical audit
scans every candidate C++/Objective-C++ file under test/ by content,
including the headers and include fragments the source tools already govern.
"""

from .gitfiles import CPP_INCLUDE_FRAGMENT_SUFFIXES, CPP_SUFFIXES

# Content-discovery universe for the lexical registration audit, reused from
# the governed source-tool suffixes so discovery and hygiene scope parity
# holds without introducing a new extension policy.
TEST_DISCOVERY_SUFFIXES = CPP_SUFFIXES
# Candidate files that can never host a registration; a detected registration
# in one of them fails placement instead of being audited.
TEST_PLACEMENT_SUFFIXES = CPP_INCLUDE_FRAGMENT_SUFFIXES
