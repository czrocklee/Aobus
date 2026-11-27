// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "image/GroupCoverPresenters.h"
#include "layout/ShellBuilder.h"
#include "platform/StringResourceFormat.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include <ao/Contract.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace winrt::Aobus::implementation
{
  namespace
  {
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

  void MainWindow::revealCurrentTrack()
  {
    if (_session != nullptr)
    {
      _session->runtime().playback().commands().revealPlayingTrack();
    }
  }

  bool MainWindow::tryInvokeShellAction(std::string_view const actionId) const
  {
    return _shellBuilderPtr && _shellBuilderPtr->tryInvokeAction(actionId);
  }

  void MainWindow::handleTrackPropertiesInvoked(
    Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args) const
  {
    args.Handled(tryInvokeShellAction("track.presentProperties"));
  }

  void MainWindow::handleGroupCoverLoaded(Windows::Foundation::IInspectable const& sender,
                                          Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_groupCoverPresentersPtr && _sessionPhase != SessionPhase::Retired)
    {
      _groupCoverPresentersPtr->refresh(sender);
    }
  }

  void MainWindow::handleGroupCoverDataContextChanged(Microsoft::UI::Xaml::FrameworkElement const& sender,
                                                      Microsoft::UI::Xaml::DataContextChangedEventArgs const& /*args*/)
  {
    if (_groupCoverPresentersPtr && _sessionPhase != SessionPhase::Retired)
    {
      _groupCoverPresentersPtr->refresh(sender);
    }
  }

  void MainWindow::handleGroupCoverUnloaded(Windows::Foundation::IInspectable const& sender,
                                            Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_groupCoverPresentersPtr && _sessionPhase != SessionPhase::Retired)
    {
      _groupCoverPresentersPtr->unload(sender);
    }
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
    tryApplyShellState(width);
  }

  void MainWindow::handleColumnHeaderClicked(Windows::Foundation::IInspectable const& sender,
                                             Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_sessionPhase == SessionPhase::Retired)
    {
      return;
    }

    // The compiled template calls the window, which outlives the shell that
    // realized this header. Only senders in the mounted shell can target it.
    auto const shell = ShellLayoutHost().Child();

    for (auto node = sender.try_as<Microsoft::UI::Xaml::DependencyObject>(); shell && node;
         node = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(node))
    {
      if (node == shell)
      {
        executeSort(requiredColumnFieldId(sender));
        return;
      }
    }
  }

  void MainWindow::handleColumnResizeCompleted(
    Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const& args)
  {
    constexpr double kMinimumColumnResize = 0.5;

    if (_trackListPtr == nullptr || args.Canceled() || std::abs(args.HorizontalChange()) < kMinimumColumnResize)
    {
      return;
    }

    auto const fieldId = requiredColumnFieldId(sender);

    if (auto resizedRes = _trackListPtr->resizeColumn(fieldId, args.HorizontalChange()); !resizedRes)
    {
      updateStatus(ao::winui::formatResource("winui_column_resize_failed", resizedRes.error().message));
    }
  }

  void MainWindow::handleColumnMoveLeftClicked(Windows::Foundation::IInspectable const& sender,
                                               Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    moveColumn(sender, -1);
  }

  void MainWindow::handleColumnMoveRightClicked(Windows::Foundation::IInspectable const& sender,
                                                Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    moveColumn(sender, 1);
  }

  void MainWindow::moveColumn(Windows::Foundation::IInspectable const& sender, std::int32_t const offset)
  {
    if (_trackListPtr == nullptr)
    {
      return;
    }

    auto const fieldId = requiredColumnFieldId(sender);

    if (auto movedRes = _trackListPtr->moveColumn(fieldId, offset); !movedRes)
    {
      updateStatus(ao::winui::formatResource("winui_column_move_failed", movedRes.error().message));
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

    auto const optColumn = ao::rt::trackFieldFromId(columnId);

    if (!optColumn || !ao::rt::trackSortFieldForColumn(*optColumn))
    {
      updateStatus(ao::winui::formatResource("track_sort_column_unavailable", columnId));
      return;
    }

    if (auto const sortedRes = _trackListPtr->toggleSort(*optColumn); !sortedRes)
    {
      updateStatus(ao::winui::formatResource("track_sort_failed", sortedRes.error().message));
      return;
    }

    _session->requestWorkspaceSortCheckpoint();
  }
} // namespace winrt::Aobus::implementation
