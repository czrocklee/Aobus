// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListController.h"

#include "platform/StringResources.h"
#include "platform/WinRtStringResources.h"
#include "track/TrackCellItem.h"
#include "track/TrackItemView.h"
#include "track/TrackRowItem.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/uimodel/library/track/IndexedTrackRowCache.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>
#include <ao/winui/track/TrackRevealAdapter.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    class TrackItemMaterializer final
    {
    public:
      TrackItemMaterializer(rt::Library const& inputLibrary,
                            rt::TrackListProjection const& inputProjection,
                            i18n::MessageCatalog textCatalog,
                            uimodel::TrackDisplayIndex displayIndex,
                            std::vector<TrackColumnCellSpec> columns,
                            std::weak_ptr<void> lifetimePtr)
        : _library{&inputLibrary}
        , _projection{&inputProjection}
        , _textCatalog{std::move(textCatalog)}
        , _displayIndex{std::move(displayIndex)}
        , _columns{std::move(columns)}
        , _lifetimePtr{std::move(lifetimePtr)}
      {
        auto const* const projection = _projection;
        auto const* const library = _library;
        auto const bindingPtr = _lifetimePtr;
        _rows.reset(projection->size(),
                    [projection, library, bindingPtr](std::size_t const index) -> std::optional<rt::TrackRow>
                    {
                      if (bindingPtr.expired() || index >= projection->size())
                      {
                        return std::nullopt;
                      }

                      return library->snapshot().trackRow(projection->trackIdAt(index));
                    });
      }

      winrt::Windows::Foundation::IInspectable itemAt(std::size_t const displayIndex)
      {
        auto const bindingPtr = _lifetimePtr.lock();

        if (!bindingPtr)
        {
          return nullptr;
        }

        auto const optItem = _displayIndex.itemAt(displayIndex);

        if (!optItem)
        {
          return nullptr;
        }

        if (optItem->kind == uimodel::TrackDisplayItemKind::GroupHeader)
        {
          auto const group = _projection->groupAt(optItem->groupIndex);
          auto heading = uimodel::formatTrackGroupHeading(_textCatalog, group.heading);
          auto optMonogram = uimodel::trackGroupCoverArtMonogram(group.heading);
          return winrt::make<winrt::Aobus::implementation::TrackRowItem>(
            static_cast<std::uint32_t>(displayIndex),
            static_cast<std::uint32_t>(optItem->sourceIndex),
            group.imageId.raw(),
            i18n::requiredFormat(_textCatalog, i18n::MessageId::TrackCount, {{"count", group.rows.count}}),
            std::move(heading.primaryText),
            std::move(heading.secondaryText),
            std::move(heading.tertiaryText),
            optMonogram ? std::move(*optMonogram) : std::string{});
        }

        auto const* row = _rows.rowAt(optItem->sourceIndex);

        if (row == nullptr)
        {
          return winrt::make<winrt::Aobus::implementation::TrackRowItem>(
            static_cast<std::uint32_t>(displayIndex), 0, 0, resourceHstring(L"winui_unavailable_track"), L"", L"", L"");
        }

        return winrt::make<winrt::Aobus::implementation::TrackRowItem>(static_cast<std::uint32_t>(displayIndex),
                                                                       static_cast<std::uint32_t>(optItem->sourceIndex),
                                                                       *row,
                                                                       _textCatalog,
                                                                       _columns);
      }

    private:
      rt::Library const* _library = nullptr;
      rt::TrackListProjection const* _projection = nullptr;
      i18n::MessageCatalog _textCatalog;
      uimodel::TrackDisplayIndex _displayIndex;
      std::vector<TrackColumnCellSpec> _columns;
      uimodel::IndexedTrackRowCache _rows;
      std::weak_ptr<void> _lifetimePtr;
    };

    // A header gesture can arrive after the stored layout changes but before the headers refresh.
    std::vector<uimodel::TrackColumnSolveSpec> visibleColumnSpecs(rt::TrackPresentationSpec const& presentation,
                                                                  std::span<uimodel::TrackColumnState const> stored)
    {
      auto const fields = uimodel::visibleTrackFieldsInStoredLayout(presentation.visibleFields, stored);
      return uimodel::pixelTrackColumnSpecs(fields, stored);
    }

    bool tryMaterializeStoredColumns(std::vector<uimodel::TrackColumnState>& stored,
                                     std::span<rt::TrackField const> const presentationFields)
    {
      auto order = std::vector<rt::TrackField>{};
      order.reserve(stored.size());

      for (auto const& column : stored)
      {
        order.push_back(column.field);
      }

      auto const presentationOrder = uimodel::visibleTrackFieldsInStoredOrder(presentationFields, order);
      auto const specs = uimodel::pixelTrackColumnSpecs(presentationOrder, stored);

      if (specs.empty())
      {
        return false;
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
      return true;
    }
  } // namespace

  TrackListController::TrackListController(rt::ViewService& views,
                                           rt::WorkspaceService& workspace,
                                           rt::Library& library,
                                           uimodel::TrackColumnLayouts& columnLayouts,
                                           uimodel::ListPresentations const& listPresentations,
                                           i18n::MessageCatalog textCatalog)
    : _textCatalog{std::move(textCatalog)}
    , _views{views}
    , _workspace{workspace}
    , _library{library}
    , _columnLayouts{columnLayouts}
    , _listPresentations{listPresentations}
    , _items{makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries)}
    , _headers{winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>()}
  {
    resetPresentation();
    _viewProjectionSub = _views.onProjectionChanged(
      [this](rt::TrackListProjectionChanged const& changed)
      {
        if (changed.viewId == _viewId)
        {
          adoptWorkspaceView(_viewId);
        }
      });
    _workspaceSub = _workspace.onChanged(
      [this](rt::WorkspaceChanged const&)
      {
        // A synchronous reveal may already have adopted a later commit.
        if (auto const active = _workspace.snapshot().activeViewId; active != _viewId)
        {
          adoptWorkspaceView(active);
        }
      });
    reload();
  }

  TrackListController::~TrackListController()
  {
    _bindingLifetimePtr.reset();
    _projectionSub.reset();
    _viewProjectionSub.reset();
    _workspaceSub.reset();
    _projectionPtr.reset();
    _displayIndex.clear();
    _columns.clear();
    _items = nullptr;
    _headers = nullptr;
  }

  void TrackListController::resetPresentation()
  {
    _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);
    _headers = winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>();
  }

  void TrackListController::reload()
  {
    if (auto const restoredView = _workspace.snapshot().activeViewId; restoredView != rt::kInvalidViewId)
    {
      adoptWorkspaceView(restoredView);
      return;
    }

    if (auto openedRes = navigateTo(rt::kAllTracksListId); !openedRes)
    {
      _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);
      _changed.emit();
    }
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

  void TrackListController::resetProjection(std::shared_ptr<rt::TrackListProjection const> projectionPtr)
  {
    _bindingLifetimePtr.reset();
    _projectionSub.reset();
    _projectionPtr = std::move(projectionPtr);
    _displayIndex.clear();
    _projectionInvalidated = false;
    _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);

    if (_projectionPtr == nullptr)
    {
      _columns.clear();
      _headers.Clear();
      _changed.emit();
      return;
    }

    _bindingLifetimePtr = std::make_shared<int>(0);
    _projectionSub = _projectionPtr->subscribe([this](rt::TrackListProjectionDeltaBatch const& batch)
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
      _bindingLifetimePtr.reset();
      _items = makeTrackItemView(0, {}, uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);
      _changed.emit();
      return;
    }

    refreshRows();
  }

  void TrackListController::refreshRows()
  {
    if (_projectionPtr == nullptr || _projectionInvalidated)
    {
      return;
    }

    refreshColumns();
    auto displayIndex = uimodel::TrackDisplayIndex{};

    if (!displayIndex.tryReset(*_projectionPtr))
    {
      std::ignore = displayIndex.tryReset(_projectionPtr->size(), {});
    }

    _displayIndex = displayIndex;
    auto const displayCount = displayIndex.displayCount();
    auto materializerPtr = std::make_shared<TrackItemMaterializer>(_library,
                                                                   *_projectionPtr,
                                                                   _textCatalog,
                                                                   std::move(displayIndex),
                                                                   _columns,
                                                                   std::weak_ptr<void>{_bindingLifetimePtr});
    _items = makeTrackItemView(
      displayCount,
      [materializerPtr = std::move(materializerPtr)](std::size_t const index)
      { return materializerPtr->itemAt(index); },
      uimodel::IndexedTrackRowCache::kDefaultMaximumEntries);

    _changed.emit();
  }

  void TrackListController::refreshColumns()
  {
    _columns.clear();
    _headers.Clear();
    _contentWidth = static_cast<double>(_surfaceViewportWidth);

    if (_viewId == rt::kInvalidViewId)
    {
      return;
    }

    auto const stateRes = _views.findTrackListState(_viewId);

    if (!stateRes)
    {
      return;
    }

    auto const& state = *stateRes;
    auto const listId = state.listId;
    auto const& storedLayout = _columnLayouts.layoutForList(listId);
    auto const specs = visibleColumnSpecs(state.presentation, storedLayout);
    auto const widths = uimodel::solveTrackColumnWidths(specs, _viewportWidth);
    _columns.reserve(specs.size());

    for (std::size_t index = 0; index < specs.size(); ++index)
    {
      auto const field = specs[index].field;
      auto const width = index < widths.size() ? static_cast<double>(widths[index])
                                               : static_cast<double>(uimodel::trackColumnDefaults(field).width);
      _columns.push_back({.field = field, .width = width});

      auto const* definition = rt::trackFieldDefinition(field);
      auto const fieldId = rt::trackFieldId(field);
      auto label = stableResourceString("track_field_", fieldId, uimodel::trackFieldLabel(_textCatalog, field));
      // The gripper names the column it widens, without the header's sort mark.
      auto const resizeName =
        i18n::requiredFormat(_textCatalog, i18n::MessageId::WinUiTrackResizeColumn, {{"column", label}});

      if (definition != nullptr && definition->optSortField && !state.presentation.sortBy.empty() &&
          state.presentation.sortBy.front().field == *definition->optSortField)
      {
        label +=
          resourceString(state.presentation.sortBy.front().ascending ? "SortAscendingSuffix" : "SortDescendingSuffix");
      }

      _headers.Append(
        winrt::make<winrt::Aobus::implementation::TrackCellItem>(winrt::to_hstring(label),
                                                                 winrt::to_hstring(std::string{fieldId}),
                                                                 width,
                                                                 definition != nullptr && definition->optSortField,
                                                                 winrt::to_hstring(resizeName)));
    }

    _contentWidth = 0.0;

    for (auto const& column : _columns)
    {
      _contentWidth += column.width;
    }

    _contentWidth =
      std::max(_contentWidth + static_cast<double>(_trailingChromeWidth), static_cast<double>(_surfaceViewportWidth));
  }

  void TrackListController::storeColumnSpecs(ListId const listId, std::span<uimodel::TrackColumnSolveSpec const> specs)
  {
    auto const& stored = _columnLayouts.layoutForList(listId);
    _columnLayouts.updateLayout(listId, uimodel::mergeVisibleTrackColumnSpecs(stored, specs));
    refreshRows();
  }

  Result<> TrackListController::resizeColumn(std::string_view const fieldId, double const horizontalChange)
  {
    auto const optField = rt::trackFieldFromId(fieldId);

    if (!optField)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoResizableColumn"));
    }

    auto const stateRes = _views.findTrackListState(_viewId);

    if (!stateRes || stateRes->listId == kInvalidListId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoListActive"));
    }

    auto const specs = visibleColumnSpecs(stateRes->presentation, _columnLayouts.layoutForList(stateRes->listId));
    auto const it = std::ranges::find(specs, *optField, &uimodel::TrackColumnSolveSpec::field);

    if (it == specs.end())
    {
      return makeError(Error::Code::NotFound, resourceString("ColumnHidden"));
    }

    auto const widths = uimodel::solveTrackColumnWidths(specs, _viewportWidth);
    auto const index = static_cast<std::size_t>(std::distance(specs.begin(), it));
    auto const target = static_cast<std::int32_t>(std::lround(static_cast<double>(widths[index]) + horizontalChange));
    storeColumnSpecs(stateRes->listId, uimodel::resizeTrackColumnSpecs(specs, *optField, target, _viewportWidth));
    return {};
  }

  Result<> TrackListController::moveColumn(std::string_view const fieldId, std::int32_t const offset)
  {
    auto const optField = rt::trackFieldFromId(fieldId);

    if (!optField || offset == 0)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoMovableColumn"));
    }

    auto const stateRes = _views.findTrackListState(_viewId);

    if (!stateRes || stateRes->listId == kInvalidListId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoListActive"));
    }

    auto specs = visibleColumnSpecs(stateRes->presentation, _columnLayouts.layoutForList(stateRes->listId));
    auto const it = std::ranges::find(specs, *optField, &uimodel::TrackColumnSolveSpec::field);

    if (it == specs.end())
    {
      return makeError(Error::Code::NotFound, resourceString("ColumnHidden"));
    }

    auto const target = std::distance(specs.begin(), it) + static_cast<std::ptrdiff_t>(offset);

    if (target < 0 || std::cmp_greater_equal(target, specs.size()))
    {
      return {};
    }

    std::iter_swap(it, specs.begin() + target);
    storeColumnSpecs(stateRes->listId, specs);
    return {};
  }

  std::vector<TrackColumnChoice> TrackListController::columnChoices() const
  {
    auto choices = std::vector<TrackColumnChoice>{};

    if (_viewId == rt::kInvalidViewId)
    {
      return choices;
    }

    auto const stateRes = _views.findTrackListState(_viewId);

    if (!stateRes)
    {
      return choices;
    }

    auto const& state = *stateRes;
    auto const& stored = _columnLayouts.layoutForList(state.listId);

    choices.reserve(state.presentation.visibleFields.size());

    for (auto const field : state.presentation.visibleFields)
    {
      bool visible = true;

      if (auto const it = std::ranges::find(stored, field, &uimodel::TrackColumnState::field); it != stored.end())
      {
        visible = it->visible;
      }

      choices.push_back({.field = field, .visible = visible});
    }

    return choices;
  }

  Result<> TrackListController::setColumnVisible(std::string_view const fieldId, bool const visible)
  {
    auto const optField = rt::trackFieldFromId(fieldId);

    if (!optField || _viewId == rt::kInvalidViewId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoConfigurableColumn"));
    }

    auto const stateRes = _views.findTrackListState(_viewId);

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    auto const& state = *stateRes;

    if (state.listId == kInvalidListId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoListActive"));
    }

    if (!std::ranges::contains(state.presentation.visibleFields, *optField))
    {
      return makeError(Error::Code::NotFound, resourceString("ColumnOutsidePresentation"));
    }

    auto stored = _columnLayouts.layoutForList(state.listId);
    auto existing = std::ranges::find(stored, *optField, &uimodel::TrackColumnState::field);
    auto const currentlyVisible = existing == stored.end() || existing->visible;

    if (currentlyVisible == visible)
    {
      return {};
    }

    if (!visible)
    {
      std::size_t visibleCount = 0;

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
      if (!tryMaterializeStoredColumns(stored, state.presentation.visibleFields))
      {
        return makeError(Error::Code::InvalidState, resourceString("ColumnLayoutPolicyMissing"));
      }

      existing = std::ranges::find(stored, *optField, &uimodel::TrackColumnState::field);

      if (existing == stored.end())
      {
        return makeError(Error::Code::InvalidState, resourceString("ColumnLayoutPolicyMissing"));
      }
    }

    existing->visible = visible;
    _columnLayouts.updateLayout(state.listId, stored);
    refreshRows();
    return {};
  }

  void TrackListController::publishSelection(std::span<TrackId const> const trackIds)
  {
    if (_viewId == rt::kInvalidViewId)
    {
      return;
    }

    auto selection = std::vector<TrackId>{trackIds.begin(), trackIds.end()};
    std::ignore = _views.setSelection(_viewId, std::move(selection));
    std::ignore = _workspace.focusView(_viewId);
  }

  std::vector<TrackId> TrackListController::selection() const
  {
    if (_viewId == rt::kInvalidViewId)
    {
      return {};
    }

    auto stateRes = _views.findTrackListState(_viewId);
    return stateRes ? std::move(stateRes->selection) : std::vector<TrackId>{};
  }

  std::optional<std::size_t> TrackListController::displayIndexOfTrack(TrackId const trackId) const noexcept
  {
    if (_projectionPtr == nullptr || _projectionInvalidated || trackId == kInvalidTrackId)
    {
      return std::nullopt;
    }

    auto const optSourceIndex = _projectionPtr->indexOf(trackId);
    return optSourceIndex ? _displayIndex.displayIndexOfSourceRow(*optSourceIndex) : std::nullopt;
  }

  Result<> TrackListController::play(TrackId const trackId,
                                     std::function<Result<>(rt::ViewId, TrackId)> const& playTrack)
  {
    if (_viewId == rt::kInvalidViewId || trackId == kInvalidTrackId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoPlayableTrackRow"));
    }

    return playTrack(_viewId, trackId);
  }

  Result<> TrackListController::toggleSort(rt::TrackSortField const field)
  {
    if (_viewId == rt::kInvalidViewId)
    {
      return makeError(Error::Code::InvalidState, resourceString("NoTrackViewActive"));
    }

    auto const stateRes = _views.findTrackListState(_viewId);

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    auto spec = stateRes->presentation;
    auto const ascending =
      spec.sortBy.empty() || spec.sortBy.front().field != field ? true : !spec.sortBy.front().ascending;
    spec.id = kColumnSortPresentationId;
    spec.sortBy = {{.field = field, .ascending = ascending}};
    return _views.setPresentation(_viewId, spec);
  }

  Result<> TrackListController::navigateTo(ListId const listId)
  {
    return navigateTo(listId,
                      {.mode = rt::NavigationPresentationMode::NewViewDefault,
                       .spec = _listPresentations.presentationForList(listId, _library)});
  }

  Result<> TrackListController::navigateTo(ListId const listId, rt::NavigationPresentation presentation)
  {
    auto viewRes = _workspace.navigate({.target = listId, .optPresentation = std::move(presentation)});

    if (!viewRes)
    {
      return std::unexpected{viewRes.error()};
    }

    if (_viewId != *viewRes)
    {
      adoptWorkspaceView(*viewRes);
    }

    return {};
  }

  void TrackListController::adoptWorkspaceView(rt::ViewId const viewId)
  {
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection const>{};

    if (viewId != rt::kInvalidViewId)
    {
      if (auto const projectionRes = _views.findTrackListProjection(viewId); projectionRes)
      {
        projectionPtr = *projectionRes;
      }
    }

    // Projection replacement is posted too. Bind the live identity, not an
    // obsolete event payload, and do not reset an already-revealed table.
    if (_viewId != viewId || _projectionPtr != projectionPtr)
    {
      _viewId = viewId;
      resetProjection(std::move(projectionPtr));
    }
  }

  Result<rt::ViewId> TrackListController::focusExplicitReveal(rt::PlaybackRevealTrackRequest const& request)
  {
    auto const preferredViewId = request.preferredViewId;
    auto const preferredListId = request.preferredListId;
    auto const snapshot = _workspace.snapshot();
    auto targetViewId = rt::kInvalidViewId;

    if (preferredViewId != rt::kInvalidViewId && std::ranges::contains(snapshot.openViews, preferredViewId) &&
        _views.findTrackListState(preferredViewId))
    {
      targetViewId = preferredViewId;
    }

    if (targetViewId == rt::kInvalidViewId && preferredListId != kInvalidListId)
    {
      for (auto const viewId : snapshot.openViews)
      {
        if (auto const stateRes = _views.findTrackListState(viewId); stateRes && stateRes->listId == preferredListId)
        {
          targetViewId = viewId;
          break;
        }
      }

      if (targetViewId == rt::kInvalidViewId)
      {
        if (auto openedRes = navigateTo(preferredListId); !openedRes)
        {
          return std::unexpected{openedRes.error()};
        }

        targetViewId = _viewId;
      }
    }

    if (targetViewId != rt::kInvalidViewId)
    {
      if (auto focusedRes = _workspace.focusView(targetViewId); !focusedRes)
      {
        return std::unexpected{focusedRes.error()};
      }
    }

    return targetViewId;
  }

  Result<> TrackListController::revealTrack(rt::PlaybackRevealTrackRequest const& request)
  {
    if (request.trackId == kInvalidTrackId)
    {
      return {};
    }

    auto const viewRes =
      request.fromPlaybackSource
        ? _workspace.navigateToTrackSource(request.trackId,
                                           request.preferredListId,
                                           [this](ListId listId)
                                           { return _listPresentations.presentationForList(listId, _library); })
        : focusExplicitReveal(request);

    if (!viewRes)
    {
      return std::unexpected{viewRes.error()};
    }

    auto const targetViewId = *viewRes;

    if (targetViewId == rt::kInvalidViewId)
    {
      return {};
    }

    adoptWorkspaceView(targetViewId);

    if (auto selectedRes = _views.setSelection(targetViewId, {request.trackId}); !selectedRes)
    {
      return selectedRes;
    }

    recordTrackRevealIntent(_revealIntent, targetViewId, request.trackId);
    _changed.emit();
    return {};
  }

  std::optional<TrackRevealTarget> TrackListController::revealTarget() const noexcept
  {
    if (_projectionPtr == nullptr || _projectionInvalidated)
    {
      return std::nullopt;
    }

    return resolveTrackRevealTarget(
      _revealIntent, _viewId, _projectionPtr->indexOf(_revealIntent.trackId), _displayIndex);
  }

  ListId TrackListController::activeListId() const
  {
    if (_viewId == rt::kInvalidViewId)
    {
      return kInvalidListId;
    }

    auto const stateRes = _views.findTrackListState(_viewId);
    return stateRes ? stateRes->listId : kInvalidListId;
  }

  std::string TrackListController::activePresentationId() const
  {
    if (_viewId == rt::kInvalidViewId)
    {
      return {};
    }

    auto const stateRes = _views.findTrackListState(_viewId);
    return stateRes ? stateRes->presentation.id : std::string{};
  }

  std::size_t TrackListController::rowCount() const noexcept
  {
    return _projectionPtr != nullptr && !_projectionInvalidated ? _projectionPtr->size() : 0;
  }
} // namespace ao::winui
