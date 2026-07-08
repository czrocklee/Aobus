// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackSection.h"

#include <string>

namespace ao::tui
{
  std::string trackSectionDisplayName(TrackSection const& section)
  {
    return section.primaryText.empty() ? std::string{"Untitled Section"} : section.primaryText;
  }
} // namespace ao::tui
