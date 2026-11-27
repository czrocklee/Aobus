// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "CompletionTokenizer.h"

#include <span>
#include <string_view>

namespace ao::query::detail
{
  // `tokens` is the already-scanned span for `prefix`, with trailing whitespace excluded.
  bool hasCompletionPredicateShape(std::string_view prefix, std::span<CompletionToken const> tokens);
} // namespace ao::query::detail
