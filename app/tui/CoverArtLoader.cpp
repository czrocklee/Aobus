// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArtLoader.h"

#include "CoverArt.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryTaskService.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::size_t kBlockCoverArtColumns = 24;
    constexpr std::size_t kBlockCoverArtRows = 12;
    constexpr std::int32_t kKittyCoverArtWidth = 768;
    constexpr std::int32_t kKittyCoverArtHeight = 384;
  } // namespace

  CoverArtLoader::CoverArtLoader(rt::LibraryTaskService& tasks,
                                 async::Runtime& runtime,
                                 CoverArtDeliveryMode const mode,
                                 RefreshCallback refresh)
    : _tasks{tasks}, _runtime{runtime}, _mode{mode}, _refresh{std::move(refresh)}
  {
  }

  CoverArtLoader::~CoverArtLoader()
  {
    cancel();
  }

  void CoverArtLoader::request(ResourceId const resourceId)
  {
    if (resourceId == _resourceId)
    {
      return;
    }

    _task.reset();
    ++_generation;
    _resourceId = resourceId;
    _optPreview.reset();
    _optKittyPng.reset();

    if (_refresh)
    {
      _refresh();
    }

    if (resourceId == kInvalidResourceId || _mode == CoverArtDeliveryMode::Off)
    {
      return;
    }

    auto const generation = _generation;
    _task = _runtime.spawnCancellable(
      [loader = this, tasks = &_tasks, runtime = &_runtime, mode = _mode, resourceId, generation](
        std::stop_token const stopToken)
      { return load(loader, tasks, runtime, mode, resourceId, generation, stopToken); });
  }

  void CoverArtLoader::clear()
  {
    if (_resourceId == kInvalidResourceId && !_optPreview && !_optKittyPng)
    {
      return;
    }

    _task.reset();
    ++_generation;
    _resourceId = kInvalidResourceId;
    _optPreview.reset();
    _optKittyPng.reset();

    if (_refresh)
    {
      _refresh();
    }
  }

  void CoverArtLoader::cancel() noexcept
  {
    _task.reset();
    ++_generation;
  }

  async::Task<void> CoverArtLoader::load(CoverArtLoader* const loader,
                                         rt::LibraryTaskService* const tasks,
                                         async::Runtime* const runtime,
                                         CoverArtDeliveryMode const mode,
                                         ResourceId const resourceId,
                                         std::uint64_t const generation,
                                         std::stop_token const stopToken)
  {
    auto optPreview = std::optional<CoverArtRows>{};
    auto optKittyPng = std::optional<std::vector<std::byte>>{};

    try
    {
      auto bytesResult = co_await tasks->loadResourceAsync(resourceId, stopToken);

      if (!bytesResult)
      {
        if (bytesResult.error().code == Error::Code::ValueTooLarge)
        {
          APP_LOG_WARN("TUI cover resource {} exceeds the interactive byte limit", resourceId.raw());
        }
      }
      else if (*bytesResult)
      {
        auto bytes = std::move(**bytesResult);
        co_await runtime->resumeOnWorker(stopToken);

        if (mode == CoverArtDeliveryMode::Blocks)
        {
          optPreview = decodeCoverArtPreview(bytes, kBlockCoverArtColumns, kBlockCoverArtRows);
        }
        else if (mode == CoverArtDeliveryMode::Kitty)
        {
          optKittyPng = decodeCoverArtPng(bytes, kKittyCoverArtWidth, kKittyCoverArtHeight);
        }
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      runtime->reportUnhandledException(std::current_exception(), "TUI cover-art decode workflow");
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (loader->_resourceId != resourceId || loader->_generation != generation)
    {
      co_return;
    }

    loader->_optPreview = std::move(optPreview);
    loader->_optKittyPng = std::move(optKittyPng);
    loader->_task.reset();

    if (loader->_refresh)
    {
      loader->_refresh();
    }
  }
} // namespace ao::tui
