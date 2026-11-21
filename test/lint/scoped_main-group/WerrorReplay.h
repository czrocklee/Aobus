// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

// Only the analysis view sees this compiler warning. Under the product's
// warning promotion it must stay a filtered compiler diagnostic, not become
// an error that the report gate classifies as an incomplete analysis.
#ifdef AOBUS_LINT_HEADER_MAIN
#warning "Aobus lint header-main analysis keeps compiler warnings non-fatal"
#endif

inline std::int32_t werrorReplayValue()
{
  return 1;
}
