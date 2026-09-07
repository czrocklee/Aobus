// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ExitController.h"

#include <utility>

namespace ao::tui
{
  ExitController::ExitController(Outputs outputs)
    : _outputs{std::move(outputs)}
  {
  }

  void ExitController::requestExit()
  {
    if (_phase == Phase::ExitPosted)
    {
      return;
    }

    if (_phase == Phase::WaitingForSubmittedWrite)
    {
      postExitOnce();
      return;
    }

    // Asked once, before anything is retired, because retirement is what takes
    // the editor and its pending submission away.
    auto const pending = _outputs.hasPendingSubmittedWrite && _outputs.hasPendingSubmittedWrite();
    _phase = pending ? Phase::WaitingForSubmittedWrite : Phase::ExitPosted;

    if (_outputs.retire)
    {
      _outputs.retire();
    }

    // Retirement can request exit again, or settle the write it was waiting
    // for; either already moved the phase, so only an unfinished pass posts.
    if (_phase == Phase::ExitPosted)
    {
      postExitOnce();
    }
  }

  void ExitController::notifySubmittedWriteSettled()
  {
    if (_phase != Phase::WaitingForSubmittedWrite)
    {
      return;
    }

    postExitOnce();
  }

  void ExitController::postExitOnce()
  {
    _phase = Phase::ExitPosted;

    if (std::exchange(_exitPosted, true))
    {
      return;
    }

    if (_outputs.postExit)
    {
      _outputs.postExit();
    }
  }
} // namespace ao::tui
