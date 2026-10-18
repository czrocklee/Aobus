// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioRunControl.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace ao::audio::backend::detail
{
  CoreAudioRunControl::Scope::Scope(std::unique_lock<std::mutex> lock, std::uint64_t const generation) noexcept
    : _lock{std::move(lock)}, _generation{generation}
  {
  }

  std::uint64_t CoreAudioRunControl::Scope::generation() const noexcept
  {
    return _generation;
  }

  std::uint64_t CoreAudioRunControl::current() const noexcept
  {
    return _generation.load(std::memory_order_acquire);
  }

  bool CoreAudioRunControl::isCurrent(std::uint64_t const generation) const noexcept
  {
    return current() == generation;
  }

  CoreAudioRunControl::Scope CoreAudioRunControl::lock()
  {
    auto lock = std::unique_lock{_mutex};
    auto const generation = current();
    return Scope{std::move(lock), generation};
  }

  CoreAudioRunControl::Scope CoreAudioRunControl::advance()
  {
    auto lock = std::unique_lock{_mutex};
    auto const generation = _generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    return Scope{std::move(lock), generation};
  }

  std::optional<CoreAudioRunControl::Scope> CoreAudioRunControl::tryCurrent(std::uint64_t const generation)
  {
    auto lock = std::unique_lock{_mutex};

    if (!isCurrent(generation))
    {
      return std::nullopt;
    }

    return Scope{std::move(lock), generation};
  }
} // namespace ao::audio::backend::detail
