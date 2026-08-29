// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArtLoader.h"

#include "CoverArt.h"
#include <ao/CoreIds.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceByteMemoryCache.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kKittyCoverArtDimension = 512;
  } // namespace

  CoverArtLoader::CoverArtLoader(rt::ResourceByteMemoryCache& byteCache,
                                 async::Runtime& runtime,
                                 CoverArtDeliveryMode const mode,
                                 RefreshCallback refresh,
                                 std::int32_t const columns)
    : _byteCache{byteCache}
    , _runtime{runtime}
    , _mode{mode}
    , _refresh{std::move(refresh)}
    , _columns{std::max(0, columns)}
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

    cancel();
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

    _settleTask =
      _runtime.spawnCancellable([loader = this, runtime = &_runtime, resourceId](std::stop_token const stopToken)
                                { return waitForSelectionSettle(loader, runtime, resourceId, stopToken); },
                                "TUI cover-art selection settle");
  }

  void CoverArtLoader::clear()
  {
    if (_resourceId == kInvalidResourceId && !_optPreview && !_optKittyPng)
    {
      return;
    }

    cancel();
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
    _settleTask.reset();
    _task.reset();
    _byteRequest.reset();
  }

  async::Task<void> CoverArtLoader::waitForSelectionSettle(CoverArtLoader* const loader,
                                                           async::Runtime* const runtime,
                                                           ResourceId const resourceId,
                                                           std::stop_token const stopToken)
  {
    co_await runtime->sleepFor(kCoverArtSelectionSettleInterval, stopToken);
    co_await runtime->resumeOnCallbackExecutor(stopToken);

    // The settle window is not the publication fence: it only decides whether a
    // read is worth starting, and the selection may have moved on regardless.
    if (loader->_resourceId != resourceId)
    {
      co_return;
    }

    // The window has done its job; retire its handle before the read it
    // authorizes registers, so only live work is left to cancel.
    loader->_settleTask.reset();
    loader->startByteRequest(resourceId);
  }

  void CoverArtLoader::startByteRequest(ResourceId const resourceId)
  {
    _byteRequest = _byteCache.request(resourceId,
                                      [this, mode = _mode, columns = _columns](rt::ResourceBytes bytes)
                                      {
                                        _task = _runtime.spawnCancellable(
                                          [loader = this, runtime = &_runtime, mode, columns, bytes = std::move(bytes)](
                                            std::stop_token const stopToken) mutable
                                          { return load(loader, runtime, mode, columns, std::move(bytes), stopToken); },
                                          "TUI cover-art decode workflow");
                                      });
  }

  async::Task<void> CoverArtLoader::load(CoverArtLoader* const loader,
                                         async::Runtime* const runtime,
                                         CoverArtDeliveryMode const mode,
                                         std::int32_t const columns,
                                         rt::ResourceBytes bytes,
                                         std::stop_token const stopToken)
  {
    auto optPreview = std::optional<CoverArtRows>{};
    auto optKittyPng = std::optional<std::vector<std::byte>>{};

    co_await runtime->resumeOnWorker(stopToken);

    if (mode == CoverArtDeliveryMode::Blocks)
    {
      optPreview =
        decodeCoverArtPreview(bytes.view(), static_cast<std::size_t>(columns), static_cast<std::size_t>(kCoverArtRows));
    }
    else if (mode == CoverArtDeliveryMode::Kitty)
    {
      optKittyPng = decodeCoverArtPng(bytes.view(), kKittyCoverArtDimension, kKittyCoverArtDimension);
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    loader->_optPreview = std::move(optPreview);
    loader->_optKittyPng = std::move(optKittyPng);
    loader->_byteRequest.reset();
    loader->_task.reset();

    if (loader->_refresh)
    {
      loader->_refresh();
    }
  }
} // namespace ao::tui
