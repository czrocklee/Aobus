// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackListAdapter.h"

#include "TrackRow.h"
#include "TrackRowDataProvider.h"

#include <ao/library/TrackStore.h>

#include <boost/algorithm/string.hpp>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

#include <algorithm>

namespace ao::gtk
{
  namespace
  {
    bool isQueryableIdentifier(std::string_view value)
    {
      if (value.empty())
      {
        return false;
      }

      auto const isIdentifierStart = [](char ch)
      {
        auto const uch = static_cast<unsigned char>(ch);
        return std::isalpha(uch) != 0 || ch == '_';
      };

      auto const isIdentifierChar = [](char ch)
      {
        auto const uch = static_cast<unsigned char>(ch);
        return std::isalnum(uch) != 0 || ch == '_';
      };

      if (!isIdentifierStart(value.front()))
      {
        return false;
      }

      return std::ranges::all_of(value, isIdentifierChar);
    }

    bool looksLikeExpression(std::string_view value)
    {
      return std::ranges::any_of(value,
                                 [](char ch)
                                 {
                                   switch (ch)
                                   {
                                     case '$':
                                     case '@':
                                     case '#':
                                     case '%':
                                     case '=':
                                     case '!':
                                     case '<':
                                     case '>':
                                     case '~':
                                     case '(':
                                     case ')':
                                     case '&':
                                     case '|':
                                     case '+': return true;
                                     default: return false;
                                   }
                                 });
    }

    std::vector<std::string> splitQuickTerms(std::string_view value)
    {
      auto result = std::vector<std::string>{};
      auto current = std::string{};
      char quote = '\0';

      for (auto const ch : value)
      {
        if (quote != '\0')
        {
          if (ch == quote)
          {
            quote = '\0';
          }
          else
          {
            current.push_back(ch);
          }

          continue;
        }

        if (ch == '\'' || ch == '"')
        {
          quote = ch;
          continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
          if (!current.empty())
          {
            result.push_back(std::move(current));
            current.clear();
          }

          continue;
        }

        current.push_back(ch);
      }

      if (!current.empty())
      {
        result.push_back(std::move(current));
      }

      return result;
    }

    std::string quoteExpressionString(std::string_view value)
    {
      if (!value.contains('"'))
      {
        return std::string{"\""} + std::string{value} + "\"";
      }

      if (!value.contains('\''))
      {
        return std::string{"'"} + std::string{value} + "'";
      }

      auto sanitized = std::string{value};
      std::ranges::replace(sanitized, '"', '\'');
      return std::string{"\""} + sanitized + "\"";
    }

    std::string buildQuickTermExpression(std::string_view term)
    {
      auto const quoted = quoteExpressionString(term);
      auto expression = std::format("($title ~ {0} or $artist ~ {0} or $album ~ {0} or $albumArtist ~ {0} or $genre ~ "
                                    "{0} or $composer ~ {0} or $work ~ {0})",
                                    quoted);

      if (isQueryableIdentifier(term))
      {
        expression.insert(expression.size() - 1, std::format(" or #{0}", term));
      }

      return expression;
    }

    std::optional<std::pair<TrackFilterMode, std::string>> resolveFilterExpression(std::string_view rawFilter)
    {
      auto const trimmed = boost::algorithm::trim_copy_if(std::string(rawFilter), boost::algorithm::is_space());

      if (trimmed.empty())
      {
        return std::nullopt;
      }

      if (looksLikeExpression(trimmed))
      {
        return std::pair{TrackFilterMode::Expression, trimmed};
      }

      auto const terms = splitQuickTerms(trimmed);

      if (terms.empty())
      {
        return std::nullopt;
      }

      auto expression = buildQuickTermExpression(terms.front());

      for (auto const& term : terms | std::views::drop(1))
      {
        expression = std::format("({}) and ({})", expression, buildQuickTermExpression(term));
      }

      return std::pair{TrackFilterMode::Quick, std::move(expression)};
    }
  }

  TrackListAdapter::TrackListAdapter(ao::model::TrackIdList& source,
                                     ao::library::MusicLibrary& musicLibrary,
                                     TrackRowDataProvider const& provider)
    : _source{source}, _musicLibrary{musicLibrary}, _provider{provider}, _listModel(Gio::ListStore<TrackRow>::create())
  {
    _source.attach(this);
  }

  TrackListAdapter::~TrackListAdapter()
  {
    _source.detach(this);
  }

