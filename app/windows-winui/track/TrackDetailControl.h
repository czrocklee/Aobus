// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/detail/TrackFieldGridSchema.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::winui
{
  class CoverArtPresenter;
  struct WinUiDependencies;

  struct TrackDetailControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer fieldScroll{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement detailContent{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::Button metadataHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock metadataHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon metadataChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel metadataRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button showEmptyButton{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::Button technicalHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock technicalHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon technicalChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel technicalRows{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer classicFieldScroll{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement classicDetailContent{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement classicMetadataSection{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button classicMetadataHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock classicMetadataHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon classicMetadataChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel classicMetadataRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button classicShowEmptyButton{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement classicTechnicalSection{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button classicTechnicalHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock classicTechnicalHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon classicTechnicalChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel classicTechnicalRows{nullptr};
  };

  /**
   * Read-only WinUI adapter for the shared focused-view track-detail projection.
   *
   * The runtime projection owns selection tracking and field aggregation. This
   * control owns only WinUI section state and renders the shared schema,
   * visibility, formatting, and cover-art policies into the supplied controls.
   */
  class TrackDetailControl final
  {
  public:
    explicit TrackDetailControl(TrackDetailControlConfig config);
    ~TrackDetailControl();

    TrackDetailControl(TrackDetailControl const&) = delete;
    TrackDetailControl& operator=(TrackDetailControl const&) = delete;
    TrackDetailControl(TrackDetailControl&&) = delete;
    TrackDetailControl& operator=(TrackDetailControl&&) = delete;

    void bind(WinUiDependencies const& dependencies);
    void unbind();

  private:
    void handleSnapshot(rt::TrackDetailSnapshot const& snapshot);
    void renderSnapshot();
    void renderMetadataRows(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& rows,
                            bool expanded,
                            bool showEmpty,
                            bool compact);
    void renderTechnicalRows(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& rows, bool compact);
    void updateSectionPresentation();
    void updateModernSectionPresentation(bool renderMetadataSection, bool renderTechnicalSection);
    void updateClassicSectionPresentation(bool renderMetadataSection,
                                          bool renderTechnicalSection,
                                          bool hasMetadataFields);
    void updateSelectionPresentation();
    void resetFieldScroll();

    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer _fieldScroll{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _detailContent{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::Button _metadataHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _metadataHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon _metadataChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _metadataRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _showEmptyButton{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::Button _technicalHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _technicalHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon _technicalChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _technicalRows{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer _classicFieldScroll{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _classicDetailContent{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _classicMetadataSection{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _classicMetadataHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _classicMetadataHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon _classicMetadataChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _classicMetadataRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _classicShowEmptyButton{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _classicTechnicalSection{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _classicTechnicalHeaderButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _classicTechnicalHeader{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon _classicTechnicalChevron{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _classicTechnicalRows{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _metadataHeaderClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _showEmptyClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _technicalHeaderClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _classicMetadataHeaderClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _classicShowEmptyClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _classicTechnicalHeaderClickRevoker{};

    uimodel::TrackFieldGridSchema _schema;
    rt::TrackDetailSnapshot _snapshot;
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    std::unique_ptr<rt::TrackDetailProjection> _projectionPtr;
    async::Subscription _subscription;
    CoverArtPresenter* _coverArt = nullptr;

    bool _metadataExpanded = true;
    bool _technicalExpanded = false;
    bool _showEmptyMetadata = false;
  };
} // namespace ao::winui
