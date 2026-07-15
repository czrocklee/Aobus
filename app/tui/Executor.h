// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/QueuedExecutorBase.h>

#include <ftxui/component/screen_interactive.hpp>

#include <functional>

namespace ao::tui
{
  class Executor final : public async::QueuedExecutorBase
  {
  public:
    explicit Executor(ftxui::ScreenInteractive& screen);
    ~Executor() override = default;

    Executor(Executor const&) = delete;
    Executor& operator=(Executor const&) = delete;
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    void drainPendingTasks();

  private:
    void wake() override;
    void executeTask(std::move_only_function<void()>& task) override;

    ftxui::ScreenInteractive& _screen;
  };
} // namespace ao::tui
