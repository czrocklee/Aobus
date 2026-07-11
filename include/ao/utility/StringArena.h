// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <string_view>

namespace ao::utility
{
  /**
   * StringArena - Interning arena for immutable strings.
   *
   * Stores each unique string once in monotonic (bump-allocated) blocks and deduplicates by
   * content. intern() returns a string_view that stays valid until clear() or destruction.
   *
   * Why this shape:
   * - Bump allocation replaces a per-string heap allocation with amortized block allocation,
   *   so interning N unique strings costs a handful of block allocations rather than N.
   * - The dedup index is a flat (open-addressing) set, so unique strings cost no per-node
   *   allocation. It stores views into the arena, not the characters, so rehashing only
   *   relocates 16-byte views; the arena bytes never move and outstanding views stay valid.
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

    /**
     * Intern a string by content.
     *
     * @param str The string to intern (copied into the arena on first sighting)
     * @return Stable view of the stored copy; equal inputs return the same view. Empty input
     *         returns an empty view without storing anything.
     */
    std::string_view intern(std::string_view str)
    {
      if (str.empty())
      {
        return {};
      }

      if (auto const it = _index.find(str); it != _index.end())
      {
        return *it;
      }

      auto* const mem = static_cast<char*>(_resource.allocate(str.size(), alignof(char)));
      std::memcpy(mem, str.data(), str.size());

      auto const view = std::string_view{mem, str.size()};
      _index.insert(view);
      return view;
    }

    /** Drop all interned strings. Invalidates every previously returned view. */
    void clear()
    {
      _index.clear();
      _resource.release();
    }

    std::size_t size() const noexcept { return _index.size(); }
    std::size_t allocatedBytes() const noexcept { return _upstream.allocatedBytes(); }
    bool empty() const noexcept { return _index.empty(); }

  private:
    class CountingMemoryResource final : public std::pmr::memory_resource
    {
    public:
      std::size_t allocatedBytes() const noexcept { return _allocatedBytes; }

    private:
      void* do_allocate(std::size_t bytes, std::size_t alignment) override
      {
        auto* const result = _resource->allocate(bytes, alignment);
        _allocatedBytes += bytes;
        return result;
      }

      void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override
      {
        _resource->deallocate(ptr, bytes, alignment);
        _allocatedBytes -= bytes;
      }

      bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override { return this == &other; }

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
} // namespace ao::utility
