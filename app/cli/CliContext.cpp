// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliContext.h"

#include <ao/async/ImmediateExecutor.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/Library.h>

#include <cstddef>
#include <memory>
#include <ostream>
#include <utility>

namespace ao::cli
{
  CliContext::CliContext(std::ostream& out, std::ostream& err, std::size_t const musicLibraryMapSize)
    : _io{.out = out, .err = err}, _musicLibraryMapSize{musicLibraryMapSize}
  {
  }

  CliContext::~CliContext() = default;

  rt::CoreRuntime& CliContext::runtime()
  {
    if (!_runtimePtr)
    {
      auto executorPtr = std::make_unique<async::ImmediateExecutor>();
      _runtimePtr = std::make_unique<rt::CoreRuntime>(
        std::move(executorPtr), _options.root, _options.root / ".aobus/library", _musicLibraryMapSize);
    }

    return *_runtimePtr;
  }

  library::MusicLibrary& CliContext::musicLibrary()
  {
    return runtime().musicLibrary();
  }

  rt::Library& CliContext::library()
  {
    return runtime().library();
  }
} // namespace ao::cli
