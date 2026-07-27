// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/CoverArtPresenter.h"

#include "image/CoverArtPlaceholderRenderer.h"
#include "image/WindowsCoverArtLoader.h"
#include "platform/MemoryRandomAccessStream.h"
#include "theme/WindowsThemeCoordinator.h"
#include <ao/rt/Log.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace ao::winui
{
  struct CoverArtPresenter::State final
  {
    winrt::Microsoft::UI::Xaml::Controls::Image image{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid placeholder{nullptr};
    uimodel::CoverArtPlaceholderStyle style = uimodel::CoverArtPlaceholderStyle::Note;
    std::uint64_t generation = 0;
    bool active = false;
  };

  CoverArtPresenter::CoverArtPresenter(winrt::Microsoft::UI::Xaml::Controls::Image image,
                                       winrt::Microsoft::UI::Xaml::Controls::Grid placeholder,
                                       WindowsCoverArtLoader& loader,
                                       WindowsThemeCoordinator& theme,
                                       uimodel::CoverArtPlaceholderStyle const style)
    : _statePtr{std::make_shared<State>()}, _loader{loader}, _theme{theme}
  {
    _statePtr->image = std::move(image);
    _statePtr->placeholder = std::move(placeholder);
    _statePtr->style = style;
  }

  CoverArtPresenter::~CoverArtPresenter()
  {
    unbind();
  }

  void CoverArtPresenter::bind()
  {
    unbind();
    _statePtr->active = true;
  }

  void CoverArtPresenter::unbind()
  {
    _request.reset();
    _statePtr->active = false;
    ++_statePtr->generation;
    _statePtr->image.Source(nullptr);
    _statePtr->image.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    _statePtr->placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
  }

  void CoverArtPresenter::select(ResourceId const resourceId,
                                 uimodel::CoverArtPlaceholderIdentity identity,
                                 bool const hasEntity)
  {
    _request.reset();
    auto const generation = ++_statePtr->generation;
    _statePtr->image.Source(nullptr);
    _statePtr->image.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    _statePtr->placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

    if (!_statePtr->active || !hasEntity)
    {
      return;
    }

    if (resourceId == kInvalidResourceId)
    {
      auto const presentation = uimodel::makeCoverArtPlaceholderPresentation(_statePtr->style, identity);
      renderCoverArtPlaceholder(_statePtr->placeholder, presentation, _theme.theme().shared.accent);
      _statePtr->placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
      return;
    }

    auto const state = std::weak_ptr<State>{_statePtr};
    _request = _loader.request(resourceId,
                               [state, generation](std::span<std::byte const> const bytes)
                               {
                                 auto statePtr = state.lock();
                                 if (!statePtr || !statePtr->active || statePtr->generation != generation)
                                 {
                                   return;
                                 }
                                 display(*statePtr, generation, bytes);
                               });
  }

  void CoverArtPresenter::display(State& state, std::uint64_t const generation, std::span<std::byte const> const bytes)
  {
    if (!state.active || state.generation != generation || bytes.empty())
    {
      return;
    }

    try
    {
      auto stream = makeMemoryRandomAccessStream(bytes);
      auto bitmap = winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage{};
      bitmap.SetSource(stream);
      if (!state.active || state.generation != generation)
      {
        return;
      }
      state.image.Source(bitmap);
      state.image.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
      state.placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }
    catch (winrt::hresult_error const& error)
    {
      APP_LOG_WARN("Windows cover-art decode failed: {}", winrt::to_string(error.message()));
    }
  }
} // namespace ao::winui
