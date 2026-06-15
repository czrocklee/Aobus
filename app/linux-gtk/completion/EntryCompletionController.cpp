// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "completion/EntryCompletionController.h"

#include <ao/rt/CompletionItem.h>
#include <ao/rt/CompletionResult.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <giomm/liststore.h>
#include <glib.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/singleselection.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr guint kInvalidListPosition = std::numeric_limits<guint>::max();

    class CompletionListItem final : public Glib::Object
    {
    public:
      rt::CompletionItem const& item() const noexcept { return _item; }

      static Glib::RefPtr<CompletionListItem> create(rt::CompletionItem item)
      {
        auto objPtr = Glib::make_refptr_for_instance<CompletionListItem>(new CompletionListItem{});
        objPtr->_item = std::move(item);
        return objPtr;
      }

    protected:
      CompletionListItem()
        : Glib::ObjectBase{"CompletionListItem"}
      {
      }

    private:
      rt::CompletionItem _item;
    };

    std::size_t charOffsetToByteOffset(std::string const& text, std::int32_t charOffset)
    {
      if (charOffset <= 0)
      {
        return 0;
      }

      auto const* const begin = text.c_str();
      auto const charCount = ::g_utf8_strlen(begin, static_cast<gssize>(text.size()));
      auto const clampedOffset = std::min(static_cast<glong>(charOffset), charCount);
      auto const* const pointer = ::g_utf8_offset_to_pointer(begin, clampedOffset);
      return static_cast<std::size_t>(pointer - begin);
    }

    std::int32_t byteOffsetToCharOffset(std::string const& text, std::size_t byteOffset)
    {
      auto const clampedOffset = std::min(byteOffset, text.size());
      auto const* const begin = text.c_str();
      auto const charOffset = ::g_utf8_pointer_to_offset(begin, begin + clampedOffset);
      auto const maxOffset = static_cast<glong>(std::numeric_limits<std::int32_t>::max());
      return static_cast<std::int32_t>(std::min(charOffset, maxOffset));
    }
  }

  EntryCompletionController::EntryCompletionController(Gtk::Entry& entry,
                                                       rt::CompletionProvider provider,
                                                       EntryCompletionControllerOptions options)
    : _entry{entry}, _provider{std::move(provider)}, _options{options}
  {
    _itemsPtr = Gio::ListStore<Glib::Object>::create();
    _selectionPtr = Gtk::SingleSelection::create(_itemsPtr);

    auto factoryPtr = Gtk::SignalListItemFactory::create();
    factoryPtr->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->add_css_class("ao-query-completion-row");

        auto* const title = Gtk::make_managed<Gtk::Label>("");
        title->set_halign(Gtk::Align::START);
        title->set_xalign(0.0F);
        title->set_hexpand(true);
        title->add_css_class("ao-query-completion-row-title");
        row->append(*title);

        auto* const detail = Gtk::make_managed<Gtk::Label>("");
        detail->set_halign(Gtk::Align::END);
        detail->set_xalign(1.0F);
        detail->add_css_class("ao-query-completion-row-detail");
        row->append(*detail);

        listItem->set_child(*row);
      });

    factoryPtr->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto* const row = dynamic_cast<Gtk::Box*>(listItem->get_child());

        if (row == nullptr)
        {
          return;
        }

        auto* const title = dynamic_cast<Gtk::Label*>(row->get_first_child());
        auto* const detail = title == nullptr ? nullptr : dynamic_cast<Gtk::Label*>(title->get_next_sibling());
        auto const itemPtr = std::dynamic_pointer_cast<CompletionListItem>(listItem->get_item());

        if (title == nullptr || detail == nullptr || !itemPtr)
        {
          return;
        }

        auto const& item = itemPtr->item();
        title->set_text(item.displayText);
        detail->set_text(item.detail);
        detail->set_visible(!item.detail.empty());
      });

    _listView.set_factory(factoryPtr);
    _listView.set_model(_selectionPtr);
    _listView.set_can_focus(false);
    _listView.set_focusable(false);
    _listView.set_single_click_activate(true);
    _listView.signal_activate().connect(
      [this](guint position)
      {
        if (!_itemsPtr || position >= _itemsPtr->get_n_items())
        {
          return;
        }

        _selectionPtr->set_selected(position);
        applySelected();
      });

    _scrolledWindow.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _scrolledWindow.set_min_content_width(_options.popoverWidth);
    _scrolledWindow.set_min_content_height(0);
    _scrolledWindow.set_max_content_height(_options.popoverMaxHeight);
    _scrolledWindow.set_propagate_natural_height(true);
    _scrolledWindow.set_vexpand(false);
    _scrolledWindow.set_valign(Gtk::Align::START);
    _scrolledWindow.set_can_focus(false);
    _scrolledWindow.set_focusable(false);
    _scrolledWindow.set_child(_listView);

    _listView.set_vexpand(false);
    _listView.set_valign(Gtk::Align::START);

    _popover.set_autohide(false);
    _popover.set_has_arrow(false);
    _popover.set_position(Gtk::PositionType::BOTTOM);
    _popover.set_child(_scrolledWindow);
    _popover.set_parent(_entry);
    _popover.add_css_class("ao-query-completion-popover");
    _popover.signal_closed().connect(
      [this]
      {
        clearCompletionState();
        _dismissController.remove();
      });

    _changedConnection = _entry.signal_changed().connect([this] { update(); });

    _keyControllerPtr = Gtk::EventControllerKey::create();
    _keyControllerPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _keyControllerPtr->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) { return handleKeyPressed(keyval); }, false);
    _entry.add_controller(_keyControllerPtr);

    _popoverKeyControllerPtr = Gtk::EventControllerKey::create();
    _popoverKeyControllerPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _popoverKeyControllerPtr->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) { return handleKeyPressed(keyval); }, false);
    _popover.add_controller(_popoverKeyControllerPtr);

    _clickControllerPtr = Gtk::GestureClick::create();
    _clickControllerPtr->signal_released().connect([this](std::int32_t, double, double) { update(); });
    _entry.add_controller(_clickControllerPtr);

    _focusControllerPtr = Gtk::EventControllerFocus::create();
    _focusControllerPtr->signal_leave().connect([this] { hide(); });
    _entry.add_controller(_focusControllerPtr);
  }

  EntryCompletionController::~EntryCompletionController()
  {
    if (_keyControllerPtr)
    {
      _entry.remove_controller(_keyControllerPtr);
    }

    if (_clickControllerPtr)
    {
      _entry.remove_controller(_clickControllerPtr);
    }

    if (_focusControllerPtr)
    {
      _entry.remove_controller(_focusControllerPtr);
    }

    if (_popoverKeyControllerPtr)
    {
      _popover.remove_controller(_popoverKeyControllerPtr);
    }

    _dismissController.remove();
    _popover.popdown();
    _popover.unparent();
  }

  bool EntryCompletionController::handleKeyPressed(std::uint32_t keyval)
  {
    if (!_popover.get_visible())
    {
      if (keyval == GDK_KEY_Left || keyval == GDK_KEY_Right || keyval == GDK_KEY_Home || keyval == GDK_KEY_End)
      {
        hide();
      }

      return false;
    }

    switch (keyval)
    {
      case GDK_KEY_Up: return moveSelection(-1);
      case GDK_KEY_Down: return moveSelection(1);
      case GDK_KEY_Tab:
      case GDK_KEY_KP_Tab:
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter: applySelected(); return true;
      case GDK_KEY_Escape: hide(); return true;
      case GDK_KEY_Left:
      case GDK_KEY_Right:
      case GDK_KEY_Home:
      case GDK_KEY_End: hide(); return false;
      default: return false;
    }
  }

  void EntryCompletionController::update()
  {
    auto const text = std::string{_entry.get_text()};
    auto const cursor = _entry.get_position();

    if (cursor < 0)
    {
      hide();
      return;
    }

    auto optResult = _provider(text, charOffsetToByteOffset(text, cursor));

    if (!optResult || optResult->items.empty())
    {
      hide();
      return;
    }

    _replaceBegin = optResult->replaceBegin;
    _replaceEnd = optResult->replaceEnd;
    _hasReplacement = true;
    auto newItems = std::vector<Glib::RefPtr<Glib::Object>>{};
    newItems.reserve(optResult->items.size());

    for (auto& item : optResult->items)
    {
      newItems.push_back(CompletionListItem::create(std::move(item)));
    }

    _itemsPtr->splice(0, _itemsPtr->get_n_items(), newItems);

    _selectionPtr->set_selected(0);
    _listView.scroll_to(0);

    if (!_options.suppressPopup && !_popover.get_visible())
    {
      _popover.popup();
    }

    if (!_options.suppressPopup)
    {
      _dismissController.install(_entry, {&_entry, &_popover}, [this] { hide(); });
    }
  }

  void EntryCompletionController::hide()
  {
    clearCompletionState();

    if (_popover.get_visible())
    {
      _popover.popdown();
    }

    _dismissController.remove();
  }

  void EntryCompletionController::clearCompletionState()
  {
    _hasReplacement = false;

    if (_itemsPtr)
    {
      _itemsPtr->remove_all();
    }
  }

  void EntryCompletionController::applySelected()
  {
    if (!_hasReplacement || !_itemsPtr || _itemsPtr->get_n_items() == 0)
    {
      hide();
      return;
    }

    auto selected = _selectionPtr ? _selectionPtr->get_selected() : kInvalidListPosition;

    if (selected == kInvalidListPosition || selected >= _itemsPtr->get_n_items())
    {
      selected = 0;
    }

    auto const itemPtr = std::dynamic_pointer_cast<CompletionListItem>(_itemsPtr->get_item(selected));

    if (!itemPtr)
    {
      hide();
      return;
    }

    auto const& replacement = itemPtr->item().insertText;
    auto text = std::string{_entry.get_text()};

    if (_replaceBegin > text.size() || _replaceEnd > text.size() || _replaceBegin > _replaceEnd)
    {
      hide();
      return;
    }

    text.replace(_replaceBegin, _replaceEnd - _replaceBegin, replacement);
    auto const newCursor = byteOffsetToCharOffset(text, _replaceBegin + replacement.size());

    _changedConnection.block();
    _entry.set_text(text);
    _entry.set_position(newCursor);
    _changedConnection.unblock();
    hide();
  }

  bool EntryCompletionController::moveSelection(std::int32_t delta)
  {
    if (!_selectionPtr || !_itemsPtr || _itemsPtr->get_n_items() == 0)
    {
      return false;
    }

    auto selected = _selectionPtr->get_selected();
    auto const itemCount = static_cast<std::int32_t>(_itemsPtr->get_n_items());
    auto const current = (selected == kInvalidListPosition) ? 0 : static_cast<std::int32_t>(selected);
    auto const next = std::clamp(current + delta, 0, itemCount - 1);

    _selectionPtr->set_selected(static_cast<guint>(next));
    _listView.scroll_to(static_cast<guint>(next));
    return true;
  }

  void EntryCompletionController::setTextProgrammatically(Glib::ustring const& text)
  {
    _changedConnection.block();
    _entry.set_text(text);
    _changedConnection.unblock();
    hide();
  }
} // namespace ao::gtk
