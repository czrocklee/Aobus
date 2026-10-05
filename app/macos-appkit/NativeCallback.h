// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>

#include <exception>
#include <utility>

namespace ao::appkit
{
  // AppKit target/action, delegate, and notification callbacks are noexcept
  // boundaries: an exception escaping their C++ work is fatal rather than an
  // Objective-C unwind through native frames.
  template<typename Callback>
  void nativeCallback(Callback&& callback) noexcept
  {
    try
    {
      std::forward<Callback>(callback)();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "AppKit native callback");
    }
  }
} // namespace ao::appkit
