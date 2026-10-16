// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace ao::audio::backend::detail
{
  // Orders control-side run transitions against already consumed worker actions.
  // Tokens do not retain the owner: every Scope must end before this object dies.
  // Realtime and native listener paths may only use current() and isCurrent().
  class CoreAudioRunControl final
  {
  public:
    class [[nodiscard]] Scope final
    {
    public:
      Scope(Scope const&) = delete;
      Scope& operator=(Scope const&) = delete;
      Scope(Scope&&) noexcept = default;
      Scope& operator=(Scope&&) noexcept = default;
      ~Scope() = default;

      std::uint64_t generation() const noexcept;

    private:
      friend class CoreAudioRunControl;
      Scope(std::unique_lock<std::mutex> lock, std::uint64_t generation) noexcept;

      std::unique_lock<std::mutex> _lock;
      std::uint64_t _generation;
    };

    std::uint64_t current() const noexcept;
    bool isCurrent(std::uint64_t generation) const noexcept;

    // Control threads only. Never invoke outward callbacks while a Scope lives.
    Scope lock();
    Scope advance();
    std::optional<Scope> tryCurrent(std::uint64_t generation);

  private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::mutex _mutex{};
    std::atomic<std::uint64_t> _generation{1U};
  };
} // namespace ao::audio::backend::detail
