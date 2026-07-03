// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/LibraryScanner.h>

#include <string_view>

namespace ao::cli
{
  std::string_view scanClassificationName(library::ScanClassification classification);
} // namespace ao::cli
