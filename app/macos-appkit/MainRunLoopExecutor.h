// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/async/QueuedExecutorBase.h>

#include <CoreFoundation/CFRunLoop.h>

namespace ao::appkit
{
  class MainRunLoopExecutor final : public async::QueuedExecutorBase
  {
  public:
    MainRunLoopExecutor();
    ~MainRunLoopExecutor() override;

    MainRunLoopExecutor(MainRunLoopExecutor const&) = delete;
    MainRunLoopExecutor& operator=(MainRunLoopExecutor const&) = delete;
    MainRunLoopExecutor(MainRunLoopExecutor&&) = delete;
    MainRunLoopExecutor& operator=(MainRunLoopExecutor&&) = delete;

    // The owner must join all producers first, keeping callback consumers alive.
    void finishClosing() noexcept;
    bool isPerforming() const noexcept { return _performing; }

  private:
    void wake() noexcept override;
    static void perform(void* context) noexcept;

    ::CFRunLoopSourceRef _source = nullptr;
    bool _performing = false;
  };
} // namespace ao::appkit
