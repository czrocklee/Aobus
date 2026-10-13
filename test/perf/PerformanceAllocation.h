// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>

namespace ao::rt::test
{
  struct AllocationSample final
  {
    std::size_t calls = 0;
    std::size_t requestedBytes = 0;
  };

  // One non-nested, process-wide window in the standalone perf executable.
  // Counts successful replaceable C++ new/new[] calls (including aligned and
  // nothrow paths), on all threads, at the recording hook before return.
  // Excludes malloc/realloc, native allocators, mappings, and allocations in
  // DSOs that do not bind these replacements. Not live/peak heap or RSS.
  class [[nodiscard]] AllocationSampleScope final
  {
  public:
    AllocationSampleScope();
    ~AllocationSampleScope();
    AllocationSampleScope(AllocationSampleScope const&) = delete;
    AllocationSampleScope& operator=(AllocationSampleScope const&) = delete;
    AllocationSampleScope(AllocationSampleScope&&) = delete;
    AllocationSampleScope& operator=(AllocationSampleScope&&) = delete;
    AllocationSample finish();

  private:
    bool _active = true;
  };
} // namespace ao::rt::test
