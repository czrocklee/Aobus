// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::winui
{
  struct WindowModalWorkflowState final
  {
    bool listAuthoring = false;
    bool libraryTransfer = false;
    bool trackProperties = false;
  };

  constexpr bool hasActiveWindowModalWorkflow(WindowModalWorkflowState const state) noexcept
  {
    return state.listAuthoring || state.libraryTransfer || state.trackProperties;
  }
} // namespace ao::winui
