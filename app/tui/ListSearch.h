// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "MouseBindings.h"
#include "TextFieldModel.h"
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::tui
{
  /// A local list query; selection always refers to the unfiltered owner's rows.
  class ListSearch final
  {
  public:
    bool isActive() const noexcept { return _active; }
    std::string_view query() const noexcept { return _query.value(); }
    void clear();
    void invalidateMouseRegions() const;
    bool tryHandleEvent(ftxui::Event const& event);
    bool matches(std::string_view label) const;
    /// Candidate keys must use utility::makeUtf8CaselessKey, like the query.
    bool matchesFolded(std::string_view candidateKey) const;
    std::optional<std::int32_t> selection(std::span<std::string const> labels,
                                          std::int32_t selected,
                                          std::int32_t delta = 0) const;
    ftxui::Element render(i18n::MessageCatalog const& textCatalog, bool focused = true, bool interactive = true) const;

  private:
    bool _active = false;
    TextFieldModel _query{};
    std::string _key{};
    mutable ftxui::Box _hintBox = kEmptyMouseBox;
    mutable ftxui::Box _inputBox = kEmptyMouseBox;
    mutable ftxui::Box _inputOrigin = kEmptyMouseBox;
    mutable std::string _renderedQuery{};
    mutable std::size_t _renderedCursor = 0;
  };
} // namespace ao::tui
