// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/library/ScanPlan.h>

#include <string_view>

namespace ao::cli
{
  std::string_view scanClassificationName(rt::ScanClassification classification);
} // namespace ao::cli
