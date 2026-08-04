// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutDialect.h"

#include <ao/uimodel/layout/document/LayoutDialect.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    uimodel::LayoutFieldVerdict layoutField(uimodel::LayoutNode const& /*node*/,
                                            std::string_view const name,
                                            uimodel::LayoutValue const& value)
    {
      if (name == "styleKey" || name == "surface")
      {
        return uimodel::LayoutFieldVerdict::rejected(
          uimodel::LayoutRejectionReason::UnsupportedLayoutField,
          "The GTK shell resolves appearance through CSS classes; use cssClasses");
      }

      if (name != "cssClasses")
      {
        return uimodel::LayoutFieldVerdict::unclaimed();
      }

      if (value.getIf<std::string>() == nullptr && value.getIf<std::vector<std::string>>() == nullptr)
      {
        return uimodel::LayoutFieldVerdict::rejected(
          uimodel::LayoutRejectionReason::InvalidLayoutFieldValue, "cssClasses must be a string or string list");
      }

      return uimodel::LayoutFieldVerdict::accepted();
    }
  } // namespace

  uimodel::LayoutDialect const& layoutDialect() noexcept
  {
    static constexpr auto kDialect = uimodel::LayoutDialect{
      .name = "GTK",
      .layoutField = &layoutField,
      .authorsTooltips = true,
    };

    return kDialect;
  }
} // namespace ao::gtk::layout
