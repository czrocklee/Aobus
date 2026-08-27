// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackSection.h"

#include <ao/i18n/MessageCatalog.h>

#include <string>

namespace ao::tui
{
  std::string trackSectionDisplayName(i18n::MessageCatalog const& textCatalog, TrackSection const& section)
  {
    return section.primaryText.empty()
             ? std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiUntitledSection)}
             : section.primaryText;
  }
} // namespace ao::tui
