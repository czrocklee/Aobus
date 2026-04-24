// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/QueryExpressionBox.h"

#include <gdk/gdk.h>

#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackStore.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>

namespace app::ui
{

  namespace
  {
    struct CompletionQuery
    {
      char trigger = '\0';
      int tokenStart = -1;
      std::string_view prefix;
    };

    constexpr auto kMetadataFields = std::array<std::string_view, 13>{
      "title",
      "artist",
      "album",
      "albumArtist",
      "composer",
      "work",
      "genre",
      "year",
      "trackNumber",
      "totalTracks",
      "discNumber",
      "totalDiscs",
      "coverArt",
    };

    constexpr auto kPropertyFields = std::array<std::string_view, 5>{
      "duration",
      "bitrate",
      "sampleRate",
      "channels",
      "bitDepth",
    };

    bool isIdentifierStart(char ch)
    {
      auto const uch = static_cast<unsigned char>(ch);
      return std::isalpha(uch) != 0 || ch == '_';
    }

    bool isIdentifierChar(char ch)
    {
      auto const uch = static_cast<unsigned char>(ch);
      return std::isalnum(uch) != 0 || ch == '_';
    }

    bool isQueryableIdentifier(std::string_view value)
    {
      if (value.empty() || !isIdentifierStart(value.front()))
      {
        return false;
      }

      return std::ranges::all_of(value, isIdentifierChar);
    }

