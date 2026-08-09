// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <source_location>

namespace ao::rt::detail
{
  void expectPreparedNextSlotAvailable(bool hasActivePreparedToken,
                                       std::source_location location = std::source_location::current());
} // namespace ao::rt::detail
