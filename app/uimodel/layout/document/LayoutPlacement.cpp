// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutPlacement.h>

#include <ao/uimodel/layout/document/LayoutNode.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kHorizontalExpandProp = std::string_view{"hexpand"};
    constexpr auto kVerticalExpandProp = std::string_view{"vexpand"};
    constexpr auto kHorizontalAlignProp = std::string_view{"halign"};
    constexpr auto kVerticalAlignProp = std::string_view{"valign"};
    constexpr auto kWidthRequestProp = std::string_view{"widthRequest"};
    constexpr auto kHeightRequestProp = std::string_view{"heightRequest"};
    constexpr auto kVisibleProp = std::string_view{"visible"};

    constexpr auto kCommonLayoutProps = std::to_array<std::string_view>({kHorizontalExpandProp,
                                                                         kVerticalExpandProp,
                                                                         kHorizontalAlignProp,
                                                                         kVerticalAlignProp,
                                                                         kWidthRequestProp,
                                                                         kHeightRequestProp,
                                                                         kVisibleProp});

    /// A size request keeps the version 1 meaning where a negative value asks for no minimum.
    std::optional<double> minimumSize(LayoutValueMap const& layout, std::string_view const name)
    {
      auto const it = layout.find(name);

      if (it == layout.end() || !it->second.isNumber())
      {
        return std::nullopt;
      }

      auto const value = it->second.asDouble();
      return value < 0.0 ? std::nullopt : std::optional{value};
    }

    bool hasNumber(LayoutValueMap const& layout, std::string_view const name)
    {
      auto const it = layout.find(name);
      return it != layout.end() && it->second.isNumber();
    }

    /// A boolean field, absent when the document never wrote it down.
    std::optional<bool> flag(LayoutValueMap const& layout, std::string_view const name)
    {
      auto const it = layout.find(name);

      if (it == layout.end())
      {
        return std::nullopt;
      }

      return it->second.asBool();
    }

    std::optional<LayoutAlignment> alignment(LayoutValueMap const& layout, std::string_view const name)
    {
      auto const it = layout.find(name);

      if (it == layout.end())
      {
        return std::nullopt;
      }

      return layoutAlignmentFromString(it->second.asString());
    }
  } // namespace

  bool isCommonLayoutProp(std::string_view const name) noexcept
  {
    return std::ranges::contains(kCommonLayoutProps, name);
  }

  std::optional<LayoutAlignment> layoutAlignmentFromString(std::string_view const name) noexcept
  {
    if (name == "fill")
    {
      return LayoutAlignment::Fill;
    }

    if (name == "start")
    {
      return LayoutAlignment::Start;
    }

    if (name == "end")
    {
      return LayoutAlignment::End;
    }

    if (name == "center")
    {
      return LayoutAlignment::Center;
    }

    return std::nullopt;
  }

  LayoutPlacement planLayoutPlacement(LayoutNode const& node)
  {
    auto const& layout = node.layout;

    return LayoutPlacement{
      .optHorizontalExpand = flag(layout, kHorizontalExpandProp),
      .optVerticalExpand = flag(layout, kVerticalExpandProp),
      .optHorizontalAlignment = alignment(layout, kHorizontalAlignProp),
      .optVerticalAlignment = alignment(layout, kVerticalAlignProp),
      .optMinWidth = minimumSize(layout, kWidthRequestProp),
      .optMinHeight = minimumSize(layout, kHeightRequestProp),
      .widthRequestAuthored = hasNumber(layout, kWidthRequestProp),
      .heightRequestAuthored = hasNumber(layout, kHeightRequestProp),
      .optAuthoredVisible = flag(layout, kVisibleProp),
    };
  }
} // namespace ao::uimodel
