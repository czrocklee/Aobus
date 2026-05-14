// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackListAdapter.h"

#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"

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
        return std::format("\"{}\"", value);
      }

      if (!value.contains('\''))
      {
        return std::format("'{}'", value);
      }

      auto sanitized = std::string{value};
      std::ranges::replace(sanitized, '"', '\'');
      return std::format("\"{}\"", sanitized);
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

      auto const optExpression = std::ranges::fold_left_first(terms | std::views::transform(buildQuickTermExpression),
                                                              [](auto const& acc, auto const& next)
                                                              { return std::format("({}) and ({})", acc, next); });

      return ResolvedTrackFilter{.mode = TrackFilterMode::Quick, .expression = optExpression.value_or("")};
    }
  } // namespace

  ResolvedTrackFilter TrackListAdapter::resolveFilterExpression(std::string_view rawFilter)
  {
    if (auto const optResolved = resolveFilter(rawFilter))
    {
      return *optResolved;
    }

    return ResolvedTrackFilter{};
  }

  TrackListAdapter::TrackListAdapter(rt::TrackSource& source,
                                     library::MusicLibrary& musicLibrary,
                                     TrackRowCache const& provider)
    : _source{source}
    , _musicLibrary{musicLibrary}
    , _provider{provider}
    , _listStore(Gio::ListStore<TrackRowObject>::create())
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

  void TrackListAdapter::bindProjection(std::shared_ptr<rt::ITrackListProjection> projection)
  {
    _rebuildConnection.disconnect();
    _projectionSub.reset();

    if (!_sourceDetachedForProjection)
    {
      _source.detach(this);
      _sourceDetachedForProjection = true;
    }

    auto const oldSize = static_cast<::guint>(_modelSize);
    auto const newSize = static_cast<::guint>(projection->size());

    _projection = std::move(projection);
    _projectionModel->setProjection(_projection.get(), _provider);
    _listModel = _projectionModel;
    _modelSize = newSize;

    // Fulfill GListModel contract: notify wrapper models (like SortListModel)
    // that the underlying dataset has been completely replaced.
    if (oldSize != newSize || oldSize > 0)
    {
      _projectionModel->notifyReset(oldSize, newSize);
    }

    _projectionSub = _projection->subscribe(std::bind_front(&TrackListAdapter::applyDeltaBatch, this));
    _signalModelChanged.emit();
  }

  void TrackListAdapter::applyDeltaBatch(rt::TrackListProjectionDeltaBatch const& batch)
  {
    auto const timer = utility::ScopedTimer{
      std::format("TrackListAdapter::applyDeltas ({} deltas, rev={})", batch.deltas.size(), batch.revision)};

    for (auto const& delta : batch.deltas)
    {
      std::visit(utility::makeVisitor([this](rt::ProjectionReset const&) { applyResetDelta(); },
                                      std::bind_front(&TrackListAdapter::applyInsertRange, this),
                                      std::bind_front(&TrackListAdapter::applyRemoveRange, this),
                                      std::bind_front(&TrackListAdapter::applyUpdateRange, this)),
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

  void TrackListAdapter::applyInsertRange(rt::ProjectionInsertRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _projectionModel->notifyInsert(pos, count);
    _modelSize += count;
  }

  void TrackListAdapter::applyRemoveRange(rt::ProjectionRemoveRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _projectionModel->notifyRemove(pos, count);
    _modelSize -= count;
  }

  void TrackListAdapter::applyUpdateRange(rt::ProjectionUpdateRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);

    for (auto const idx : std::views::iota(delta.range.start, delta.range.start + delta.range.count))
    {
      _provider.invalidate(_projection->trackIdAt(idx));
    }

    _projectionModel->notifyUpdate(pos, count);
  }

  std::optional<std::size_t> TrackListAdapter::indexOf(TrackId trackId) const noexcept
  {
    if (_projection != nullptr)
    {
      return _projection->indexOf(trackId);
    }

    return _source.indexOf(trackId);
  }

  std::optional<std::size_t> TrackListAdapter::groupIndexForTrack(TrackId trackId) const noexcept
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
      _listStore->remove(uintIdx);
      return;
    }

    if (!row)
    {
      _listStore->remove(uintIdx);
      return;
    }

    auto const additions = std::vector<Glib::RefPtr<TrackRowObject>>{row};
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
