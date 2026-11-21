// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>
#include <ao/winui/track/TrackRevealAdapter.h>

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class Library;
  class TrackListProjection;
  class ViewService;
  class WorkspaceService;
  struct NavigationPresentation;
  struct TrackListProjectionDeltaBatch;
}

namespace ao::uimodel
{
  class ListPresentations;
}

namespace ao::winui
{
  struct TrackColumnCellSpec;

  /// Stable presentation id stamped when a column-header sort replaces the active
  /// presentation; the presentation label resolves it to localized copy.
  inline constexpr std::string_view kColumnSortPresentationId = "windows-column-sort";

  struct TrackColumnChoice final
  {
    rt::TrackField field = rt::TrackField::Title;
    bool visible = true;
  };

  class TrackListController final
  {
  public:
    TrackListController(rt::ViewService& views,
                        rt::WorkspaceService& workspace,
                        rt::Library& library,
                        uimodel::TrackColumnLayouts& columnLayouts,
                        uimodel::ListPresentations const& listPresentations,
                        i18n::MessageCatalog textCatalog);
    ~TrackListController();

    TrackListController(TrackListController const&) = delete;
    TrackListController& operator=(TrackListController const&) = delete;
    TrackListController(TrackListController&&) = delete;
    TrackListController& operator=(TrackListController&&) = delete;

    async::Signal<>& signalChanged() noexcept { return _changed; }
    void reload();
    void setViewportWidth(double width, double trailingChromeWidth);
    void publishSelection(std::span<TrackId const> trackIds);
    std::vector<TrackId> selection() const;
    std::optional<std::size_t> displayIndexOfTrack(TrackId trackId) const noexcept;
    Result<> play(TrackId trackId, std::function<Result<>(rt::ViewId, TrackId)> const& playTrack);
    Result<> toggleSort(rt::TrackSortField field);
    Result<> navigateTo(ListId listId);
    Result<> navigateTo(ListId listId, rt::NavigationPresentation presentation);
    Result<> revealTrack(TrackId trackId, rt::ViewId preferredViewId, ListId preferredListId);
    std::optional<TrackRevealTarget> revealTarget() const noexcept;
    Result<> resizeColumn(std::string_view fieldId, double horizontalChange);
    Result<> moveColumn(std::string_view fieldId, std::int32_t offset);
    Result<> setColumnVisible(std::string_view fieldId, bool visible);
    std::vector<TrackColumnChoice> columnChoices() const;
    ListId activeListId() const;
    std::string activePresentationId() const;

    winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable> items()
      const noexcept
    {
      return _items;
    }
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> headers()
      const noexcept
    {
      return _headers;
    }
    rt::ViewId viewId() const noexcept { return _viewId; }
    std::size_t rowCount() const noexcept;
    double contentWidth() const noexcept { return _contentWidth; }

  private:
    /// Establish empty native vectors before adopting the constructor-bound workspace's active view.
    void resetPresentation();

    static constexpr std::int32_t kDefaultViewportWidth = 1200;
    static constexpr double kDefaultContentWidth = 1200.0;

    void handleProjectionBatch(rt::TrackListProjectionDeltaBatch const& batch);
    void refreshRows();
    void refreshColumns();
    void adoptWorkspaceView(rt::ViewId viewId);
    void storeColumnSpecs(ListId listId, std::span<uimodel::TrackColumnSolveSpec const> specs);
    void resetProjection(std::shared_ptr<rt::TrackListProjection const> projectionPtr);
    i18n::MessageCatalog _textCatalog;
    rt::ViewService& _views;
    rt::WorkspaceService& _workspace;
    rt::Library& _library;
    uimodel::TrackColumnLayouts& _columnLayouts;
    uimodel::ListPresentations const& _listPresentations;
    rt::ViewId _viewId{rt::kInvalidViewId};
    std::shared_ptr<rt::TrackListProjection const> _projectionPtr;
    std::shared_ptr<void> _bindingLifetimePtr;
    bool _projectionInvalidated = false;
    winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable> _items{nullptr};
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> _headers;
    async::Subscription _projectionSub;
    async::Subscription _viewProjectionSub;
    async::Subscription _workspaceSub;
    uimodel::TrackDisplayIndex _displayIndex;
    TrackRevealIntent _revealIntent{};
    std::vector<TrackColumnCellSpec> _columns;
    std::int32_t _viewportWidth = kDefaultViewportWidth;
    std::int32_t _surfaceViewportWidth = kDefaultViewportWidth;
    std::int32_t _trailingChromeWidth = 0;
    double _contentWidth = kDefaultContentWidth;
    async::Signal<> _changed;
  };
} // namespace ao::winui
