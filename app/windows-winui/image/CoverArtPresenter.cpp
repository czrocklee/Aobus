// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/CoverArtPresenter.h"

#include "image/CoverArtPlaceholderRenderer.h"
#include "theme/ThemeCoordinator.h"
#include <ao/CoreIds.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/winui/MemoryRandomAccessStream.h>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <stop_token>
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
                                       rt::ResourceByteLoader& resources,
                                       ThemeCoordinator& theme,
                                       uimodel::CoverArtPlaceholderStyle const style)
    : _statePtr{std::make_shared<State>()}, _resources{resources}, _theme{theme}
  {
    _statePtr->image = std::move(image);
    _statePtr->placeholder = std::move(placeholder);
    _statePtr->style = style;
  }

  CoverArtPresenter::~CoverArtPresenter()
  {
    unbind();
  }

  void CoverArtPresenter::bind(async::Runtime& runtime)
  {
    unbind();
    resetPresentation();
    _runtime = &runtime;
    _statePtr->active = true;
  }

  void CoverArtPresenter::unbind() noexcept
  {
    // Make every queued resource callback stale before cancelling its work or
    // touching the native image surface.
    _runtime = nullptr;
    _statePtr->active = false;
    ++_statePtr->generation;

    try
    {
      _request.reset();
    }
    // NOLINTNEXTLINE(bugprone-empty-catch): Request cancellation is best-effort after the presenter is retired.
    catch (...)
    {
    }

    try
    {
      _streamTask.reset();
    }
    // NOLINTNEXTLINE(bugprone-empty-catch): Stream-task cancellation is best-effort during presenter teardown.
    catch (...)
    {
    }
  }

  void CoverArtPresenter::resetPresentation()
  {
    _statePtr->image.Source(nullptr);
    _statePtr->image.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    _statePtr->placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
  }

  void CoverArtPresenter::select(ResourceId const resourceId,
                                 uimodel::CoverArtPlaceholderIdentity identity,
                                 bool const hasEntity)
  {
    _request.reset();
    _streamTask.reset();
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

    auto const weakStatePtr = std::weak_ptr<State>{_statePtr};
    _request = _resources.request(
      resourceId,
      [this, weakStatePtr, generation](rt::ResourceBytes bytes)
      {
        auto statePtr = weakStatePtr.lock();

        if (!statePtr || !statePtr->active || statePtr->generation != generation || _runtime == nullptr)
        {
          return;
        }

        _streamTask = _runtime->spawnCancellable(
          [weakStatePtr, runtime = _runtime, generation, bytes = std::move(bytes)](
            std::stop_token const stopToken) mutable
          { return prepareAndDisplay(weakStatePtr, runtime, generation, std::move(bytes), stopToken); });
      });
  }

  async::Task<void> CoverArtPresenter::prepareAndDisplay(std::weak_ptr<State> statePtr,
                                                         async::Runtime* const runtime,
                                                         std::uint64_t const generation,
                                                         rt::ResourceBytes bytes,
                                                         std::stop_token const stopToken)
  {
    auto prepared = PreparedMemoryRandomAccessStream{};

    try
    {
      co_await runtime->resumeOnWorker(stopToken);

      if (!bytes.empty())
      {
        prepared = prepareMemoryRandomAccessStream(bytes.view());
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      runtime->reportUnhandledException(std::current_exception(), "Windows cover-art stream preparation");
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (auto lockedStatePtr = statePtr.lock();
        lockedStatePtr && lockedStatePtr->active && lockedStatePtr->generation == generation)
    {
      display(*lockedStatePtr, generation, std::move(prepared));
    }
  }

  void CoverArtPresenter::display(State& state,
                                  std::uint64_t const generation,
                                  PreparedMemoryRandomAccessStream prepared)
  {
    if (!state.active || state.generation != generation || !prepared)
    {
      return;
    }

    try
    {
      auto stream = makeMemoryRandomAccessStream(std::move(prepared));

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