  void TrackListAdapter::setFilter(Glib::ustring const& filterText)
  {
    _filterText = filterText;
    _filterMode = TrackFilterMode::None;
    _filterExpression.clear();
    _filterErrorMessage.clear();
    _filterPlan.reset();

    if (auto const optResolved = resolveFilterExpression(_filterText.raw()); optResolved)
    {
      _filterMode = optResolved->first;
      _filterExpression = optResolved->second;

      try
      {
        auto expr = ao::query::parse(_filterExpression);
        auto compiler = ao::query::QueryCompiler{&_musicLibrary.dictionary()};
        _filterPlan = std::make_unique<ao::query::ExecutionPlan>(compiler.compile(expr));
      }
      catch (std::exception const& e)
      {
        _filterErrorMessage = e.what();
        _filterPlan.reset();
      }
    }

    rebuildView();
  }

  void TrackListAdapter::createRowForTrack(TrackId id)
  {
    if (auto row = _provider.getTrackRow(id))
    {
      _listModel->append(row);
    }
  }

  void TrackListAdapter::rebuildView()
  {
    _listModel->remove_all();

    if (_filterMode != TrackFilterMode::None && (_filterPlan == nullptr || !_filterErrorMessage.empty()))
    {
      return;
    }

    auto txn = _musicLibrary.readTransaction();
    auto reader = _musicLibrary.tracks().reader(txn);

    for (std::size_t i = 0; i < _source.size(); ++i)
    {
      auto const id = _source.trackIdAt(i);

      if (_filterMode == TrackFilterMode::None || shouldIncludeTrack(id, reader))
      {
        createRowForTrack(id);
      }
    }
  }

  bool TrackListAdapter::shouldIncludeTrack(TrackId id, ao::library::TrackStore::Reader& reader) const
  {
    if (_filterPlan == nullptr)
    {
      return true;
    }

    auto const view = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!view)
    {
      return false;
    }

    return _filterEvaluator.matches(*_filterPlan, *view);
  }

  void TrackListAdapter::onReset()
  {
    rebuildView();
  }

  void TrackListAdapter::onInserted(TrackId id, std::size_t index)
  {
    // If filter is active, rebuild to recalculate positions

    if (_filterMode != TrackFilterMode::None)
    {
      rebuildView();
      return;
    }

    // Load and insert at position

    if (auto row = _provider.getTrackRow(id))
    {
      auto const uintIdx = static_cast<std::uint32_t>(index);

      if (uintIdx <= _listModel->get_n_items())
      {
        _listModel->insert(uintIdx, row);
      }
    }
  }

  void TrackListAdapter::onUpdated(TrackId id, std::size_t index)
  {
    // If filter is active, rebuild

    if (_filterMode != TrackFilterMode::None)
    {
      rebuildView();
      return;
    }

    // Update the row at position
    auto row = _provider.getTrackRow(id);

    auto const uintIdx = static_cast<std::uint32_t>(index);

    if (uintIdx >= _listModel->get_n_items())
    {
      return;
    }

    if (!row)
    {
      // Track was deleted or missing - remove it
      _listModel->remove(uintIdx);
      return;
    }

    std::vector<Glib::RefPtr<TrackRow>> additions;
    additions.push_back(row);
    _listModel->splice(uintIdx, 1, additions);
  }

  void TrackListAdapter::onRemoved(TrackId /*id*/, std::size_t index)
  {
    // If filter is active, rebuild

    if (_filterMode != TrackFilterMode::None)
    {
      rebuildView();
      return;
    }

    auto const uintIdx = static_cast<std::uint32_t>(index);

    if (uintIdx < _listModel->get_n_items())
    {
      _listModel->remove(uintIdx);
    }
  }

  void TrackListAdapter::onUpdated(std::span<TrackId const> ids)
  {
    // Optimization: If only one track is updated and no filter is active,
    // refresh only that row to preserve scroll position and UI state.
    if (ids.size() == 1 && _filterMode == TrackFilterMode::None)
    {
      if (auto const optIndex = _source.indexOf(ids[0]); optIndex)
      {
        onUpdated(ids[0], *optIndex);
        return;
      }
    }

    // For larger batch updates or when filters are active, rebuilding the entire view
    // is more efficient and ensures correct visibility/sorting.
    rebuildView();
  }

  void TrackListAdapter::onInserted(std::span<TrackId const> /*ids*/)
  {
    rebuildView();
  }

  void TrackListAdapter::onRemoved(std::span<TrackId const> /*ids*/)
  {
    rebuildView();
  }
} // namespace ao::gtk
