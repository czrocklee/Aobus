// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/StringArena.h>

#include <cstring>
#include <string_view>

namespace ao::utility
{
  std::string_view StringArena::intern(std::string_view str)
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

  void StringArena::clear()
  {
    _index.clear();
    _resource.release();
  }

  void* StringArena::CountingMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment)
  {
    auto* const result = _resource->allocate(bytes, alignment);
    _allocatedBytes += bytes;
    return result;
  }

  void StringArena::CountingMemoryResource::do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment)
  {
    _resource->deallocate(ptr, bytes, alignment);
    _allocatedBytes -= bytes;
  }

  bool StringArena::CountingMemoryResource::do_is_equal(std::pmr::memory_resource const& other) const noexcept
  {
    return this == &other;
  }
} // namespace ao::utility
