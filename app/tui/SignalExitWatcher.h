// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>
#include <memory>

namespace ao::tui
{
  class SignalExitWatcher final
  {
  public:
    explicit SignalExitWatcher(std::move_only_function<void()> onExit);
    ~SignalExitWatcher();

    void requestExit();

    SignalExitWatcher(SignalExitWatcher const&) = delete;
    SignalExitWatcher& operator=(SignalExitWatcher const&) = delete;
    SignalExitWatcher(SignalExitWatcher&&) = delete;
    SignalExitWatcher& operator=(SignalExitWatcher&&) = delete;

  private:
    class Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::tui
