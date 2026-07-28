// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackRowItem.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
  class TrackListProjection;
  struct TrackListProjectionDeltaBatch;
}

namespace ao::winui
{
  struct TrackColumnChoice final
  {
    rt::TrackField field = rt::TrackField::Title;
    bool visible = true;
  };

  class TrackListController final
  {
  public:
    TrackListController();
    ~TrackListController();

    TrackListController(TrackListController const&) = delete;
    TrackListController& operator=(TrackListController const&) = delete;
    TrackListController(TrackListController&&) = delete;
    TrackListController& operator=(TrackListController&&) = delete;

    void bind(std::shared_ptr<rt::AppRuntime> runtimePtr, uimodel::TrackColumnLayoutState& columnLayouts);
    void setOnChanged(std::function<void()> onChanged) { _onChanged = std::move(onChanged); }
    void unbind();
    void reload();
    void setViewportWidth(double width, double trailingChromeWidth);
    void publishSelection(std::span<TrackId const> trackIds);
    Result<> play(TrackId trackId, std::function<Result<>(rt::ViewId, TrackId)> const& playTrack);
    Result<> selectPresentation(std::string_view presentationId);
    Result<> toggleSort(rt::TrackSortField field);
    Result<> navigateTo(ListId listId);
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
    static constexpr std::int32_t kDefaultViewportWidth = 1200;
    static constexpr double kDefaultContentWidth = 1200.0;

    void handleProjectionBatch(rt::TrackListProjectionDeltaBatch const& batch);
    void refreshRows();
    void refreshColumns();
    Result<> storeColumnSpecs(std::vector<uimodel::TrackColumnSolveSpec> const& specs);
    void resetProjection(std::shared_ptr<rt::TrackListProjection> projectionPtr);
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    rt::AppRuntime* _runtime = nullptr;
    uimodel::TrackColumnLayoutState* _columnLayouts = nullptr;
    rt::ViewId _viewId{rt::kInvalidViewId};
    std::shared_ptr<rt::TrackListProjection> _projectionPtr;
    bool _projectionInvalidated = false;
    winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable> _items{nullptr};
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> _headers;
    async::Subscription _projectionSub;
    async::Subscription _viewProjectionSub;
    std::vector<TrackColumnCellSpec> _columns;
    std::int32_t _viewportWidth = kDefaultViewportWidth;
    std::int32_t _surfaceViewportWidth = kDefaultViewportWidth;
    std::int32_t _trailingChromeWidth = 0;
    double _contentWidth = kDefaultContentWidth;
    std::function<void()> _onChanged;
  };
} // namespace ao::winui
