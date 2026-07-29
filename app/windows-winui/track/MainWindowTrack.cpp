// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "image/CoverArtPresenter.h"
#include "pch.h"
#include "platform/ScopedBooleanFlag.h"
#include "platform/WindowsStringResources.h"
#include "track/TrackListController.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/utility/Path.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace winrt::Aobus::implementation
{
  using Microsoft::UI::Xaml::Visibility;

  namespace
  {
    using ProjectedTrackRowItem = ::winrt::Aobus::TrackRowItem;

    constexpr double kNavigationItemSpacing = 6.0;
    constexpr double kNavigationFilterOpacity = 0.55;

    Microsoft::UI::Xaml::Controls::Symbol navigationSymbol(ao::uimodel::ListTreeProjectionRow const& row) noexcept
    {
      if (row.id == ao::rt::kAllTracksListId)
      {
        return Microsoft::UI::Xaml::Controls::Symbol::MusicInfo;
      }

      return Microsoft::UI::Xaml::Controls::Symbol::Find;
    }

    hstring navigationLabel(ao::uimodel::ListTreeProjectionRow const& row)
    {
      return row.id == ao::rt::kAllTracksListId ? ao::winui::resourceHstring(L"AllTracks") : to_hstring(row.name);
    }

    Microsoft::UI::Xaml::FrameworkElement navigationContent(ao::uimodel::ListTreeProjectionRow const& row,
                                                            bool const includeIcon)
    {
      auto content = Microsoft::UI::Xaml::Controls::StackPanel{};
      content.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
      content.Spacing(kNavigationItemSpacing);
      content.Tag(box_value(row.id.raw()));

      if (includeIcon)
      {
        auto icon = Microsoft::UI::Xaml::Controls::SymbolIcon{navigationSymbol(row)};
        content.Children().Append(icon);
      }

      auto label = Microsoft::UI::Xaml::Controls::TextBlock{};
      label.Text(navigationLabel(row));
      label.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
      content.Children().Append(label);

      if (!row.localExpression.empty())
      {
        auto filterText = std::string{"["};
        filterText += row.localExpression;
        filterText += ']';

        auto filter = Microsoft::UI::Xaml::Controls::TextBlock{};
        filter.Text(to_hstring(filterText));
        filter.Opacity(kNavigationFilterOpacity);
        filter.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
        content.Children().Append(filter);
      }

      return content;
    }

    ProjectedTrackRowItem trackRowFromEventSource(Windows::Foundation::IInspectable const& source)
    {
      auto current = source.try_as<Microsoft::UI::Xaml::DependencyObject>();

      while (current)
      {
        if (auto const element = current.try_as<Microsoft::UI::Xaml::FrameworkElement>(); element)
        {
          if (auto const row = element.DataContext().try_as<ProjectedTrackRowItem>(); row)
          {
            return row;
          }
        }

        current = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(current);
      }

      return nullptr;
    }
  } // namespace

  void MainWindow::reconcileLibrary()
  {
    if (_session == nullptr || _trackListPtr == nullptr)
    {
      return;
    }

    ModernTrackList().ItemsSource(_trackListPtr->items());
    ClassicTrackList().ItemsSource(_trackListPtr->items());
    ModernColumnHeaders().ItemsSource(_trackListPtr->headers());
    ClassicColumnHeaders().ItemsSource(_trackListPtr->headers());
    ModernLibraryPath().Text(to_hstring(ao::utility::pathToUtf8(_session->libraryRuntime().musicRoot())));

    auto const listId = _trackListPtr->activeListId();

    if (auto const it = _session->presentationPreferences().presentations.find(listId);
        it != _session->presentationPreferences().presentations.end())
    {
      std::ignore = _trackListPtr->selectPresentation(it->second);
    }

    rebuildNavigation();
    updateBrowserHeader();
    updateTrackSurfaceWidth();
  }

  void MainWindow::rebuildNavigation()
  {
    if (_session == nullptr || _trackListPtr == nullptr)
    {
      return;
    }

    auto const lists = _session->libraryRuntime().library().reader().lists();
    auto const projection = ao::uimodel::buildListTreeProjection(lists);
    auto const activeListId = _trackListPtr->activeListId();
    [[maybe_unused]] auto const applyingNavigation = ao::winui::ScopedBooleanFlag{_applyingNavigation};
    ModernNavigation().MenuItems().Clear();
    ClassicLibraryTree().RootNodes().Clear();
    _navigationEntriesById.clear();

    auto modernItems = std::map<ao::ListId, Microsoft::UI::Xaml::Controls::NavigationViewItem>{};
    auto classicItems = std::map<ao::ListId, Microsoft::UI::Xaml::Controls::TreeViewNode>{};

    for (auto const& [id, row] : projection.rowsById)
    {
      auto modernItem = Microsoft::UI::Xaml::Controls::NavigationViewItem{};
      modernItem.Content(navigationContent(row, false));
      modernItem.Icon(Microsoft::UI::Xaml::Controls::SymbolIcon{navigationSymbol(row)});
      modernItem.Tag(box_value(id.raw()));
      auto const entry = NavigationEntry{
        .listId = id,
        .label = to_string(navigationLabel(row)),
      };
      _navigationEntriesById.emplace(id, entry);
      modernItems.emplace(id, modernItem);

      auto classicItem = Microsoft::UI::Xaml::Controls::TreeViewNode{};
      classicItem.Content(navigationContent(row, true));
      classicItems.emplace(id, classicItem);
    }

    for (auto const& [id, row] : projection.rowsById)
    {
      auto& modernItem = modernItems.at(id);
      auto& classicItem = classicItems.at(id);

      for (auto const childId : row.childIds)
      {
        if (auto const child = modernItems.find(childId); child != modernItems.end())
        {
          modernItem.MenuItems().Append(child->second);
          classicItem.Children().Append(classicItems.at(childId));
        }
      }

      auto const hasChildren = !row.childIds.empty();
      modernItem.IsExpanded(hasChildren);
      classicItem.IsExpanded(hasChildren);
    }

    for (auto const rootId : projection.rootIds)
    {
      if (auto const root = modernItems.find(rootId); root != modernItems.end())
      {
        ModernNavigation().MenuItems().Append(root->second);
        ClassicLibraryTree().RootNodes().Append(classicItems.at(rootId));
      }
    }

    auto const defaultModern = modernItems.find(ao::rt::kAllTracksListId);
    auto const defaultClassic = classicItems.find(ao::rt::kAllTracksListId);

    if (defaultModern != modernItems.end() && defaultClassic != classicItems.end())
    {
      auto selectedModern = defaultModern->second;
      auto selectedClassic = defaultClassic->second;

      if (auto const modern = modernItems.find(activeListId); modern != modernItems.end())
      {
        selectedModern = modern->second;
        selectedClassic = classicItems.at(activeListId);
      }

      ModernNavigation().SelectedItem(selectedModern);
      ClassicLibraryTree().SelectedNode(selectedClassic);
    }
  }

  void MainWindow::navigateTo(NavigationEntry const& entry)
  {
    if (_trackListPtr == nullptr || entry.listId == ao::kInvalidListId)
    {
      return;
    }

    auto const navigated = _trackListPtr->navigateTo(entry.listId);

    if (!navigated)
    {
      updateStatus(ao::winui::formatResource("NavigationFailedFormat", navigated.error().message));
      return;
    }

    auto presentationId = std::string{};

    if (_session != nullptr)
    {
      if (auto const preference = _session->presentationPreferences().presentations.find(entry.listId);
          preference != _session->presentationPreferences().presentations.end())
      {
        presentationId = preference->second;
      }
    }

    if (!presentationId.empty())
    {
      if (auto const selected = _trackListPtr->selectPresentation(presentationId); !selected)
      {
        updateStatus(ao::winui::formatResource("PresentationFailedFormat", selected.error().message));
        return;
      }
    }

    updateBrowserHeader();
    updateStatus(ao::winui::formatResource("ListStatusFormat", entry.label));
  }

  void MainWindow::updateBrowserHeader()
  {
    if (_trackListPtr == nullptr)
    {
      return;
    }

    auto const count = _trackListPtr->rowCount();
    ModernTrackList().ItemsSource(_trackListPtr->items());
    ClassicTrackList().ItemsSource(_trackListPtr->items());
    auto const countText = count == 1 ? ao::winui::resourceString("TrackCountOne")
                                      : ao::winui::formatResource("TrackCountManyFormat", count);
    ModernTrackCount().Text(to_hstring(countText));
    ClassicTrackCount().Text(to_hstring(countText));

    auto const presentationId = _trackListPtr->activePresentationId();
    auto const optText = ao::uimodel::PresentationTextCatalog{}.builtinTrackPresentation(presentationId);
    ModernPresentationButton().Content(box_value(to_hstring(ao::winui::stableResourceString(
      "Presentation_", presentationId, optText ? optText->label : std::string_view{presentationId}))));
    updateTrackSurfaceWidth();
  }

  void MainWindow::updateTrackSurfaceWidth()
  {
    if (_trackListPtr == nullptr)
    {
      return;
    }

    auto const width = _trackListPtr->contentWidth();
    ModernTrackSurface().Width(width);
    ClassicTrackSurface().Width(width);
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
      presenterPtr->bind(_session->libraryRuntime().async());
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

  void MainWindow::clearGroupCoverPresenters()
  {
    _groupCoverPresenters.clear();
  }

  void MainWindow::OnTrackViewportSizeChanged(Windows::Foundation::IInspectable const& sender,
                                              Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
  {
    if (_trackListPtr != nullptr && args.NewSize().Width > 0.0)
    {
      auto const viewport = sender.try_as<Microsoft::UI::Xaml::FrameworkElement>();
      auto const trailingChromeWidth = viewport ? unbox_value_or<double>(viewport.Tag(), 12.0) : 12.0;
      _trackListPtr->setViewportWidth(args.NewSize().Width, trailingChromeWidth);
      updateTrackSurfaceWidth();
    }
  }

  void MainWindow::OnNavigationSelectionChanged(
    Windows::Foundation::IInspectable const& /*sender*/,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args)
  {
    if (_applyingNavigation || _trackListPtr == nullptr)
    {
      return;
    }

    if (auto item = args.SelectedItemContainer(); item)
    {
      auto const listId = ao::ListId{unbox_value_or<std::uint32_t>(item.Tag(), 0U)};

      if (auto const entry = _navigationEntriesById.find(listId); entry != _navigationEntriesById.end())
      {
        navigateTo(entry->second);
      }
    }
  }

  void MainWindow::OnClassicTreeSelectionChanged(
    Microsoft::UI::Xaml::Controls::TreeView const& sender,
    Microsoft::UI::Xaml::Controls::TreeViewSelectionChangedEventArgs const& /*args*/)
  {
    if (_applyingNavigation || _trackListPtr == nullptr)
    {
      return;
    }

    if (auto node = sender.SelectedNode(); node)
    {
      auto const content = node.Content().try_as<Microsoft::UI::Xaml::FrameworkElement>();

      if (!content)
      {
        return;
      }

      auto const listId = ao::ListId{unbox_value_or<std::uint32_t>(content.Tag(), 0U)};

      if (auto const entry = _navigationEntriesById.find(listId); entry != _navigationEntriesById.end())
      {
        navigateTo(entry->second);
      }
    }
  }

  void MainWindow::OnPresentationClicked(Windows::Foundation::IInspectable const& sender,
                                         Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_trackListPtr == nullptr)
    {
      return;
    }

    auto flyout = Microsoft::UI::Xaml::Controls::MenuFlyout{};
    auto const textCatalog = ao::uimodel::PresentationTextCatalog{};
    auto weak = get_weak();

    for (auto const& preset : ao::rt::builtinTrackPresentationPresets())
    {
      auto item = Microsoft::UI::Xaml::Controls::MenuFlyoutItem{};
      auto const eligibility = ao::uimodel::trackPresentationEligibility(_trackListPtr->activeListId(), preset.spec.id);
      auto const optText = textCatalog.builtinTrackPresentation(preset.spec.id);
      item.Text(to_hstring(ao::winui::stableResourceString(
        "Presentation_", preset.spec.id, optText ? optText->label : std::string_view{preset.spec.id})));
      item.IsEnabled(eligibility.enabled);

      if (!eligibility.enabled)
      {
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
          item, box_value(to_hstring(eligibility.disabledReason)));
      }

      item.Click(
        [weak, presentationId = preset.spec.id](
          Windows::Foundation::IInspectable const& /*sender*/, Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
        {
          if (auto self = weak.get(); self && self->_trackListPtr)
          {
            auto selected = self->_trackListPtr->selectPresentation(presentationId);

            if (!selected)
            {
              self->updateStatus(ao::winui::formatResource("PresentationFailedFormat", selected.error().message));
              return;
            }

            if (self->_session != nullptr)
            {
              auto const listId = self->_trackListPtr->activeListId();
              self->_session->presentationPreferences().presentations[listId] = presentationId;
              std::ignore = self->_session->saveSettings();
            }

            self->updateBrowserHeader();
          }
        });
      flyout.Items().Append(item);
    }

    flyout.ShowAt(sender.as<Microsoft::UI::Xaml::FrameworkElement>());
  }

  void MainWindow::OnInspectorToggleClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                            Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    _inspectorRequested = !_inspectorRequested;
    applyShellState(RootGrid().ActualWidth());
  }

  void MainWindow::OnTrackSelectionChanged(Windows::Foundation::IInspectable const& sender,
                                           Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
  {
    if (_trackListPtr == nullptr || _applyingTrackSelection)
    {
      return;
    }

    auto list = sender.as<Microsoft::UI::Xaml::Controls::ListView>();
    auto peer = get_abi(list) == get_abi(ModernTrackList()) ? ClassicTrackList() : ModernTrackList();
    auto const selectedItems = list.SelectedItems();
    auto const peerItems = peer.SelectedItems();
    auto selectionsMatch = selectedItems.Size() == peerItems.Size();

    for (std::uint32_t index = 0; selectionsMatch && index < selectedItems.Size(); ++index)
    {
      selectionsMatch = get_abi(selectedItems.GetAt(index)) == get_abi(peerItems.GetAt(index));
    }

    if (!selectionsMatch)
    {
      [[maybe_unused]] auto const applyingTrackSelection = ao::winui::ScopedBooleanFlag{_applyingTrackSelection};
      peerItems.Clear();

      for (auto const& item : selectedItems)
      {
        peerItems.Append(item);
      }
    }

    auto selected = std::vector<ao::TrackId>{};

    for (auto const& item : selectedItems)
    {
      if (auto row = item.try_as<ProjectedTrackRowItem>(); row && !row.IsGroupHeader() && row.TrackId() != 0)
      {
        selected.emplace_back(row.TrackId());
      }
    }

    _trackListPtr->publishSelection(selected);
    auto summary = ao::winui::resourceString("NoSelection");

    if (selected.size() == 1)
    {
      summary = ao::winui::resourceString("ItemSelectedOne");
    }
    else if (!selected.empty())
    {
      summary = ao::winui::formatResource("ItemsSelectedFormat", selected.size());
    }

    ModernSelectionSummary().Text(to_hstring(summary));
  }

  void MainWindow::OnTrackDoubleTapped(Windows::Foundation::IInspectable const& /*sender*/,
                                       Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& args)
  {
    if (_trackListPtr == nullptr || _session == nullptr)
    {
      return;
    }

    if (auto row = trackRowFromEventSource(args.OriginalSource()); row && !row.IsGroupHeader())
    {
      auto played = _trackListPtr->play(ao::TrackId{row.TrackId()},
                                        [this](ao::rt::ViewId const viewId, ao::TrackId const trackId)
                                        { return _session->playTrack(viewId, trackId); });

      if (!played)
      {
        updateStatus(ao::winui::formatResource("PlaybackFailedFormat", played.error().message));
      }
    }
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

  void MainWindow::OnColumnsClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                    Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
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

    auto const target = ModernShell().Visibility() == Microsoft::UI::Xaml::Visibility::Visible
                          ? ModernPresentationButton().as<Microsoft::UI::Xaml::FrameworkElement>()
                          : ClassicTrackList().as<Microsoft::UI::Xaml::FrameworkElement>();
    flyout.ShowAt(target);
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

    updateBrowserHeader();
  }
} // namespace winrt::Aobus::implementation
