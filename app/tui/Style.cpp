// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Style.h"

#include "TextCell.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui::style
{
  namespace
  {
    ftxui::Element frameEdge(std::string_view const text)
    {
      return ftxui::text(std::string{text}) | ftxui::color(ftxui::Color::Default);
    }

    bool hasEdgeButton(PanelEdgeButton const& button)
    {
      return !button.label.empty() || !button.value.empty();
    }

    constexpr std::size_t kPanelTitleRightPartReserve = 5;

    std::int32_t bodyPaddingColumns(PanelOptions const options)
    {
      return 2 * std::max(0, options.bodyPaddingColumns);
    }

    PanelOptions popupPanelOptions(PanelOptions options)
    {
      options.bodyPaddingColumns = kPopupPanelBodyPaddingColumns;
      return options;
    }

    ftxui::Element edgeButton(PanelEdgeButton const button)
    {
      auto parts = ftxui::Elements{};

      if (!button.label.empty())
      {
        auto labelPtr = ftxui::text(std::string{button.label});
        parts.push_back(button.hovered ? std::move(labelPtr) : std::move(labelPtr) | muted());
      }

      if (!button.label.empty() && !button.value.empty())
      {
        parts.push_back(ftxui::text(" "));
      }

      if (!button.value.empty())
      {
        auto valuePtr = ftxui::text(std::string{button.value});
        parts.push_back(button.hovered ? std::move(valuePtr) : std::move(valuePtr) | accent() | ftxui::bold);
      }

      auto elementPtr = ftxui::hbox(std::move(parts));

      if (button.hovered)
      {
        elementPtr = std::move(elementPtr) | buttonHover();
      }

      if (button.box != nullptr)
      {
        elementPtr = std::move(elementPtr) | ftxui::reflect(*button.box);
      }

      return elementPtr;
    }

    ftxui::Element leftFooter(PanelEdgeButton const left, PanelEdgeButton const right)
    {
      auto parts = ftxui::Elements{};
      parts.push_back(frameEdge("╰─ "));
      parts.push_back(edgeButton(left));

      if (hasEdgeButton(right))
      {
        parts.push_back(frameEdge(" ─ "));
        parts.push_back(edgeButton(right));
      }

      parts.push_back(frameEdge(" "));
      return ftxui::hbox(std::move(parts)) | ftxui::clear_under;
    }

    ftxui::Element rightFooter(std::string_view const label)
    {
      return ftxui::clear_under(ftxui::hbox({
        frameEdge("─ "),
        ftxui::text(std::string{label}) | accent() | ftxui::bold,
        frameEdge(" "),
      }));
    }

    ftxui::Element bodyWithHorizontalPadding(ftxui::Element bodyPtr, std::int32_t const paddingColumns)
    {
      if (paddingColumns <= 0)
      {
        return bodyPtr;
      }

      auto const padding = std::string(static_cast<std::size_t>(paddingColumns), ' ');
      return ftxui::hbox({
        ftxui::text(padding),
        std::move(bodyPtr) | ftxui::xflex,
        ftxui::text(padding),
      });
    }

    ftxui::Element warmInteractiveSurface(ftxui::Element elementPtr)
    {
      return std::move(elementPtr) | ftxui::color(ftxui::Color::Black) | ftxui::bgcolor(ftxui::Color::Yellow) |
             ftxui::bold;
    }
  } // namespace

  ftxui::Decorator muted()
  {
    return [](ftxui::Element elementPtr) { return std::move(elementPtr) | ftxui::dim; };
  }

  ftxui::Decorator accent()
  {
    return ftxui::color(ftxui::Color::Cyan);
  }

  ftxui::Decorator success()
  {
    return ftxui::color(ftxui::Color::Green);
  }

  ftxui::Decorator warning()
  {
    return ftxui::color(ftxui::Color::Yellow);
  }

  ftxui::Decorator danger()
  {
    return ftxui::color(ftxui::Color::Red);
  }

  ftxui::Decorator interactiveSurface()
  {
    return [](ftxui::Element elementPtr) { return warmInteractiveSurface(std::move(elementPtr)); };
  }

  ftxui::Decorator selected()
  {
    return interactiveSurface();
  }

  ftxui::Decorator buttonHover()
  {
    return interactiveSurface();
  }

  ftxui::Element shortcutChip(std::string_view const key, std::string_view const label)
  {
    return ftxui::hbox({
      ftxui::text(std::string{key}) | accent() | ftxui::bold,
      ftxui::text(" " + std::string{label}) | ftxui::dim,
    });
  }

  ftxui::Element mutedSeparator(std::string_view const separator)
  {
    return ftxui::text(std::string{separator}) | ftxui::dim;
  }

  ftxui::Element panelFooterHint(std::string_view const hint)
  {
    return ftxui::text(std::string{hint}) | ftxui::dim;
  }

  ftxui::Element statusSlot(ftxui::Element bodyPtr, std::int32_t const minColumns)
  {
    return ftxui::hbox({
             frameEdge("│ "),
             std::move(bodyPtr) | ftxui::xflex,
             frameEdge(" │"),
           }) |
           ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, minColumns) | ftxui::clear_under;
  }

  std::int32_t titledPanelColumnsForContent(std::int32_t const contentColumns,
                                            std::int32_t const terminalColumns,
                                            PanelOptions const options)
  {
    return panelColumnsForContent(contentColumns + bodyPaddingColumns(options), terminalColumns);
  }

  std::int32_t titledPanelBodyColumns(std::int32_t const panelColumns, PanelOptions const options)
  {
    return std::max(0, panelColumns - kPanelBorderColumns - bodyPaddingColumns(options));
  }

  std::int32_t popupPanelColumnsForContent(std::int32_t const contentColumns, std::int32_t const terminalColumns)
  {
    return titledPanelColumnsForContent(contentColumns, terminalColumns, popupPanelOptions(PanelOptions{}));
  }

  std::int32_t popupPanelBodyColumns(std::int32_t const panelColumns)
  {
    return titledPanelBodyColumns(panelColumns, popupPanelOptions(PanelOptions{}));
  }

  ftxui::Element titledPanel(std::string_view const title, ftxui::Element bodyPtr, PanelOptions const options)
  {
    auto titlePart = [](std::string_view const label, ftxui::Box* const box)
    {
      auto elementPtr = ftxui::text(std::string{label}) | accent() | ftxui::bold;

      if (box != nullptr)
      {
        elementPtr = std::move(elementPtr) | ftxui::reflect(*box);
      }

      return elementPtr;
    };

    auto titleElementPtr = ftxui::emptyElement();

    if (!title.empty())
    {
      auto titleElements = ftxui::Elements{};
      titleElements.reserve(options.rightTitle.empty() ? 3 : kPanelTitleRightPartReserve);
      titleElements.push_back(ftxui::text("─ ") | accent() | ftxui::bold);
      titleElements.push_back(titlePart(title, options.titleBox));

      if (!options.rightTitle.empty())
      {
        titleElements.push_back(ftxui::text(" ─ ") | accent() | ftxui::bold);
        titleElements.push_back(titlePart(options.rightTitle, options.rightTitleBox));
      }

      titleElements.push_back(ftxui::text(" ") | accent() | ftxui::bold);
      titleElementPtr = ftxui::hbox(std::move(titleElements));
    }

    auto panelPtr = ftxui::window(
      std::move(titleElementPtr), bodyWithHorizontalPadding(std::move(bodyPtr), options.bodyPaddingColumns));

    if (auto const hasLeftFooter = hasEdgeButton(options.leftFooter); hasLeftFooter || !options.rightFooter.empty())
    {
      auto bottomElements = ftxui::Elements{};

      if (hasLeftFooter)
      {
        bottomElements.push_back(leftFooter(options.leftFooter, options.leftFooterRight));
      }

      bottomElements.push_back(ftxui::filler());

      if (!options.rightFooter.empty())
      {
        bottomElements.push_back(rightFooter(options.rightFooter));
        bottomElements.push_back(ftxui::text("─╯"));
      }

      panelPtr = ftxui::dbox({
        std::move(panelPtr),
        ftxui::vbox({
          ftxui::filler(),
          ftxui::hbox(std::move(bottomElements)),
        }),
      });
    }

    return panelPtr;
  }

  ftxui::Element popupPanel(std::string_view const title, ftxui::Element bodyPtr, PanelOptions const options)
  {
    return titledPanel(title, std::move(bodyPtr), popupPanelOptions(options));
  }
} // namespace ao::tui::style
