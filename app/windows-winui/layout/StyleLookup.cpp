// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/StyleLookup.h>

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/layout/ElementKind.h>
#include <ao/winui/layout/LayoutCatalog.h>

#include <optional>
#include <string>

namespace ao::winui
{
  std::optional<StyleLookupPlan> planStyleLookup(uimodel::LayoutNode const& node, ElementKind const elementKind)
  {
    auto const it = node.layout.find(kStyleKeyLayoutProp);

    if (it == node.layout.end())
    {
      return std::nullopt;
    }

    auto const* const key = it->second.getIf<std::string>();

    if (key == nullptr || key->empty())
    {
      return std::nullopt;
    }

    return StyleLookupPlan{.key = *key, .elementKind = elementKind};
  }

  StyleResolution resolveStyle(std::optional<StyleLookupPlan> const& optPlan,
                               StyleScope const scope,
                               std::optional<ElementKind> const optStyleTarget) noexcept
  {
    if (!optPlan)
    {
      return StyleResolution::NoStyleAuthored;
    }

    if (scope != StyleScope::RootGridResources || !optStyleTarget)
    {
      return StyleResolution::MissingKey;
    }

    return isElementKindDerivedFrom(optPlan->elementKind, *optStyleTarget) ? StyleResolution::Applied
                                                                           : StyleResolution::IncompatibleTarget;
  }
} // namespace ao::winui
