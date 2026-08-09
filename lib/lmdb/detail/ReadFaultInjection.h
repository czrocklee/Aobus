// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <thread>

namespace ao::lmdb::detail
{
  /**
   * Owns one native read-result injection on the current thread.
   *
   * This source-private test seam is consumed by the next LMDB read adapter
   * call. It exists only to prove the phase-specific open/live/write failure
   * channels without corrupting a mapped environment behind LMDB's back.
   */
  class ReadFaultInjection final
  {
  public:
    explicit ReadFaultInjection(std::int32_t code);
    ~ReadFaultInjection();

    ReadFaultInjection(ReadFaultInjection const&) = delete;
    ReadFaultInjection& operator=(ReadFaultInjection const&) = delete;
    ReadFaultInjection(ReadFaultInjection&&) = delete;
    ReadFaultInjection& operator=(ReadFaultInjection&&) = delete;

    bool wasConsumed() const noexcept { return _consumed; }

  private:
    std::thread::id _ownerThreadId;
    bool _consumed = false;
  };
} // namespace ao::lmdb::detail
