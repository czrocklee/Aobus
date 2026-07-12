// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"

#include <chrono>
#include <stop_token>

namespace ao::async
{
  /**
   * Injectable delay strategy for Runtime::sleepFor.
   *
   * A composition root may supply a virtual or controlled clock in place of the
   * Runtime's default steady-timer sleep. The Runtime holds a non-owning
   * pointer, so an injected Sleeper must outlive the Runtime that uses it.
   *
   * Each call completes exactly once, including when stop is requested. A
   * stopped sleep resumes on its awaiting executor so Runtime can observe the
   * stop token and finish the coroutine. Implementations must not retain a
   * completion handler after that completion or beyond the awaiting Runtime's
   * lifetime.
   */
  class Sleeper
  {
  public:
    virtual ~Sleeper() = default;

    Sleeper(Sleeper const&) = delete;
    Sleeper& operator=(Sleeper const&) = delete;
    Sleeper(Sleeper&&) = delete;
    Sleeper& operator=(Sleeper&&) = delete;

    virtual Task<void> sleepFor(std::chrono::milliseconds delay, std::stop_token stopToken) = 0;

  protected:
    Sleeper() = default;
  };
} // namespace ao::async
