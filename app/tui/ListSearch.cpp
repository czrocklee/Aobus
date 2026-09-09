// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ListSearch.h"

#include "MouseBindings.h"
#include "SelectionNavigation.h"
#include "TextField.h"
#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/utility/UnicodeText.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::string listSearchKey(std::string_view const value)
    {
      auto keyRes = utility::makeUtf8CaselessKey(value);
      AO_INVARIANT(keyRes, "Validated list search text failed Unicode case folding: {}", keyRes.error().message);
      return std::move(*keyRes);
    }
  } // namespace

  void ListSearch::invalidateMouseRegions() const
  {
    _hintBox = kEmptyMouseBox;
    _inputBox = kEmptyMouseBox;
    _inputOrigin = kEmptyMouseBox;
  }

  void ListSearch::clear()
  {
    _active = false;
    _query.reset({});
    _key.clear();
    invalidateMouseRegions();
  }

  bool ListSearch::tryHandleEvent(ftxui::Event const& event)
  {
    if (!_active)
    {
      if (event.is_mouse())
      {
        auto mouseEvent = event;
        _active = isLeftPress(mouseEvent.mouse()) && containsMouse(_hintBox, mouseEvent.mouse());
        return _active;
      }

      if (event != ftxui::Event::Character("/"))
      {
        return false;
      }

      _active = true;
      return true;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;

      if (auto const& mouse = mouseEvent.mouse(); isLeftPress(mouse) && containsMouse(_inputBox, mouse) &&
                                                  _renderedQuery == _query.value() &&
                                                  _renderedCursor == _query.cursor())
      {
        _query.tryMoveToCell(mouse.x - _inputOrigin.x_min);
        return true;
      }

      return false;
    }

    if (event == ftxui::Event::Escape)
    {
      clear();
      return true;
    }

    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown || event == ftxui::Event::PageUp ||
        event == ftxui::Event::PageDown || event == ftxui::Event::Return || event == ftxui::Event::Tab ||
        event == ftxui::Event::TabReverse)
    {
      return false;
    }

    _query.tryApplyEvent(event);
    _key = listSearchKey(_query.value());
    return true;
  }

  bool ListSearch::matches(std::string_view const label) const
  {
    if (_key.empty())
    {
      return true;
    }

    return matchesFolded(listSearchKey(label));
  }

  bool ListSearch::matchesFolded(std::string_view const candidateKey) const
  {
    return candidateKey.contains(_key);
  }

  std::optional<std::int32_t> ListSearch::selection(std::span<std::string const> const labels,
                                                    std::int32_t const selected,
                                                    std::int32_t const delta) const
  {
    auto visible = std::vector<std::int32_t>{};

    for (std::size_t index = 0; index < labels.size(); ++index)
    {
      if (matches(labels[index]))
      {
        visible.push_back(static_cast<std::int32_t>(index));
      }
    }

    if (visible.empty())
    {
      return std::nullopt;
    }

    auto const found = std::ranges::find(visible, selected);
    auto const index = found == visible.end() ? 0 : static_cast<std::int32_t>(found - visible.begin());
    return visible[static_cast<std::size_t>(moveSelection(index, delta, visible.size()))];
  }

  ftxui::Element ListSearch::render(i18n::MessageCatalog const& textCatalog,
                                    bool const focused,
                                    bool const interactive) const
  {
    using namespace ftxui;

    if (!_active)
    {
      return text(std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiListSearchHint)}) | dim |
             (interactive ? reflect(_hintBox) : nothing);
    }

    _renderedQuery = _query.value();
    _hintBox = kEmptyMouseBox;
    _renderedCursor = _query.cursor();
    return hbox({text("/ "),
                 textFieldValue(_query, interactive ? &_inputOrigin : nullptr, focused) | flex |
                   (interactive ? reflect(_inputBox) : nothing)});
  }
} // namespace ao::tui
