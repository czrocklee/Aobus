// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/DispatcherQueueExecutor.h"

#include "app/DispatcherQueueAdmission.h"
#include <ao/Contract.h>
#include <ao/async/QueuedExecutorBase.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <exception>
#include <memory>
#include <utility>

namespace ao::winui
{
  DispatcherQueueExecutor::DispatcherQueueExecutor(winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : _dispatcher{std::move(dispatcher)}, _dispatchStatePtr{std::make_shared<DispatchState>()}
  {
    _dispatchStatePtr->executorPtr.store(this);
  }

  DispatcherQueueExecutor::~DispatcherQueueExecutor()
  {
    AO_EXPECTS(isCurrent());
    bool const closed = _admission.closeForDestruction();
    AO_INVARIANT(closed, "Dispatcher queue executor destruction began inside an owner callback");
    _dispatchStatePtr->executorPtr.store(nullptr);
  }

  void DispatcherQueueExecutor::beginClosing() noexcept
  {
    AO_EXPECTS(isCurrent());
    bool const closingStarted = _admission.beginClosing();
    AO_INVARIANT(closingStarted, "Dispatcher queue executor closure began from an invalid state");
  }

  void DispatcherQueueExecutor::completeClosing() noexcept
  {
    AO_EXPECTS(isCurrent());
    bool const drainingStarted = _admission.beginDraining();
    AO_INVARIANT(drainingStarted, "Dispatcher queue executor final drain began with owner callbacks active");
    drainQueuedTasksUntilIdle();
    bool const closed = _admission.finishClosing();
    AO_INVARIANT(closed, "Dispatcher queue executor final drain did not reach quiescence");
  }

  void DispatcherQueueExecutor::dispatch(compat::MoveOnlyFunction<void()> task)
  {
    if (!task)
    {
      return;
    }

    auto const ownerThread = isCurrent();
    auto optAdmission = _admission.tryAcquire(ownerThread);
    AO_EXPECTS(optAdmission, "Dispatcher queue executor cannot accept work after its final drain");

    QueuedExecutorBase::dispatch(std::move(task));
  }

  void DispatcherQueueExecutor::defer(compat::MoveOnlyFunction<void()> task)
  {
    if (!task)
    {
      return;
    }

    auto optAdmission = _admission.tryAcquire(isCurrent());
    AO_EXPECTS(optAdmission, "Dispatcher queue executor cannot accept work after its final drain");
    QueuedExecutorBase::defer(std::move(task));
  }

  void DispatcherQueueExecutor::wake() noexcept
  {
    try
    {
      auto statePtr = _dispatchStatePtr;
      auto const queued = _dispatcher.TryEnqueue(
        [statePtr = std::move(statePtr)]
        {
          if (auto* const executor = statePtr->executorPtr.load(); executor != nullptr)
          {
            executor->drainQueuedTasks();
          }
        });

      if (!queued && detail::wakeRejectionDisposition(_admission.state()) ==
                       detail::DispatcherQueueWakeRejectionDisposition::Fatal)
      {
        AO_FATAL("Windows dispatcher rejected a callback while executor admission was open");
      }
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "Windows dispatcher wake");
    }
  }
} // namespace ao::winui
