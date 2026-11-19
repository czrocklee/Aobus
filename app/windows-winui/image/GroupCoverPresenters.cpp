// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Native IUnknown must be available before the first C++/WinRT projection for get_unknown.
#include <unknwn.h>

// Win32's GetCurrentTime macro collides with the XAML projection.
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#include "image/GroupCoverPresenters.h"

#include <winrt/Aobus.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#pragma pop_macro("GetCurrentTime")

#include "image/CoverArtPresenter.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::Image;
    using winrt::Windows::Foundation::IInspectable;
  } // namespace

  struct GroupCoverPresenters::Impl final
  {
    struct Entry final
    {
      winrt::weak_ref<Grid> tile;
      std::unique_ptr<CoverArtPresenter> presenterPtr;
    };

    Impl(async::Runtime& asyncRuntime, rt::ResourceByteMemoryCache& resourceBytes, ThemeCoordinator& themeCoordinator)
      : runtime{asyncRuntime}, resources{resourceBytes}, theme{themeCoordinator}
    {
    }

    async::Runtime& runtime;
    rt::ResourceByteMemoryCache& resources;
    ThemeCoordinator& theme;
    std::unordered_map<::IUnknown const*, Entry> presenters;
  };

  GroupCoverPresenters::GroupCoverPresenters(async::Runtime& runtime,
                                             rt::ResourceByteMemoryCache& resources,
                                             ThemeCoordinator& theme)
    : _implPtr{std::make_unique<Impl>(runtime, resources, theme)}
  {
  }

  GroupCoverPresenters::~GroupCoverPresenters() = default;

  void GroupCoverPresenters::refresh(IInspectable const& sender)
  {
    auto const tile = sender.try_as<Grid>();

    if (!tile)
    {
      return;
    }

    auto const* key = winrt::get_unknown(tile);
    auto& presenters = _implPtr->presenters;
    auto presenterIt = presenters.find(key);

    if (presenterIt != presenters.end())
    {
      // An expired tile's address can be reused. Validate the weak reference before reusing its presenter.
      if (auto const retainedTile = presenterIt->second.tile.get();
          !retainedTile || winrt::get_unknown(retainedTile) != key)
      {
        presenters.erase(presenterIt);
        presenterIt = presenters.end();
      }
    }

    auto const row = tile.DataContext().try_as<winrt::Aobus::TrackRowItem>();

    if (!row || !row.IsGroupHeader() || tile.Children().Size() < 2)
    {
      if (presenterIt != presenters.end())
      {
        presenterIt->second.presenterPtr->select(kInvalidResourceId, uimodel::CoverArtPlaceholderIdentity{}, false);
      }

      return;
    }

    auto const placeholder = tile.Children().GetAt(0).try_as<Grid>();
    auto const image = tile.Children().GetAt(1).try_as<Image>();

    if (!placeholder || !image)
    {
      if (presenterIt != presenters.end())
      {
        presenterIt->second.presenterPtr->select(kInvalidResourceId, uimodel::CoverArtPlaceholderIdentity{}, false);
      }

      return;
    }

    if (presenterIt == presenters.end())
    {
      auto presenterPtr = std::make_unique<CoverArtPresenter>(
        image,
        placeholder,
        _implPtr->runtime,
        _implPtr->resources,
        _implPtr->theme,
        uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::GroupHeading));
      presenterIt =
        presenters.emplace(key, Impl::Entry{.tile = winrt::make_weak(tile), .presenterPtr = std::move(presenterPtr)})
          .first;
    }

    auto identity = uimodel::CoverArtPlaceholderIdentity{
      .primaryText = winrt::to_string(row.Title()),
    };

    if (auto monogram = winrt::to_string(row.CoverArtMonogram()); !monogram.empty())
    {
      identity.optMonogram = std::move(monogram);
    }

    presenterIt->second.presenterPtr->select(ResourceId{row.CoverArtId()}, std::move(identity), true);
  }

  void GroupCoverPresenters::unload(IInspectable const& sender)
  {
    if (auto const tile = sender.try_as<Grid>(); tile)
    {
      _implPtr->presenters.erase(winrt::get_unknown(tile));
    }
  }
} // namespace ao::winui
