// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>

#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <cstddef>
#include <type_traits>

namespace ao::audio::detail
{
  template<typename Signal>
  class EngineRtSignalRing final
  {
  public:
    static constexpr std::size_t kCapacity = 2;

    EngineRtSignalRing() = default;

    EngineRtSignalRing(EngineRtSignalRing const&) = delete;
    EngineRtSignalRing& operator=(EngineRtSignalRing const&) = delete;
    EngineRtSignalRing(EngineRtSignalRing&&) = delete;
    EngineRtSignalRing& operator=(EngineRtSignalRing&&) = delete;
    ~EngineRtSignalRing() = default;

    void push(Signal const& signal) noexcept
    {
      auto const pushed = _ring.push(signal);
      AO_RT_INVARIANT(pushed, "RT signal ring capacity exceeded");
    }

    bool pop(Signal& signal) noexcept { return _ring.pop(signal); }

    std::size_t readAvailable() const noexcept { return _ring.read_available(); }

  private:
    static_assert(std::is_trivially_copyable_v<Signal>);

    boost::lockfree::spsc_queue<Signal, boost::lockfree::capacity<kCapacity>> _ring;
  };
} // namespace ao::audio::detail
