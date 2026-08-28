// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/detail/TrackFieldGrid.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::winui
{
  /**
   * @brief What the detail control reads the focused selection through.
   *
   * The two shells reach it differently. A document-built inspector hands over
   * the projection its generation already shares and owns its cover as a
   * component of its own; the static shell has neither, so it lets the control
   * make a projection and drives the shell-lifetime cover presenter through it.
   */
  struct TrackDetailBinding final
  {
    /// The generation's shared projection, which every detail view of it reads.
    std::shared_ptr<rt::TrackDetailProjection> projectionPtr;
  };

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
    i18n::MessageCatalog textCatalog;
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

    void bind(TrackDetailBinding binding);
    void unbind() noexcept;

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void handleSnapshot(rt::TrackDetailSnapshot const& snapshot);
    void renderSnapshot();
    void renderMetadataRows(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& rows,
                            bool expanded,
                            bool showEmpty);
    void renderTechnicalRows(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& rows);
    void updateSectionPresentation();
    void applySectionPresentation(bool renderMetadataSection, bool renderTechnicalSection);
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

    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _metadataHeaderClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _showEmptyClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _technicalHeaderClickRevoker{};

    i18n::MessageCatalog _textCatalog;
    uimodel::TrackFieldGridSchema _schema;
    rt::TrackDetailSnapshot _snapshot;
    std::shared_ptr<rt::TrackDetailProjection> _projectionPtr;
    async::Subscription _subscription;

    bool _metadataExpanded = true;
    bool _technicalExpanded = false;
    bool _showEmptyMetadata = false;
  };
} // namespace ao::winui
