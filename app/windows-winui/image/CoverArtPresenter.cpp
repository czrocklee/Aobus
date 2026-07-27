// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/CoverArtPresenter.h"

#include "platform/MemoryRandomAccessStream.h"
#include <ao/async/OperationCancelled.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryTaskService.h>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include <exception>
#include <memory>
#include <stop_token>
#include <utility>

namespace ao::winui
{
  struct CoverArtPresenter::State final
  {
    winrt::Microsoft::UI::Xaml::Controls::Image image{nullptr};
    winrt::Microsoft::UI::Xaml::UIElement placeholder{nullptr};
    uimodel::CoverArtRequestModel requests{};
    bool active = false;
  };

  CoverArtPresenter::CoverArtPresenter(winrt::Microsoft::UI::Xaml::Controls::Image image,
                                       winrt::Microsoft::UI::Xaml::UIElement placeholder)
    : _statePtr{std::make_shared<State>()}
  {
    _statePtr->image = std::move(image);
    _statePtr->placeholder = std::move(placeholder);
  }

  CoverArtPresenter::~CoverArtPresenter()
  {
    unbind();
  }

  void CoverArtPresenter::bind(std::shared_ptr<rt::AppRuntime> runtimePtr)
  {
    unbind();
    _runtimePtr = std::move(runtimePtr);
    _statePtr->active = true;
  }

  void CoverArtPresenter::unbind()
  {
    _task.reset();
    _statePtr->active = false;
    _statePtr->requests.reset();
    _statePtr->image.Source(nullptr);
    _statePtr->placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
    _runtimePtr.reset();
  }

  void CoverArtPresenter::select(ResourceId const resourceId)
  {
    _task.reset();
    if (!_statePtr->active || !_runtimePtr || resourceId == kInvalidResourceId)
    {
      _statePtr->requests.clearSelection();
      _statePtr->image.Source(nullptr);
      _statePtr->placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
      return;
    }

    auto const token = _statePtr->requests.select(resourceId);
    if (!_statePtr->requests.cached(resourceId).empty())
    {
      display(*_statePtr, token);
      return;
    }

    _statePtr->image.Source(nullptr);
    _statePtr->placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);

    auto const state = std::weak_ptr<State>{_statePtr};
    auto* const tasks = &_runtimePtr->library().taskService();
    auto* const runtime = &_runtimePtr->async();
    // The runtime and task service outlive the presenter task. Presenter-owned
    // UI state is touched only after the cancellation-checked callback hop.
    _task = runtime->spawnCancellable([state, tasks, runtime, token](std::stop_token const stopToken) mutable
                                      { return load(state, tasks, runtime, token, stopToken); });
  }

  async::Task<void> CoverArtPresenter::load(std::weak_ptr<State> const state,
                                            rt::LibraryTaskService* const tasks,
                                            async::Runtime* const runtime,
                                            uimodel::CoverArtRequestToken const token,
                                            std::stop_token const stopToken)
  {
    try
    {
      auto bytes = co_await tasks->loadResourceAsync(token.resourceId, stopToken);
      if (!bytes || !*bytes)
      {
        co_return;
      }

      auto payload = std::move(**bytes);
      co_await runtime->resumeOnCallbackExecutor(stopToken);
      auto statePtr = state.lock();
      if (!statePtr || !statePtr->active || !statePtr->requests.store(token, std::move(payload)))
      {
        co_return;
      }
      display(*statePtr, token);
      statePtr.reset();
    }
    catch (async::OperationCancelled const&)
    {
    }
    catch (...)
    {
      runtime->reportUnhandledException(std::current_exception(), "Windows inspector cover-art workflow");
    }
  }

  void CoverArtPresenter::display(State& state, uimodel::CoverArtRequestToken const token)
  {
    try
    {
      auto const cached = state.requests.cached(token.resourceId);
      auto stream = makeMemoryRandomAccessStream(cached);
      auto bitmap = winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage{};
      bitmap.SetSource(stream);
      if (!state.active || !state.requests.accepts(token))
      {
        return;
      }
      state.image.Source(bitmap);
      state.placeholder.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }
    catch (winrt::hresult_error const& error)
    {
      APP_LOG_WARN("Windows inspector cover-art decode failed: {}", winrt::to_string(error.message()));
    }
  }
} // namespace ao::winui
