// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionAvailability.h>

#include <cstdint>

namespace ao::uimodel
{
  enum class LayoutActionActivationOutcome : std::uint8_t
  {
    Activated,
    UnknownAction,
    Disabled,
    InvalidBinding
  };

  struct LayoutActionActivationResult final
  {
    LayoutActionActivationOutcome outcome = LayoutActionActivationOutcome::UnknownAction;
    LayoutActionAvailability state = {};
  };
} // namespace ao::uimodel
