// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/MainContextCallbackScope.h"

#include <utility>

namespace ao::gtk
{
  void MainContextCallbackScope::close()
  {
    if (!_statePtr)
    {
      return;
    }

    _statePtr.reset();

    if (auto closeCallback = std::move(_closeCallback); closeCallback)
    {
      closeCallback();
    }
  }
} // namespace ao::gtk
