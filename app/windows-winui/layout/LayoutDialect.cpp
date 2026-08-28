// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutDialect.h>

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/layout/ElementKind.h>
#include <ao/winui/layout/LayoutDialect.h>
#include <ao/winui/layout/LayoutSchema.h>
#include <ao/winui/layout/ThemeSurface.h>

#include <format>
#include <string>
#include <string_view>

namespace ao::winui
{
  namespace
  {
    /**
     * @brief The field the GTK shell styles through.
     *
     * GTK styles its widgets through CSS classes. This shell resolves appearance
     * through XAML resources, so an authored `cssClasses` field is a document
     * defect rather than an ignorable field. Named here rather than in the
     * schema because rejecting it is all this shell has to say about it.
     */
    constexpr auto kGtkCssClassesLayoutProp = std::string_view{"cssClasses"};

    uimodel::LayoutFieldVerdict verdictForStyleKey(uimodel::LayoutValue const& value)
    {
      if (auto const* const key = value.getIf<std::string>(); key == nullptr || key->empty())
      {
        return uimodel::LayoutFieldVerdict::rejected(
          uimodel::LayoutRejectionReason::InvalidLayoutFieldValue, "styleKey must be a non-empty resource key");
      }

      return uimodel::LayoutFieldVerdict::accepted();
    }

    uimodel::LayoutFieldVerdict verdictForSurface(uimodel::LayoutNode const& node, uimodel::LayoutValue const& value)
    {
      if (auto const* const slot = value.getIf<std::string>(); slot == nullptr || !themeSurfaceFromString(*slot))
      {
        return uimodel::LayoutFieldVerdict::rejected(uimodel::LayoutRejectionReason::InvalidLayoutFieldValue,
                                                     std::format("Unsupported themed surface '{}'", value.asString()));
      }

      // A slot the element cannot paint would read as authored intent and do
      // nothing, so it is rejected rather than dropped.
      if (auto const optKind = componentElementKind(node); optKind && !elementKindAcceptsSurface(*optKind))
      {
        return uimodel::LayoutFieldVerdict::rejected(
          uimodel::LayoutRejectionReason::UnsupportedLayoutField,
          std::format("A {} owns no background and cannot carry a themed surface", toString(*optKind)));
      }

      return uimodel::LayoutFieldVerdict::accepted();
    }

    uimodel::LayoutFieldVerdict layoutField(uimodel::LayoutNode const& node,
                                            std::string_view const name,
                                            uimodel::LayoutValue const& value)
    {
      if (name == kGtkCssClassesLayoutProp)
      {
        return uimodel::LayoutFieldVerdict::rejected(
          uimodel::LayoutRejectionReason::UnsupportedLayoutField,
          "The Windows shell resolves appearance through XAML resources; use styleKey");
      }

      if (name == kStyleKeyLayoutProp)
      {
        return verdictForStyleKey(value);
      }

      if (name == kSurfaceLayoutProp)
      {
        return verdictForSurface(node, value);
      }

      return uimodel::LayoutFieldVerdict::unclaimed();
    }
  } // namespace

  uimodel::LayoutDialect const& layoutDialect() noexcept
  {
    static constexpr auto kDialect = uimodel::LayoutDialect{
      .name = "Windows",
      .layoutField = &layoutField,
      .presentationChildCount = &presentationChildCount,
      .requiresStableId = &componentRequiresId,
      .authorsTooltips = false,
    };

    return kDialect;
  }
} // namespace ao::winui
