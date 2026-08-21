// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackSection.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <string>

namespace ao::tui
{
  std::string trackSectionDisplayName(uimodel::PresentationTextCatalog const& textCatalog, TrackSection const& section)
  {
    return section.primaryText.empty() ? std::string{textCatalog.text(i18n::MessageId::TuiUntitledSection)}
                                       : section.primaryText;
  }
} // namespace ao::tui
