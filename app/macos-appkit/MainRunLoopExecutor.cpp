// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MainRunLoopExecutor.h"

#include <ao/Contract.h>

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFRunLoop.h>

#include <exception>

namespace ao::appkit
{
  MainRunLoopExecutor::MainRunLoopExecutor()
  {
    AO_EXPECTS(::CFRunLoopGetCurrent() == ::CFRunLoopGetMain());
    auto context = ::CFRunLoopSourceContext{};
    context.info = this;
    context.perform = &MainRunLoopExecutor::perform;
    _source = ::CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    AO_INVARIANT(_source != nullptr, "Cannot create AppKit executor source");
    ::CFRunLoopAddSource(::CFRunLoopGetMain(), _source, kCFRunLoopCommonModes);
  }

  MainRunLoopExecutor::~MainRunLoopExecutor()
  {
    AO_EXPECTS(isCurrent());
    // AppRuntime owns this executor and destroys it after its callback consumers.
    // The window session has already joined producers and drained their callbacks.
    ::CFRunLoopSourceInvalidate(_source);
    ::CFRelease(_source);
  }

  void MainRunLoopExecutor::finishClosing() noexcept
  {
    AO_EXPECTS(isCurrent());
    drainQueuedTasksUntilIdle();
    ::CFRunLoopSourceInvalidate(_source);
  }

  void MainRunLoopExecutor::wake() noexcept
  {
    ::CFRunLoopSourceSignal(_source);
    ::CFRunLoopWakeUp(::CFRunLoopGetMain());
  }

  void MainRunLoopExecutor::perform(void* context) noexcept
  {
    try
    {
      auto* executor = static_cast<MainRunLoopExecutor*>(context);
      AO_EXPECTS(executor->isCurrent());
      auto const wasPerforming = executor->_performing;
      executor->_performing = true;
      executor->drainQueuedTasks();
      executor->_performing = wasPerforming;
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "AppKit executor callback");
    }
  }
} // namespace ao::appkit
