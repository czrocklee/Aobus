// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ElementKind.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    struct ElementKindEntry final
    {
      ElementKind kind;
      std::string_view name;
      std::optional<ElementKind> optBase;
    };

    // One row per kind, ordered so a base always precedes the kinds that derive
    // from it. The edges mirror the WinUI class hierarchy that decides whether a
    // Style's TargetType is assignable to a constructed element.
    constexpr auto kElementKinds = std::to_array<ElementKindEntry>({
      {.kind = ElementKind::FrameworkElement, .name = "FrameworkElement", .optBase = std::nullopt},
      {.kind = ElementKind::Panel, .name = "Panel", .optBase = ElementKind::FrameworkElement},
      {.kind = ElementKind::Grid, .name = "Grid", .optBase = ElementKind::Panel},
      {.kind = ElementKind::Border, .name = "Border", .optBase = ElementKind::FrameworkElement},
      {.kind = ElementKind::TextBlock, .name = "TextBlock", .optBase = ElementKind::FrameworkElement},
      {.kind = ElementKind::Control, .name = "Control", .optBase = ElementKind::FrameworkElement},
      {.kind = ElementKind::ContentControl, .name = "ContentControl", .optBase = ElementKind::Control},
      {.kind = ElementKind::ButtonBase, .name = "ButtonBase", .optBase = ElementKind::ContentControl},
      {.kind = ElementKind::Button, .name = "Button", .optBase = ElementKind::ButtonBase},
      {.kind = ElementKind::ItemsControl, .name = "ItemsControl", .optBase = ElementKind::Control},
      {.kind = ElementKind::ListView, .name = "ListView", .optBase = ElementKind::ItemsControl},
      {.kind = ElementKind::ScrollViewer, .name = "ScrollViewer", .optBase = ElementKind::ContentControl},
      {.kind = ElementKind::Slider, .name = "Slider", .optBase = ElementKind::Control},
      {.kind = ElementKind::AutoSuggestBox, .name = "AutoSuggestBox", .optBase = ElementKind::Control},
      {.kind = ElementKind::NavigationView, .name = "NavigationView", .optBase = ElementKind::ContentControl},
      {.kind = ElementKind::TreeView, .name = "TreeView", .optBase = ElementKind::Control},
      {.kind = ElementKind::MenuBar, .name = "MenuBar", .optBase = ElementKind::Control},
    });

    // The table is indexed by the enumerator, so the row order must track the
    // declaration order rather than merely contain every kind.
    consteval bool isElementKindTableIndexed()
    {
      for (std::size_t index = 0; index < kElementKinds.size(); ++index)
      {
        if (std::to_underlying(kElementKinds[index].kind) != index)
        {
          return false;
        }
      }

      return true;
    }

    static_assert(isElementKindTableIndexed(), "Windows element kind table must be indexed by its enumerator");
    static_assert(kElementKinds.size() == std::to_underlying(ElementKind::MenuBar) + 1,
                  "Windows element kind table must cover every enumerator");

    ElementKindEntry const& entry(ElementKind const kind) noexcept
    {
      return kElementKinds.at(static_cast<std::size_t>(std::to_underlying(kind)));
    }
  } // namespace

  std::optional<ElementKind> elementBase(ElementKind const kind) noexcept
  {
    return entry(kind).optBase;
  }

  bool isElementKindDerivedFrom(ElementKind const kind, ElementKind const base) noexcept
  {
    auto optCurrent = std::optional{kind};

    while (optCurrent)
    {
      if (*optCurrent == base)
      {
        return true;
      }

      optCurrent = entry(*optCurrent).optBase;
    }

    return false;
  }

  std::string_view toString(ElementKind const kind) noexcept
  {
    return entry(kind).name;
  }

  std::optional<ElementKind> elementKindFromString(std::string_view const name) noexcept
  {
    for (auto const& candidate : kElementKinds)
    {
      if (candidate.name == name)
      {
        return candidate.kind;
      }
    }

    return std::nullopt;
  }
} // namespace ao::winui
