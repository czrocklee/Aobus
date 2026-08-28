// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "image/CoverArtPresenter.h"
#include "layout/ShellBuilder.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include "track/TrackPropertiesCoordinator.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/uimodel/presentation/PresentationText.h>
#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Foundation.h>

#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace winrt::Aobus::implementation
{
  namespace
  {
    using ProjectedTrackRowItem = ::winrt::Aobus::TrackRowItem;

    std::string requiredColumnFieldId(Windows::Foundation::IInspectable const& sender)
    {
      // These tags are supplied by the shipped template or by showColumnsMenu;
      // absence is broken frontend wiring, not recoverable user input. The
      // fatal contract prevents continuing with the wrong field identity.
      auto const element = sender.try_as<Microsoft::UI::Xaml::FrameworkElement>();

      AO_INVARIANT(element, "MainWindow column field-id binding requires a FrameworkElement sender");

      auto const tag = element.Tag().try_as<Windows::Foundation::IPropertyValue>();

      AO_INVARIANT(tag && tag.Type() == Windows::Foundation::PropertyType::String,
                   "MainWindow column field-id binding requires a string Tag");

      auto fieldId = to_string(tag.GetString());
      AO_INVARIANT(!fieldId.empty(), "MainWindow column field-id binding requires a non-empty Tag");

      return fieldId;
    }
  } // namespace

  void MainWindow::reconcileLibrary()
  {
    if (_session == nullptr || _trackListPtr == nullptr)
    {
      return;
    }

    if (auto const listId = _trackListPtr->activeListId(); _session->listPresentations().presentationIdForList(listId))
    {
      if (auto const selectedRes = _trackListPtr->selectPresentation(_session->presentationForList(listId));
          !selectedRes)
      {
        updateStatus(ao::winui::formatResource("winui_presentation_failed", selectedRes.error().message));
      }
    }
  }

  void MainWindow::revealCurrentTrack()
  {
    if (_session != nullptr)
    {
      _session->runtime().playback().commands().revealPlayingTrack();
    }
  }

  void MainWindow::OnTrackPropertiesInvoked(Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
                                            Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
  {
    args.Handled(_shellBuilderPtr && _shellBuilderPtr->invokeAction("track.presentProperties"));
  }

  void MainWindow::presentTrackProperties()
  {
    if (_session == nullptr || _trackListPtr == nullptr || modalWorkflowActive())
    {
      return;
    }

    _trackPropertiesCoordinatorPtr.reset();
    auto& runtime = _session->runtime();
    auto stateRes = runtime.views().findTrackListState(_trackListPtr->viewId());

    if (!stateRes || !ao::winui::canPresentTrackProperties(stateRes->selection))
    {
      return;
    }

    try
    {
      auto dialogPtr =
        std::make_unique<ao::winui::TrackPropertiesCoordinator>(ao::winui::TrackPropertiesCoordinatorConfig{
          .xamlRoot = RootGrid().XamlRoot(),
          .asyncRuntime = runtime.async(),
          .library = runtime.library(),
          .workspace = runtime.workspace(),
          .completion = runtime.completion(),
          .textCatalog = _session->textCatalog(),
          .trackIds = std::move(stateRes->selection),
        });

      if (auto presentedRes = dialogPtr->present(); !presentedRes)
      {
        updateStatus(ao::winui::formatResource("winui_error", presentedRes.error().message));
        return;
      }

      _trackPropertiesCoordinatorPtr = std::move(dialogPtr);
    }
    catch (winrt::hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("winui_error", winrt::to_string(error.message())));
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "WinUI track-properties presentation");
    }
  }

  void MainWindow::OnGroupCoverLoaded(Windows::Foundation::IInspectable const& sender,
                                      Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    refreshGroupCoverPresenter(sender);
  }

  void MainWindow::OnGroupCoverDataContextChanged(Microsoft::UI::Xaml::FrameworkElement const& sender,
                                                  Microsoft::UI::Xaml::DataContextChangedEventArgs const& /*args*/)
  {
    refreshGroupCoverPresenter(sender);
  }

  void MainWindow::refreshGroupCoverPresenter(Windows::Foundation::IInspectable const& sender)
  {
    auto const tile = sender.try_as<Microsoft::UI::Xaml::Controls::Grid>();

    if (!tile)
    {
      return;
    }

    auto const* key = static_cast<void const*>(winrt::get_unknown(tile));
    auto presenterIt = _groupCoverPresenters.find(key);

    if (presenterIt != _groupCoverPresenters.end())
    {
      if (auto const retainedTile = presenterIt->second.tile.get();
          !retainedTile || winrt::get_unknown(retainedTile) != winrt::get_unknown(tile))
      {
        _groupCoverPresenters.erase(presenterIt);
        presenterIt = _groupCoverPresenters.end();
      }
    }

    auto const row = tile.DataContext().try_as<ProjectedTrackRowItem>();

    if (!row || !row.IsGroupHeader() || _resourceBytesPtr == nullptr || _themePtr == nullptr || _session == nullptr ||
        tile.Children().Size() < 2)
    {
      if (presenterIt != _groupCoverPresenters.end())
      {
        presenterIt->second.presenterPtr->select(
          ao::kInvalidResourceId, ao::uimodel::CoverArtPlaceholderIdentity{}, false);
      }

      return;
    }

    auto const placeholder = tile.Children().GetAt(0).try_as<Microsoft::UI::Xaml::Controls::Grid>();
    auto const image = tile.Children().GetAt(1).try_as<Microsoft::UI::Xaml::Controls::Image>();

    if (!placeholder || !image)
    {
      if (presenterIt != _groupCoverPresenters.end())
      {
        presenterIt->second.presenterPtr->select(
          ao::kInvalidResourceId, ao::uimodel::CoverArtPlaceholderIdentity{}, false);
      }

      return;
    }

    if (presenterIt == _groupCoverPresenters.end())
    {
      auto presenterPtr = std::make_unique<ao::winui::CoverArtPresenter>(
        image,
        placeholder,
        *_resourceBytesPtr,
        *_themePtr,
        ao::uimodel::defaultCoverArtPlaceholderStyle(ao::uimodel::CoverArtPlaceholderSlot::GroupHeading));
      presenterPtr->bind(_session->runtime().async());
      presenterIt = _groupCoverPresenters
                      .emplace(key,
                               GroupCoverPresenterEntry{
                                 .tile = winrt::make_weak(tile),
                                 .presenterPtr = std::move(presenterPtr),
                               })
                      .first;
    }

    auto identity = ao::uimodel::CoverArtPlaceholderIdentity{
      .primaryText = winrt::to_string(row.Title()),
    };

    if (auto monogram = winrt::to_string(row.CoverArtMonogram()); !monogram.empty())
    {
      identity.optMonogram = std::move(monogram);
    }

    presenterIt->second.presenterPtr->select(ao::ResourceId{row.CoverArtId()}, std::move(identity), true);
  }

  void MainWindow::OnGroupCoverUnloaded(Windows::Foundation::IInspectable const& sender,
                                        Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (auto const tile = sender.try_as<Microsoft::UI::Xaml::Controls::Grid>(); tile)
    {
      _groupCoverPresenters.erase(static_cast<void const*>(winrt::get_unknown(tile)));
    }
  }

  void MainWindow::clearGroupCoverPresenters() noexcept
  {
    _groupCoverPresenters.clear();
  }

  void MainWindow::toggleInspector()
  {
    // Asked against what the user can see rather than against the last request,
    // so the command reads the same at every width: the first press on a shell
    // nobody has asked anything of hides an inline inspector and reveals an
    // overlay, which is what each of them is showing at that moment.
    if (!_shellBuilderPtr)
    {
      return;
    }

    auto const width = RootGrid().ActualWidth();
    _optInspectorRequest = !_shellBuilderPtr->shellState().inspectorRevealed;
    applyShellState(width);
  }

  void MainWindow::OnColumnHeaderClicked(Windows::Foundation::IInspectable const& sender,
                                         Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    executeSort(requiredColumnFieldId(sender));
  }

  void MainWindow::OnColumnResizeCompleted(
    Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const& args)
  {
    constexpr double kMinimumColumnResize = 0.5;

    if (_session == nullptr || _trackListPtr == nullptr || args.Canceled() ||
        std::abs(args.HorizontalChange()) < kMinimumColumnResize)
    {
      return;
    }

    auto const fieldId = requiredColumnFieldId(sender);

    if (auto resizedRes = _trackListPtr->resizeColumn(fieldId, args.HorizontalChange()); !resizedRes)
    {
      updateStatus(ao::winui::formatResource("winui_column_resize_failed", resizedRes.error().message));
      return;
    }

    if (auto savedRes = _session->saveSettings(); !savedRes)
    {
      updateStatus(ao::winui::formatResource("winui_column_settings_failed", savedRes.error().message));
    }
  }

  void MainWindow::OnColumnMoveLeftClicked(Windows::Foundation::IInspectable const& sender,
                                           Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    moveColumn(sender, -1);
  }

  void MainWindow::OnColumnMoveRightClicked(Windows::Foundation::IInspectable const& sender,
                                            Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    moveColumn(sender, 1);
  }

  void MainWindow::moveColumn(Windows::Foundation::IInspectable const& sender, std::int32_t const offset)
  {
    if (_session == nullptr || _trackListPtr == nullptr)
    {
      return;
    }

    auto const fieldId = requiredColumnFieldId(sender);

    if (auto movedRes = _trackListPtr->moveColumn(fieldId, offset); !movedRes)
    {
      updateStatus(ao::winui::formatResource("winui_column_move_failed", movedRes.error().message));
      return;
    }

    if (auto savedRes = _session->saveSettings(); !savedRes)
    {
      updateStatus(ao::winui::formatResource("winui_column_settings_failed", savedRes.error().message));
    }
  }

  void MainWindow::showColumnsMenu()
  {
    if (_trackListPtr == nullptr)
    {
      return;
    }

    auto flyout = Microsoft::UI::Xaml::Controls::MenuFlyout{};
    auto const& text = _session->textCatalog();
    auto weak = get_weak();

    for (auto const& choice : _trackListPtr->columnChoices())
    {
      auto item = Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem{};
      auto const fieldId = ao::rt::trackFieldId(choice.field);
      item.Text(to_hstring(
        ao::winui::stableResourceString("track_field_", fieldId, ao::uimodel::trackFieldLabel(text, choice.field))));
      item.IsChecked(choice.visible);
      item.Tag(box_value(to_hstring(std::string{fieldId})));
      item.Click(
        [weak](Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
        {
          auto self = weak.get();
          auto toggle = sender.try_as<Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem>();

          if (!self || !toggle || !self->_trackListPtr)
          {
            return;
          }

          auto const fieldId = requiredColumnFieldId(sender);
          auto changedRes = self->_trackListPtr->setColumnVisible(fieldId, toggle.IsChecked());

          if (!changedRes)
          {
            toggle.IsChecked(!toggle.IsChecked());
            self->updateStatus(ao::winui::formatResource("winui_column_visibility_failed", changedRes.error().message));
            return;
          }

          if (self->_session != nullptr)
          {
            if (auto savedRes = self->_session->saveSettings(); !savedRes)
            {
              self->updateStatus(ao::winui::formatResource("winui_column_settings_failed", savedRes.error().message));
            }
          }
        });
      flyout.Items().Append(item);
    }

    // The menu is raised by a command rather than by a widget, so it anchors on
    // the host: the frame has no element of a generation it does not own.
    flyout.ShowAt(ShellLayoutHost());
  }

  void MainWindow::executeSort(std::string const& columnId)
  {
    if (_session == nullptr || _trackListPtr == nullptr)
    {
      return;
    }

    auto const optField = ao::rt::trackFieldFromId(columnId);
    auto const* definition = optField ? ao::rt::trackFieldDefinition(*optField) : nullptr;

    if (definition == nullptr || !definition->optSortField)
    {
      updateStatus(ao::winui::formatResource("winui_column_not_sortable", columnId));
      return;
    }

    auto sortedRes = _trackListPtr->toggleSort(*definition->optSortField);

    if (!sortedRes)
    {
      updateStatus(ao::winui::formatResource("winui_sort_failed", sortedRes.error().message));
      return;
    }

    if (auto savedRes = _session->saveSettings(); !savedRes)
    {
      updateStatus(ao::winui::formatResource("winui_column_settings_failed", savedRes.error().message));
    }
  }
} // namespace winrt::Aobus::implementation
