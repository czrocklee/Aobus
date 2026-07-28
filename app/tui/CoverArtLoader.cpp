// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArtLoader.h"

#include "CoverArt.h"
#include <ao/CoreIds.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/resource/ResourceBytes.h>

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

  CoverArtLoader::CoverArtLoader(rt::ResourceByteLoader& byteLoader,
                                 async::Runtime& runtime,
                                 CoverArtDeliveryMode const mode,
                                 RefreshCallback refresh)
    : _byteLoader{byteLoader}, _runtime{runtime}, _mode{mode}, _refresh{std::move(refresh)}
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
    _byteRequest.reset();
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

    _byteRequest = _byteLoader.request(
      resourceId,
      [this, mode = _mode](rt::ResourceBytes bytes)
      {
        _task = _runtime.spawnCancellable(
          [loader = this, runtime = &_runtime, mode, bytes = std::move(bytes)](std::stop_token const stopToken) mutable
          { return load(loader, runtime, mode, std::move(bytes), stopToken); });
      });
  }

  void CoverArtLoader::clear()
  {
    if (_resourceId == kInvalidResourceId && !_optPreview && !_optKittyPng)
    {
      return;
    }

    _task.reset();
    _byteRequest.reset();
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
    _byteRequest.reset();
  }

  async::Task<void> CoverArtLoader::load(CoverArtLoader* const loader,
                                         async::Runtime* const runtime,
                                         CoverArtDeliveryMode const mode,
                                         rt::ResourceBytes bytes,
                                         std::stop_token const stopToken)
  {
    auto optPreview = std::optional<CoverArtRows>{};
    auto optKittyPng = std::optional<std::vector<std::byte>>{};

    try
    {
      co_await runtime->resumeOnWorker(stopToken);

      if (mode == CoverArtDeliveryMode::Blocks)
      {
        optPreview = decodeCoverArtPreview(bytes.view(), kBlockCoverArtColumns, kBlockCoverArtRows);
      }
      else if (mode == CoverArtDeliveryMode::Kitty)
      {
        optKittyPng = decodeCoverArtPng(bytes.view(), kKittyCoverArtWidth, kKittyCoverArtHeight);
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      runtime->reportUnhandledException(std::current_exception(), "TUI cover-art decode workflow");
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
