// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "image/CoverArtPresenter.h"
#include "layout/ShellBuilder.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace winrt::Aobus::implementation
{
  namespace
  {
    using ProjectedTrackRowItem = ::winrt::Aobus::TrackRowItem;
  } // namespace

  void MainWindow::reconcileLibrary()
  {
    if (_session == nullptr || _trackListPtr == nullptr)
    {
      return;
    }

    if (auto const listId = _trackListPtr->activeListId();
        _session->presentationPreferences().presentations.contains(listId))
    {
      if (auto const selected = _trackListPtr->selectPresentation(_session->presentationForList(listId)); !selected)
      {
        updateStatus(ao::winui::formatResource("PresentationFailedFormat", selected.error().message));
      }
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

    if (!row || !row.IsGroupHeader() || _resourceBytes == nullptr || _themePtr == nullptr || _session == nullptr ||
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
        *_resourceBytes,
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
    if (auto button = sender.try_as<Microsoft::UI::Xaml::Controls::Button>(); button)
    {
      executeSort(to_string(unbox_value_or<hstring>(button.Tag(), L"title")));
    }
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

    auto element = sender.try_as<Microsoft::UI::Xaml::FrameworkElement>();
    auto const fieldId = element ? to_string(unbox_value_or<hstring>(element.Tag(), L"")) : std::string{};

    if (auto resized = _trackListPtr->resizeColumn(fieldId, args.HorizontalChange()); !resized)
    {
      updateStatus(ao::winui::formatResource("ColumnResizeFailedFormat", resized.error().message));
      return;
    }

    if (auto saved = _session->saveSettings(); !saved)
    {
      updateStatus(ao::winui::formatResource("ColumnSettingsFailedFormat", saved.error().message));
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

    auto element = sender.try_as<Microsoft::UI::Xaml::FrameworkElement>();
    auto const fieldId = element ? to_string(unbox_value_or<hstring>(element.Tag(), L"")) : std::string{};

    if (auto moved = _trackListPtr->moveColumn(fieldId, offset); !moved)
    {
      updateStatus(ao::winui::formatResource("ColumnMoveFailedFormat", moved.error().message));
      return;
    }

    if (auto saved = _session->saveSettings(); !saved)
    {
      updateStatus(ao::winui::formatResource("ColumnSettingsFailedFormat", saved.error().message));
    }
  }

  void MainWindow::showColumnsMenu()
  {
    if (_trackListPtr == nullptr)
    {
      return;
    }

    auto flyout = Microsoft::UI::Xaml::Controls::MenuFlyout{};
    auto const text = ao::uimodel::PresentationTextCatalog{};
    auto weak = get_weak();

    for (auto const& choice : _trackListPtr->columnChoices())
    {
      auto item = Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem{};
      auto const fieldId = ao::rt::trackFieldId(choice.field);
      item.Text(
        to_hstring(ao::winui::stableResourceString("TrackField_", fieldId, text.trackFieldLabel(choice.field))));
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

          auto const fieldId = to_string(unbox_value_or<hstring>(toggle.Tag(), L""));
          auto changed = self->_trackListPtr->setColumnVisible(fieldId, toggle.IsChecked());

          if (!changed)
          {
            toggle.IsChecked(!toggle.IsChecked());
            self->updateStatus(ao::winui::formatResource("ColumnVisibilityFailedFormat", changed.error().message));
            return;
          }

          if (self->_session != nullptr)
          {
            if (auto saved = self->_session->saveSettings(); !saved)
            {
              self->updateStatus(ao::winui::formatResource("ColumnSettingsFailedFormat", saved.error().message));
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
      updateStatus(ao::winui::formatResource("ColumnNotSortableFormat", columnId));
      return;
    }

    auto sorted = _trackListPtr->toggleSort(*definition->optSortField);

    if (!sorted)
    {
      updateStatus(ao::winui::formatResource("SortFailedFormat", sorted.error().message));
      return;
    }

    if (auto saved = _session->saveSettings(); !saved)
    {
      updateStatus(ao::winui::formatResource("ColumnSettingsFailedFormat", saved.error().message));
    }
  }
} // namespace winrt::Aobus::implementation
