// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstddef>
#include <memory_resource>
#include <string_view>

namespace ao::rt::detail
{
  /**
   * Interning arena for immutable projection strings.
   *
   * Stores each unique string once in monotonic (bump-allocated) blocks and deduplicates by
   * content. intern() returns a string_view that stays valid until clear() or destruction.
   *
   * Bump allocation replaces a per-string heap allocation with amortized block allocation.
   * The flat dedup index stores views into arena bytes, so rehashing never invalidates the
   * views held by a TrackListProjection.
   */
  class StringArena final
  {
  public:
    StringArena() = default;

    StringArena(StringArena const&) = delete;
    StringArena& operator=(StringArena const&) = delete;
    StringArena(StringArena&&) = delete;
    StringArena& operator=(StringArena&&) = delete;
    ~StringArena() = default;

    /** Stable view of the stored copy; equal inputs return the same view. */
    std::string_view intern(std::string_view str);

    /** Drop all interned strings and invalidate every previously returned view. */
    void clear();

    std::size_t size() const noexcept { return _index.size(); }
    std::size_t allocatedBytes() const noexcept { return _upstream.allocatedBytes(); }
    bool empty() const noexcept { return _index.empty(); }

  private:
    class CountingMemoryResource final : public std::pmr::memory_resource
    {
    public:
      std::size_t allocatedBytes() const noexcept { return _allocatedBytes; }

    private:
      void* do_allocate(std::size_t bytes, std::size_t alignment) override;
      void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override;
      bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override;

      std::pmr::memory_resource* _resource = std::pmr::get_default_resource();
      std::size_t _allocatedBytes = 0;
    };

    // Declared before the arena and index so it outlives both. The index only holds views
    // into arena-owned blocks, while the counting upstream tracks those blocks for rebase
    // decisions without approximating from string lengths.
    CountingMemoryResource _upstream;
    std::pmr::monotonic_buffer_resource _resource{&_upstream};
    boost::unordered_flat_set<std::string_view> _index;
  };
} // namespace ao::rt::detail
