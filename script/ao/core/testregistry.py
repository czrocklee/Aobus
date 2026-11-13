"""Shared test source conventions for the lexical registration audit.

The audit scans every candidate C++/Objective-C++ file under test/ by content,
including the headers and include fragments the source tools already govern.
Executable ownership comes from the Bazel graph (bazel_test_ownership), never
from a text scan of build files.
"""

from .gitfiles import CPP_INCLUDE_FRAGMENT_SUFFIXES, CPP_SUFFIXES

# Content-discovery universe for the lexical registration audit, reused from
# the governed source-tool suffixes so discovery and hygiene scope parity
# holds without introducing a new extension policy.
TEST_DISCOVERY_SUFFIXES = CPP_SUFFIXES
# Candidate files that can never host a registration; a detected registration
# in one of them fails placement instead of being audited.
TEST_PLACEMENT_SUFFIXES = CPP_INCLUDE_FRAGMENT_SUFFIXES
