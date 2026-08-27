// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <chrono>
#include <cstddef>
#include <optional>

namespace ao::rt
{
  class ViewService;
}

namespace ao::gtk
{
  /**
   * SelectionInfoLabel displays "N items selected" with optional total duration.
   * It self-subscribes to ViewService selection changes.
   */
  class SelectionInfoLabel final
  {
  public:
    SelectionInfoLabel(rt::ViewService& viewService, i18n::MessageCatalog textCatalog);
    ~SelectionInfoLabel();

    // Not copyable or movable
    SelectionInfoLabel(SelectionInfoLabel const&) = delete;
    SelectionInfoLabel& operator=(SelectionInfoLabel const&) = delete;
    SelectionInfoLabel(SelectionInfoLabel&&) = delete;
    SelectionInfoLabel& operator=(SelectionInfoLabel&&) = delete;

    Gtk::Widget& widget() { return _label; }

  private:
    void updateState(std::size_t count, std::optional<std::chrono::milliseconds> optTotalDuration = std::nullopt);

    rt::ViewService& _viewService;
    i18n::MessageCatalog _textCatalog;
    Gtk::Label _label;
    async::Subscription _selectionChangedSub;
  };
} // namespace ao::gtk