    bool startsWithInsensitive(std::string_view candidate, std::string_view prefix)
    {
      if (prefix.size() > candidate.size())
      {
        return false;
      }

      return std::equal(
        prefix.begin(),
        prefix.end(),
        candidate.begin(),
        [](char lhs, char rhs)
        { return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs)); });
    }

    std::optional<CompletionQuery> completionQueryForCursor(std::string_view text, int cursor)
    {
      if (cursor <= 0 || cursor > static_cast<int>(text.size()))
      {
        return std::nullopt;
      }

      if (cursor < static_cast<int>(text.size()) && isIdentifierChar(text[static_cast<std::size_t>(cursor)]))
      {
        return std::nullopt;
      }

      auto tokenStart = cursor;

      while (tokenStart > 0 && isIdentifierChar(text[static_cast<std::size_t>(tokenStart - 1)]))
      {
        --tokenStart;
      }

      if (tokenStart <= 0)
      {
        return std::nullopt;
      }

      auto const trigger = text[static_cast<std::size_t>(tokenStart - 1)];

      if (trigger != '$' && trigger != '@' && trigger != '#' && trigger != '%')
      {
        return std::nullopt;
      }

      if (tokenStart > 1 && isIdentifierChar(text[static_cast<std::size_t>(tokenStart - 2)]))
      {
        return std::nullopt;
      }

      return CompletionQuery{
        .trigger = trigger,
        .tokenStart = tokenStart - 1,
        .prefix = text.substr(static_cast<std::size_t>(tokenStart), static_cast<std::size_t>(cursor - tokenStart)),
      };
    }
  }

  QueryExpressionBox::QueryExpressionBox(rs::core::MusicLibrary& musicLibrary)
    : Gtk::Box(Gtk::Orientation::VERTICAL), _musicLibrary(&musicLibrary)
  {
    _entry.set_hexpand(true);
    append(_entry);
    setupCompletion();
    refreshCompletionData();
  }

  QueryExpressionBox::~QueryExpressionBox()
  {
    _completionPopover.popdown();
  }

  void QueryExpressionBox::setupCompletion()
  {
    constexpr int kCompletionWidth = 260;
    constexpr int kCompletionHeight = 180;

    _completionItems = Gtk::StringList::create();
    _completionSelection = Gtk::SingleSelection::create(_completionItems);

    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        label->set_margin_start(8);
        label->set_margin_end(8);
        label->set_margin_top(4);
        label->set_margin_bottom(4);
        listItem->set_child(*label);
      });

    factory->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto item = listItem->get_item();
        auto stringObject = std::dynamic_pointer_cast<Gtk::StringObject>(item);

        if (auto label = dynamic_cast<Gtk::Label*>(listItem->get_child()); label != nullptr)
        {
          label->set_text(stringObject ? stringObject->get_string() : "");
        }
      });

    _completionListView.set_factory(factory);
    _completionListView.set_model(_completionSelection);
    _completionListView.set_single_click_activate(true);
    _completionListView.signal_activate().connect(
      [this](guint position)
      {
        if (!_completionItems || position >= _completionItems->get_n_items())
        {
          return;
        }

        _completionSelection->set_selected(position);
        applySelectedCompletion();
      });

    _completionScrolledWindow.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _completionScrolledWindow.set_min_content_width(kCompletionWidth);
    _completionScrolledWindow.set_min_content_height(kCompletionHeight);
    _completionScrolledWindow.set_child(_completionListView);

    _completionPopover.set_autohide(false);
    _completionPopover.set_has_arrow(false);
    _completionPopover.set_position(Gtk::PositionType::BOTTOM);
    _completionPopover.set_child(_completionScrolledWindow);
    _completionPopover.set_parent(_entry);

    _entry.signal_changed().connect(
      [this]
      {
        if (_suppressNextCompletionUpdate)
        {
          _suppressNextCompletionUpdate = false;
          return;
        }

        updateCompletion();
      });

    auto keyController = Gtk::EventControllerKey::create();
    keyController->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType)
      {
        if (!_completionPopover.get_visible())
        {
          if (keyval == GDK_KEY_Left || keyval == GDK_KEY_Right || keyval == GDK_KEY_Home || keyval == GDK_KEY_End)
          {
            hideCompletion();
          }

          return false;
        }

        switch (keyval)
        {
          case GDK_KEY_Up: return moveCompletionSelection(-1);
          case GDK_KEY_Down: return moveCompletionSelection(1);
          case GDK_KEY_Tab:
          case GDK_KEY_KP_Tab:
          case GDK_KEY_Return:
          case GDK_KEY_KP_Enter: applySelectedCompletion(); return true;
          case GDK_KEY_Escape: hideCompletion(); return true;
          case GDK_KEY_Left:
          case GDK_KEY_Right:
          case GDK_KEY_Home:
          case GDK_KEY_End: hideCompletion(); return false;
          default: return false;
        }
      },
      false);
    _entry.add_controller(keyController);

    auto clickController = Gtk::GestureClick::create();
    clickController->signal_released().connect([this](int, double, double) { updateCompletion(); });
    _entry.add_controller(clickController);
  }

  void QueryExpressionBox::updateCompletion()
  {
    auto const expr = std::string{_entry.get_text()};
    auto const cursor = _entry.get_position();
    auto const query = completionQueryForCursor(expr, cursor);

    if (!query.has_value())
    {
      hideCompletion();
      return;
    }

    auto suggestions = std::vector<Glib::ustring>{};
    auto const appendPrefixedMatches = [&](char trigger, std::span<std::string_view const> values)
    {
      for (auto const value : values)
      {
        if (startsWithInsensitive(value, query->prefix))
        {
          suggestions.emplace_back(std::string(1, trigger) + std::string(value));
        }
      }
    };

    switch (query->trigger)
    {
      case '$': appendPrefixedMatches('$', std::span{kMetadataFields}); break;
      case '@': appendPrefixedMatches('@', std::span{kPropertyFields}); break;
      case '#':
        for (auto const& tag : _availableTags)
        {
          if (startsWithInsensitive(tag, query->prefix))
          {
            suggestions.emplace_back("#" + tag);
          }
        }
        break;
      case '%':
        for (auto const& key : _availableCustomKeys)
        {
          if (startsWithInsensitive(key, query->prefix))
          {
            suggestions.emplace_back("%" + key);
          }
        }
        break;
      default: break;
    }

    if (suggestions.empty())
    {
      hideCompletion();
      return;
    }

    _completionTokenStart = query->tokenStart;
    _completionItems->splice(0, _completionItems->get_n_items(), suggestions);
    _completionSelection->set_selected(0);
    _completionListView.scroll_to(0);

    if (!_completionPopover.get_visible())
    {
      _completionPopover.popup();
    }
  }

  void QueryExpressionBox::hideCompletion()
  {
    _completionTokenStart = -1;

    if (_completionPopover.get_visible())
    {
      _completionPopover.popdown();
    }
  }

  void QueryExpressionBox::applySelectedCompletion()
  {
    if (_completionTokenStart < 0 || !_completionItems || _completionItems->get_n_items() == 0)
    {
      hideCompletion();
      return;
    }

    auto selected = _completionSelection ? _completionSelection->get_selected() : GTK_INVALID_LIST_POSITION;

    if (selected == GTK_INVALID_LIST_POSITION || selected >= _completionItems->get_n_items())
    {
      selected = 0;
    }

    auto const replacement = std::string{_completionItems->get_string(selected)};
    auto expr = std::string{_entry.get_text()};
    auto const cursor = _entry.get_position();
    expr.replace(static_cast<std::size_t>(_completionTokenStart),
                 static_cast<std::size_t>(cursor - _completionTokenStart),
                 replacement);

    _suppressNextCompletionUpdate = true;
    _entry.set_text(expr);
    _entry.set_position(_completionTokenStart + static_cast<int>(replacement.size()));
    hideCompletion();
  }

  bool QueryExpressionBox::moveCompletionSelection(int delta)
  {
    if (!_completionSelection || !_completionItems || _completionItems->get_n_items() == 0)
    {
      return false;
    }

    auto selected = _completionSelection->get_selected();
    auto const itemCount = static_cast<int>(_completionItems->get_n_items());
    auto const current = (selected == GTK_INVALID_LIST_POSITION) ? 0 : static_cast<int>(selected);
    auto const next = std::clamp(current + delta, 0, itemCount - 1);

    _completionSelection->set_selected(static_cast<guint>(next));
    _completionListView.scroll_to(static_cast<guint>(next));
    return true;
  }

  void QueryExpressionBox::refreshCompletionData()
  {
    auto uniqueTags = std::set<std::string>{};
    auto uniqueCustomKeys = std::set<std::string>{};
    auto txn = _musicLibrary->readTransaction();
    auto reader = _musicLibrary->tracks().reader(txn);
    auto const& dictionary = _musicLibrary->dictionary();

    for (auto iter = reader.begin(rs::core::TrackStore::Reader::LoadMode::Both),
              end = reader.end(rs::core::TrackStore::Reader::LoadMode::Both);
         iter != end;
         ++iter)
    {
      auto const& [_, view] = *iter;

      for (auto const tagId : view.tags())
      {
        if (auto const tag = dictionary.get(tagId); isQueryableIdentifier(tag))
        {
          uniqueTags.emplace(tag);
        }
      }

      for (auto const& [dictId, _] : view.custom())
      {
        if (auto const key = dictionary.get(dictId); isQueryableIdentifier(key))
        {
          uniqueCustomKeys.emplace(key);
        }
      }
    }

    _availableTags.assign(uniqueTags.begin(), uniqueTags.end());
    _availableCustomKeys.assign(uniqueCustomKeys.begin(), uniqueCustomKeys.end());
  }

} // namespace app::ui
