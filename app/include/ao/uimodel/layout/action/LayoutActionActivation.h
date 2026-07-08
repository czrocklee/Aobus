// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionAvailability.h>

#include <cstdint>

namespace ao::uimodel
{
  enum class LayoutActionActivationResult : std::uint8_t
  {
    Activated,
    UnknownAction,
    Disabled,
    InvalidBinding
  };

  struct LayoutActionActivationOutcome final
  {
    LayoutActionActivationResult result = LayoutActionActivationResult::UnknownAction;
    LayoutActionAvailability state = {};
  };
} // namespace ao::uimodel
