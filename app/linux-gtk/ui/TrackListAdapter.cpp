// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackListAdapter.h"

#include "TrackRow.h"
#include "TrackRowDataProvider.h"

#include <ao/library/TrackStore.h>
#include <ao/utility/ScopedTimer.h>
#include <ao/utility/VariantVisitor.h>

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

    std::optional<ResolvedTrackFilter> resolveFilter(std::string_view rawFilter)
    {
      auto const trimmed = boost::algorithm::trim_copy_if(std::string(rawFilter), boost::algorithm::is_space());

      if (trimmed.empty())
      {
        return std::nullopt;
      }

      if (looksLikeExpression(trimmed))
      {
        return ResolvedTrackFilter{.mode = TrackFilterMode::Expression, .expression = trimmed};
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

      return ResolvedTrackFilter{.mode = TrackFilterMode::Quick, .expression = std::move(expression)};
    }
  }

  ResolvedTrackFilter TrackListAdapter::resolveFilterExpression(std::string_view rawFilter)
  {
    if (auto const optResolved = resolveFilter(rawFilter))
    {
      return *optResolved;
    }

    return ResolvedTrackFilter{};
  }

  TrackListAdapter::TrackListAdapter(ao::rt::TrackSource& source,
                                     ao::library::MusicLibrary& musicLibrary,
                                     TrackRowDataProvider const& provider)
    : _source{source}
    , _musicLibrary{musicLibrary}
    , _provider{provider}
    , _listStore(Gio::ListStore<TrackRow>::create())
    , _projectionModel(ProjectionTrackModel::create())
    , _listModel(_listStore)
  {
    _source.attach(this);
  }

  TrackListAdapter::~TrackListAdapter()
  {
    _rebuildConnection.disconnect();
    _projectionSub.reset();

    if (!_sourceDetachedForProjection)
    {
      _source.detach(this);
    }
  }

  void TrackListAdapter::bindProjection(ao::rt::ITrackListProjection& projection)
  {
    _rebuildConnection.disconnect();
    _projectionSub.reset();

    if (!_sourceDetachedForProjection)
    {
      _source.detach(this);
      _sourceDetachedForProjection = true;
    }

    _projection = &projection;
    _projectionModel->setProjection(&projection, _provider);
    _listModel = _projectionModel;
    _modelSize = projection.size();

    _projectionSub =
      projection.subscribe([this](ao::rt::TrackListProjectionDeltaBatch const& batch) { applyDeltaBatch(batch); });
    _signalModelChanged.emit();
  }

  void TrackListAdapter::applyDeltaBatch(ao::rt::TrackListProjectionDeltaBatch const& batch)
  {
    auto const timer = ao::utility::ScopedTimer{
      std::format("TrackListAdapter::applyDeltas ({} deltas, rev={})", batch.deltas.size(), batch.revision)};

    for (auto const& delta : batch.deltas)
    {
      std::visit(
        ao::utility::makeVisitor([this](ao::rt::ProjectionReset const&) { applyResetDelta(); },
                                 [this](ao::rt::ProjectionInsertRange const& delta) { applyInsertRange(delta); },
                                 [this](ao::rt::ProjectionRemoveRange const& delta) { applyRemoveRange(delta); },
                                 [this](ao::rt::ProjectionUpdateRange const& delta) { applyUpdateRange(delta); }),
        delta);
    }

    gsl_Expects(_modelSize == _projection->size());
  }

  void TrackListAdapter::applyResetDelta()
  {
    auto const newSize = static_cast<::guint>(_projection->size());
    auto const oldSize = static_cast<::guint>(_modelSize);
    _projectionModel->notifyReset(oldSize, newSize);
    _modelSize = newSize;
  }

  void TrackListAdapter::applyInsertRange(ao::rt::ProjectionInsertRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _projectionModel->notifyInsert(pos, count);
    _modelSize += count;
  }

  void TrackListAdapter::applyRemoveRange(ao::rt::ProjectionRemoveRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _projectionModel->notifyRemove(pos, count);
    _modelSize -= count;
  }

  void TrackListAdapter::applyUpdateRange(ao::rt::ProjectionUpdateRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);

    for (auto const idx : std::views::iota(delta.range.start, delta.range.start + delta.range.count))
    {
      _provider.invalidate(_projection->trackIdAt(idx));
    }

    _projectionModel->notifyUpdate(pos, count);
  }

  std::optional<std::size_t> TrackListAdapter::indexOf(TrackId trackId) const
  {
    if (_projection != nullptr)
    {
      return _projection->indexOf(trackId);
    }

    return _source.indexOf(trackId);
  }

  std::optional<std::size_t> TrackListAdapter::groupIndexForTrack(TrackId trackId) const
  {
    if (_projection == nullptr)
    {
      return std::nullopt;
    }

    if (auto const idx = _projection->indexOf(trackId))
    {
      return _projection->groupIndexAt(*idx);
    }

    return std::nullopt;
  }

  void TrackListAdapter::createRowForTrack(TrackId id)
  {
    if (auto row = _provider.getTrackRow(id))
    {
      _listStore->append(row);
    }
  }

  void TrackListAdapter::rebuildView()
  {
    if (_rebuildConnection)
    {
      return;
    }

    _rebuildConnection = Glib::signal_idle().connect(
      [this]
      {
        rebuildViewInternal();
        return false; // Run once
      });
  }

  void TrackListAdapter::rebuildViewInternal()
  {
    _rebuildConnection.disconnect();
    _listStore->remove_all();

    for (auto const idx : std::views::iota(0UZ, _source.size()))
    {
      createRowForTrack(_source.trackIdAt(idx));
    }
  }

  void TrackListAdapter::onReset()
  {
    rebuildView();
  }

  void TrackListAdapter::onInserted(TrackId id, std::size_t index)
  {
    if (auto const row = _provider.getTrackRow(id))
    {
      auto const uintIdx = static_cast<std::uint32_t>(index);

      if (uintIdx <= _listStore->get_n_items())
      {
        _listStore->insert(uintIdx, row);
      }
    }
  }

  void TrackListAdapter::onUpdated(TrackId id, std::size_t index)
  {
    auto const row = _provider.getTrackRow(id);

    auto const uintIdx = static_cast<std::uint32_t>(index);

    if (uintIdx >= _listStore->get_n_items())
    {
      return;
    }

    if (!row)
    {
      _listStore->remove(uintIdx);
      return;
    }

    auto additions = std::vector<Glib::RefPtr<TrackRow>>{};
    additions.push_back(row);
    _listStore->splice(uintIdx, 1, additions);
  }

  void TrackListAdapter::onRemoved(TrackId /*id*/, std::size_t index)
  {
    auto const uintIdx = static_cast<std::uint32_t>(index);

    if (uintIdx < _listStore->get_n_items())
    {
      _listStore->remove(uintIdx);
    }
  }

  void TrackListAdapter::onUpdated(std::span<TrackId const> ids)
  {
    if (ids.size() == 1)
    {
      if (auto const optIndex = _source.indexOf(ids[0]); optIndex)
      {
        onUpdated(ids[0], *optIndex);
        return;
      }
    }

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
