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
    if (_phase != Phase::Running)
    {
      return;
    }

    _phase = Phase::ExitPosted;

    if (_outputs.retire)
    {
      _outputs.retire();
    }

    if (_outputs.postExit)
    {
      _outputs.postExit();
    }
  }
} // namespace ao::tui
