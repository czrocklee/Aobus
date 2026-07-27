// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListController.h"

#include "platform/WindowsStringResources.h"
#include "track/TrackCellItem.h"
#include "track/TrackItemView.h"
#include "track/TrackRowItem.h"
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/track/IndexedTrackRowCache.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::winui
{
  namespace
  {
    constexpr std::int32_t kMinimumColumnViewportWidth = 320;
    constexpr std::int32_t kMaximumColumnViewportWidth = 4096;
    constexpr std::int32_t kViewportResizeThreshold = 16;

    auto const kHeadingText = uimodel::PresentationTextCatalog{};

    class TrackItemMaterializer final
    {
    public:
      TrackItemMaterializer(std::shared_ptr<rt::AppRuntime> runtimePtr,
                            std::shared_ptr<rt::TrackListProjection> projectionPtr,
                            uimodel::TrackDisplayIndex displayIndex,
                            std::vector<TrackColumnCellSpec> columns)
        : _runtimePtr{std::move(runtimePtr)}
        , _projectionPtr{std::move(projectionPtr)}
        , _displayIndex{std::move(displayIndex)}
        , _columns{std::move(columns)}
      {
        auto const projection = _projectionPtr;
        auto const runtime = _runtimePtr;
        _rows.reset(projection->size(),
                    [projection, runtime](std::size_t const index) -> std::optional<rt::TrackRow>
                    {
                      if (index >= projection->size())
                      {
                        return std::nullopt;
                      }
                      return runtime->library().reader().trackRow(projection->trackIdAt(index));
                    });
      }

      winrt::Windows::Foundation::IInspectable itemAt(std::size_t const displayIndex)
      {
        auto const item = _displayIndex.itemAt(displayIndex);
        if (!item)
        {
          return nullptr;
        }

        if (item->kind == uimodel::TrackDisplayItemKind::GroupHeader)
        {
          auto const group = _projectionPtr->groupAt(item->groupIndex);
          auto const heading = uimodel::formatTrackGroupHeading(kHeadingText, group.heading);
          return winrt::make<winrt::Aobus::implementation::TrackRowItem>(static_cast<std::uint32_t>(displayIndex),
                                                                         static_cast<std::uint32_t>(item->sourceIndex),
                                                                         group.imageId.raw(),
                                                                         static_cast<std::uint32_t>(group.rows.count),
                                                                         heading.primaryText,
                                                                         heading.secondaryText,
                                                                         heading.tertiaryText);
        }

        auto const* row = _rows.rowAt(item->sourceIndex);
        if (row == nullptr)
        {
          return winrt::make<winrt::Aobus::implementation::TrackRowItem>(
            static_cast<std::uint32_t>(displayIndex), 0, 0, resourceHstring(L"UnavailableTrack"), L"", L"", L"");
        }

        return winrt::make<winrt::Aobus::implementation::TrackRowItem>(
          static_cast<std::uint32_t>(displayIndex), static_cast<std::uint32_t>(item->sourceIndex), *row, _columns);
      }

    private:
      std::shared_ptr<rt::AppRuntime> _runtimePtr;
      std::shared_ptr<rt::TrackListProjection> _projectionPtr;
      uimodel::TrackDisplayIndex _displayIndex;
      std::vector<TrackColumnCellSpec> _columns;
      uimodel::IndexedTrackRowCache _rows;
    };
  } // namespace

  TrackListController::TrackListController()
    : _items{makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries)}
    , _headers{winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>()}
  {
  }

  TrackListController::~TrackListController()
  {
    unbind();
  }

  void TrackListController::bind(std::shared_ptr<rt::AppRuntime> runtimePtr,
                                 uimodel::TrackColumnLayoutState& columnLayouts)
  {
    unbind();
    _runtimePtr = std::move(runtimePtr);
    _runtime = _runtimePtr.get();
    _columnLayouts = &columnLayouts;
    _viewProjectionSub = _runtime->views().onProjectionChanged(
      [this](rt::TrackListProjectionChanged const& changed) noexcept
      {
        if (changed.viewId == _viewId)
        {
          resetProjection(changed.projectionPtr);
        }
      });
    reload();
  }

  void TrackListController::unbind()
  {
    _projectionSub.reset();
    _viewProjectionSub.reset();
    _projectionPtr.reset();
    _projectionInvalidated = false;
    _columns.clear();
    _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);
    _headers.Clear();
    _viewId = rt::kInvalidViewId;
    _columnLayouts = nullptr;
    _runtime = nullptr;
    _runtimePtr.reset();
    if (_onChanged)
    {
      _onChanged();
    }
  }

  void TrackListController::reload()
  {
    if (_runtime == nullptr)
    {
      return;
    }

    _runtime->reloadAllTracks();
    auto const restoredView = _runtime->workspace().snapshot().activeViewId;
    if (restoredView != rt::kInvalidViewId)
    {
      _viewId = restoredView;
      auto const foundProjection = _runtime->views().findTrackListProjection(_viewId);
      resetProjection(foundProjection ? *foundProjection : nullptr);
      return;
    }

    auto view = _runtime->workspace().navigate({.target = rt::GlobalViewKind::AllTracks});

    if (!view)
    {
      _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);
      if (_onChanged)
      {
        _onChanged();
      }
      return;
    }

    _viewId = *view;
    auto const foundProjection = _runtime->views().findTrackListProjection(_viewId);
    resetProjection(foundProjection ? *foundProjection : nullptr);
  }

  void TrackListController::setViewportWidth(double const width, double const trailingChromeWidth)
  {
    if (!std::isfinite(width) || width <= 0.0 || !std::isfinite(trailingChromeWidth) || trailingChromeWidth < 0.0)
    {
      return;
    }

    auto const normalizedSurface = std::clamp(
      static_cast<std::int32_t>(std::lround(width)), kMinimumColumnViewportWidth, kMaximumColumnViewportWidth);
    auto const normalizedChrome =
      std::clamp(static_cast<std::int32_t>(std::lround(trailingChromeWidth)), 0, normalizedSurface - 1);
    auto const normalizedViewport = normalizedSurface - normalizedChrome;
    if (std::abs(normalizedSurface - _surfaceViewportWidth) < kViewportResizeThreshold &&
        normalizedChrome == _trailingChromeWidth)
    {
      return;
    }

    _surfaceViewportWidth = normalizedSurface;
    _trailingChromeWidth = normalizedChrome;
    _viewportWidth = normalizedViewport;
    refreshRows();
  }

  void TrackListController::resetProjection(std::shared_ptr<rt::TrackListProjection> projectionPtr)
  {
    _projectionSub.reset();
    _projectionPtr = std::move(projectionPtr);
    _projectionInvalidated = false;
    _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);

    if (_projectionPtr == nullptr || _runtime == nullptr)
    {
      _columns.clear();
      _headers.Clear();
      if (_onChanged)
      {
        _onChanged();
      }
      return;
    }

    _projectionSub = _projectionPtr->subscribe([this](rt::TrackListProjectionDeltaBatch const& batch) noexcept
                                               { handleProjectionBatch(batch); });
  }

  void TrackListController::handleProjectionBatch(rt::TrackListProjectionDeltaBatch const& batch)
  {
    if (batch.deltas.empty())
    {
      return;
    }

    if (std::holds_alternative<rt::ProjectionSourceInvalidated>(batch.deltas.front()))
    {
      // Keep the projection owner alive until a later reset/unbind. The callback can
      // run synchronously from subscribe(), so releasing the last owner here would
      // destroy the projection while one of its member functions is still active.
      _projectionInvalidated = true;
      _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);
      if (_onChanged)
      {
        _onChanged();
      }
      return;
    }

    refreshRows();
  }

  void TrackListController::refreshRows()
  {
    if (_projectionPtr == nullptr || _runtime == nullptr || _projectionInvalidated)
    {
      return;
    }

    refreshColumns();
    auto const rowCount = _projectionPtr->size();
    auto sections = std::vector<uimodel::TrackDisplaySection>{};
    sections.reserve(_projectionPtr->groupCount());
    for (auto groupIndex = std::size_t{0}; groupIndex < _projectionPtr->groupCount(); ++groupIndex)
    {
      auto const group = _projectionPtr->groupAt(groupIndex);
      sections.push_back({.start = group.rows.start, .count = group.rows.count});
    }
    auto displayIndex = uimodel::TrackDisplayIndex{};
    if (!displayIndex.reset(rowCount, sections))
    {
      std::ignore = displayIndex.reset(rowCount, {});
    }
    auto const displayCount = displayIndex.displayCount();
    auto materializer =
      std::make_shared<TrackItemMaterializer>(_runtimePtr, _projectionPtr, std::move(displayIndex), _columns);
    _items = makeTrackItemView(
      displayCount,
      [materializer = std::move(materializer)](std::size_t const index) { return materializer->itemAt(index); },
      uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);
    if (_onChanged)
    {
      _onChanged();
    }
  }

  void TrackListController::refreshColumns()
  {
    _columns.clear();
    _headers.Clear();
    _contentWidth = static_cast<double>(_surfaceViewportWidth);

    if (_runtime == nullptr || _viewId == rt::kInvalidViewId)
    {
      return;
    }

    auto const state = _runtime->views().trackListState(_viewId);
    auto const listId = state.listId;
    auto const* stored = static_cast<std::vector<uimodel::TrackColumnState> const*>(nullptr);
    if (_columnLayouts != nullptr)
    {
      if (auto const it = _columnLayouts->listLayouts.find(listId); it != _columnLayouts->listLayouts.end())
      {
        stored = &it->second;
      }
    }

    auto const empty = std::vector<uimodel::TrackColumnState>{};
    auto const& storedLayout = stored != nullptr ? *stored : empty;
    auto const fields = uimodel::visibleTrackFieldsInStoredLayout(state.presentation.visibleFields, storedLayout);
    auto const specs = uimodel::pixelTrackColumnSpecs(fields, storedLayout);
    auto const widths = uimodel::solveTrackColumnWidths(specs, _viewportWidth);
    auto const text = uimodel::PresentationTextCatalog{};

    _columns.reserve(fields.size());
    for (std::size_t index = 0; index < fields.size(); ++index)
    {
      auto const field = fields[index];
      auto const width = index < widths.size() ? static_cast<double>(widths[index])
                                               : static_cast<double>(uimodel::defaultTrackFieldColumnWidth(field));
      _columns.push_back({.field = field, .width = width});

      auto const* definition = rt::trackFieldDefinition(field);
      auto const fieldId = rt::trackFieldId(field);
      auto label = stableResourceString("TrackField_", fieldId, text.trackFieldLabel(field));
      if (definition != nullptr && definition->optSortField && !state.presentation.sortBy.empty() &&
          state.presentation.sortBy.front().field == *definition->optSortField)
      {
        label +=
          resourceString(state.presentation.sortBy.front().ascending ? "SortAscendingSuffix" : "SortDescendingSuffix");
      }
      _headers.Append(winrt::make<winrt::Aobus::implementation::TrackCellItem>(
        winrt::to_hstring(label),
        winrt::to_hstring(std::string{fieldId}),
        width,
        definition != nullptr && definition->optSortField.has_value()));
    }
    _contentWidth = 0.0;
    for (auto const& column : _columns)
    {
      _contentWidth += column.width;
    }
    _contentWidth =
      std::max(_contentWidth + static_cast<double>(_trailingChromeWidth), static_cast<double>(_surfaceViewportWidth));
  }

  Result<> TrackListController::storeColumnSpecs(std::vector<uimodel::TrackColumnSolveSpec> const& specs)
  {
    if (_columnLayouts == nullptr)
    {
      return makeError(Error::Code::InvalidState, resourceString("ColumnLayoutUnavailable"));
    }

    auto const listId = activeListId();
    if (listId == kInvalidListId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoListActive"));
    }

    auto& stored = _columnLayouts->listLayouts[listId];
    auto visibleLayout = std::vector<uimodel::TrackColumnState>{};
    visibleLayout.reserve(specs.size());
    for (auto const& spec : specs)
    {
      auto state = uimodel::canonicalTrackColumnState(spec);
      if (auto const prior = std::ranges::find(stored, spec.field, &uimodel::TrackColumnState::field);
          prior != stored.end())
      {
        state.visible = prior->visible;
      }
      visibleLayout.push_back(state);
    }
    stored = uimodel::mergeVisibleTrackColumnLayout(stored, visibleLayout);
    refreshRows();
    return {};
  }

  Result<> TrackListController::resizeColumn(std::string_view const fieldId, double const horizontalChange)
  {
    auto const field = rt::trackFieldFromId(fieldId);
    if (!field || _runtime == nullptr || _columnLayouts == nullptr)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoResizableColumn"));
    }

    auto fields = std::vector<rt::TrackField>{};
    fields.reserve(_columns.size());
    for (auto const& column : _columns)
    {
      fields.push_back(column.field);
    }
    auto const listId = activeListId();
    if (listId == kInvalidListId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoListActive"));
    }
    auto const& stored = _columnLayouts->listLayouts[listId];
    auto const specs = uimodel::pixelTrackColumnSpecs(fields, stored);
    auto const widths = uimodel::solveTrackColumnWidths(specs, _viewportWidth);
    auto const it = std::ranges::find(fields, *field);
    if (it == fields.end())
    {
      return makeError(Error::Code::NotFound, resourceString("ColumnHidden"));
    }
    auto const index = static_cast<std::size_t>(std::distance(fields.begin(), it));
    auto const target = static_cast<std::int32_t>(std::lround(static_cast<double>(widths[index]) + horizontalChange));
    return storeColumnSpecs(uimodel::resizeTrackColumnSpecs(specs, *field, target, _viewportWidth));
  }

  Result<> TrackListController::moveColumn(std::string_view const fieldId, int const offset)
  {
    auto const field = rt::trackFieldFromId(fieldId);
    if (!field || _runtime == nullptr || _columnLayouts == nullptr || offset == 0)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoMovableColumn"));
    }

    auto fields = std::vector<rt::TrackField>{};
    fields.reserve(_columns.size());
    for (auto const& column : _columns)
    {
      fields.push_back(column.field);
    }
    auto const it = std::ranges::find(fields, *field);
    if (it == fields.end())
    {
      return makeError(Error::Code::NotFound, resourceString("ColumnHidden"));
    }
    auto const index = static_cast<std::ptrdiff_t>(std::distance(fields.begin(), it));
    auto const target = index + static_cast<std::ptrdiff_t>(offset);
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(fields.size()))
    {
      return {};
    }
    std::iter_swap(fields.begin() + index, fields.begin() + target);

    auto const listId = activeListId();
    if (listId == kInvalidListId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoListActive"));
    }
    auto const& stored = _columnLayouts->listLayouts[listId];
    return storeColumnSpecs(uimodel::pixelTrackColumnSpecs(fields, stored));
  }

  std::vector<TrackColumnChoice> TrackListController::columnChoices() const
  {
    auto choices = std::vector<TrackColumnChoice>{};
    if (_runtime == nullptr || _viewId == rt::kInvalidViewId)
    {
      return choices;
    }

    auto const state = _runtime->views().trackListState(_viewId);
    auto const* stored = static_cast<std::vector<uimodel::TrackColumnState> const*>(nullptr);
    if (_columnLayouts != nullptr)
    {
      if (auto const it = _columnLayouts->listLayouts.find(state.listId); it != _columnLayouts->listLayouts.end())
      {
        stored = &it->second;
      }
    }

    choices.reserve(state.presentation.visibleFields.size());
    for (auto const field : state.presentation.visibleFields)
    {
      auto visible = true;
      if (stored != nullptr)
      {
        if (auto const it = std::ranges::find(*stored, field, &uimodel::TrackColumnState::field); it != stored->end())
        {
          visible = it->visible;
        }
      }
      choices.push_back({.field = field, .visible = visible});
    }
    return choices;
  }

  Result<> TrackListController::setColumnVisible(std::string_view const fieldId, bool const visible)
  {
    auto const field = rt::trackFieldFromId(fieldId);
    if (!field || _runtime == nullptr || _columnLayouts == nullptr || _viewId == rt::kInvalidViewId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoConfigurableColumn"));
    }

    auto const state = _runtime->views().trackListState(_viewId);
    if (!std::ranges::contains(state.presentation.visibleFields, *field))
    {
      return makeError(Error::Code::NotFound, resourceString("ColumnOutsidePresentation"));
    }

    auto& stored = _columnLayouts->listLayouts[state.listId];
    auto existing = std::ranges::find(stored, *field, &uimodel::TrackColumnState::field);
    auto const currentlyVisible = existing == stored.end() || existing->visible;
    if (currentlyVisible == visible)
    {
      return {};
    }

    if (!visible)
    {
      auto visibleCount = std::size_t{0};
      for (auto const choice : columnChoices())
      {
        visibleCount += choice.visible ? 1U : 0U;
      }
      if (visibleCount <= 1)
      {
        return makeError(Error::Code::InvalidState, resourceString("OneVisibleColumnRequired"));
      }
    }

    if (existing == stored.end())
    {
      auto order = std::vector<rt::TrackField>{};
      order.reserve(stored.size());
      for (auto const& column : stored)
      {
        order.push_back(column.field);
      }
      auto const presentationOrder = uimodel::visibleTrackFieldsInStoredOrder(state.presentation.visibleFields, order);
      auto const specs = uimodel::pixelTrackColumnSpecs(presentationOrder, stored);
      if (specs.empty())
      {
        return makeError(Error::Code::InvalidState, resourceString("ColumnLayoutPolicyMissing"));
      }

      auto complete = std::vector<uimodel::TrackColumnState>{};
      complete.reserve(stored.size() + specs.size());
      for (auto const& spec : specs)
      {
        auto column = uimodel::canonicalTrackColumnState(spec);
        if (auto const prior = std::ranges::find(stored, spec.field, &uimodel::TrackColumnState::field);
            prior != stored.end())
        {
          column.visible = prior->visible;
        }
        complete.push_back(column);
      }
      for (auto const& column : stored)
      {
        if (!std::ranges::contains(complete, column.field, &uimodel::TrackColumnState::field))
        {
          complete.push_back(column);
        }
      }
      stored = std::move(complete);
      existing = std::ranges::find(stored, *field, &uimodel::TrackColumnState::field);
      if (existing == stored.end())
      {
        return makeError(Error::Code::InvalidState, resourceString("ColumnLayoutPolicyMissing"));
      }
      existing->visible = visible;
    }
    else
    {
      existing->visible = visible;
    }

    refreshRows();
    return {};
  }

  void TrackListController::publishSelection(std::span<TrackId const> const trackIds)
  {
    if (_runtime == nullptr || _viewId == rt::kInvalidViewId)
    {
      return;
    }

    auto selection = std::vector<TrackId>{trackIds.begin(), trackIds.end()};
    std::ignore = _runtime->views().setSelection(_viewId, std::move(selection));
    std::ignore = _runtime->workspace().focusView(_viewId);
  }

  Result<> TrackListController::play(TrackId const trackId,
                                     std::function<Result<>(rt::ViewId, TrackId)> const& playTrack)
  {
    if (_runtime == nullptr || _viewId == rt::kInvalidViewId || trackId == kInvalidTrackId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoPlayableTrackRow"));
    }

    return playTrack(_viewId, trackId);
  }

  Result<> TrackListController::toggleSort(rt::TrackSortField const field)
  {
    if (_runtime == nullptr || _viewId == rt::kInvalidViewId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoTrackViewActive"));
    }

    auto spec = _runtime->views().trackListState(_viewId).presentation;
    auto const ascending =
      spec.sortBy.empty() || spec.sortBy.front().field != field ? true : !spec.sortBy.front().ascending;
    spec.id = "windows-column-sort";
    spec.sortBy = {{.field = field, .ascending = ascending}};
    return _runtime->views().setPresentation(_viewId, spec);
  }

  Result<> TrackListController::selectPresentation(std::string_view presentationId)
  {
    if (_runtime == nullptr || _viewId == rt::kInvalidViewId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoTrackViewActive"));
    }

    if (presentationId == "tracks" || presentationId == "folders" || presentationId == "playlists")
    {
      presentationId = rt::kDefaultTrackPresentationId;
    }

    auto const* preset = rt::builtinTrackPresentationPreset(presentationId);
    if (preset == nullptr)
    {
      return makeError(Error::Code::NotFound, formatResource("UnknownPresentationFormat", presentationId));
    }

    return _runtime->views().setPresentation(_viewId, preset->spec);
  }

  Result<> TrackListController::navigateTo(ListId const listId)
  {
    if (_runtime == nullptr)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoLibraryActive"));
    }

    auto request = rt::NavigationRequest{};
    request.target = listId == rt::kAllTracksListId ? rt::NavigationTarget{rt::GlobalViewKind::AllTracks}
                                                    : rt::NavigationTarget{listId};
    auto view = _runtime->workspace().navigate(request);
    if (!view)
    {
      return std::unexpected{view.error()};
    }

    _viewId = *view;
    auto const foundProjection = _runtime->views().findTrackListProjection(_viewId);
    resetProjection(foundProjection ? *foundProjection : nullptr);
    return {};
  }

  ListId TrackListController::activeListId() const
  {
    if (_runtime == nullptr || _viewId == rt::kInvalidViewId)
    {
      return kInvalidListId;
    }

    return _runtime->views().trackListState(_viewId).listId;
  }

  std::string TrackListController::activePresentationId() const
  {
    if (_runtime == nullptr || _viewId == rt::kInvalidViewId)
    {
      return {};
    }

    return _runtime->views().trackListState(_viewId).presentation.id;
  }

  std::size_t TrackListController::rowCount() const noexcept
  {
    return _projectionPtr != nullptr && !_projectionInvalidated ? _projectionPtr->size() : 0;
  }
} // namespace ao::winui
