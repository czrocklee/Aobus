// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliRuntime.h"

#include "CommandError.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibrarySchemaPolicy.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryDurability.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/utility/PlatformDirectories.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <utility>

#ifdef _WIN32
#include <ao/utility/FileAccessPath.h>

#include <system_error>
#endif

namespace ao::cli
{
  struct CliRuntime::Storage final
  {
    std::optional<rt::CoreRuntime> optRuntime;
    async::LoopExecutor* loopExecutor = nullptr;
  };

  namespace
  {
#ifdef _WIN32
    bool hasWindowsNamespacePrefix(std::filesystem::path const& path)
    {
      auto const& name = path.native();
      auto const isSeparator = [](wchar_t unit) { return unit == L'\\' || unit == L'/'; };
      return name.size() >= 3 && isSeparator(name[0]) && isSeparator(name[1]) && (name[2] == L'?' || name[2] == L'.') &&
             (name.size() == 3 || isSeparator(name[3]));
    }

    void validateWindowsRootSpelling(std::filesystem::path const& root)
    {
      if (hasWindowsNamespacePrefix(root))
      {
        throwCommandError(Error::Code::InvalidInput,
                          "failed to open library: Windows namespace paths are not supported for the library root; "
                          "use an ordinary path on a local drive without a namespace prefix");
      }

      if (auto const spellingRes = utility::checkedFileSpelling(root); !spellingRes)
      {
        throwCommandError(
          spellingRes.error(), "failed to open library: invalid library root: {}", spellingRes.error().message);
      }
    }

    void validateWindowsLibraryRoot(std::filesystem::path const& root)
    {
      validateWindowsRootSpelling(root);

      if (!root.is_absolute())
      {
        auto ec = std::error_code{};
        auto const cwd = std::filesystem::current_path(ec);

        if (ec)
        {
          throwCommandError(
            Error::Code::IoError, "failed to open library: cannot determine the current directory: {}", ec.message());
        }

        validateWindowsRootSpelling(cwd / root);
      }
    }
#endif

    struct TaskCompletionState final
    {
      async::Executor& executor;
      bool completed = false;
    };

    class [[nodiscard]] TaskCompletionGuard final
    {
    public:
      explicit TaskCompletionGuard(std::weak_ptr<TaskCompletionState> statePtr) noexcept
        : _statePtr{std::move(statePtr)}
      {
      }

      TaskCompletionGuard(TaskCompletionGuard const&) = delete;
      TaskCompletionGuard& operator=(TaskCompletionGuard const&) = delete;
      // Moving the weak token disarms the source guard.
      TaskCompletionGuard(TaskCompletionGuard&&) noexcept = default;
      TaskCompletionGuard& operator=(TaskCompletionGuard&&) = delete;

      ~TaskCompletionGuard() noexcept
      {
        // Late frame cleanup must not touch the executor after runTask exits.
        if (auto statePtr = _statePtr.lock(); statePtr != nullptr)
        {
          try
          {
            auto& executor = statePtr->executor;
            executor.dispatch([statePtr = std::move(statePtr)] { statePtr->completed = true; });
          }
          catch (...)
          {
            // Without this notification the owner loop cannot finish runTask.
            AO_FATAL_EXCEPTION(std::current_exception(), "CLI task completion dispatch");
          }
        }
      }

    private:
      std::weak_ptr<TaskCompletionState> _statePtr;
    };

    async::Task<void> publishTaskCompletionAsync(async::Task<void> task,
                                                 [[maybe_unused]] TaskCompletionGuard notifyCompletion)
    {
      co_await std::move(task);
    }
  } // namespace

  CliRuntime::CliRuntime(std::ostream& out,
                         std::ostream& err,
                         std::uint64_t const musicLibraryPinnedMapBytes,
                         rt::LibraryDurability const musicLibraryDurability,
                         std::optional<std::filesystem::path> optCacheDirectory)
    : _io{.out = out, .err = err}
    , _musicLibraryPinnedMapBytes{musicLibraryPinnedMapBytes}
    , _musicLibraryDurability{musicLibraryDurability}
    , _optCacheDirectory{std::move(optCacheDirectory)}
    , _storagePtr{std::make_unique<Storage>()}
  {
  }

  CliRuntime::~CliRuntime()
  {
    if (!_storagePtr->optRuntime)
    {
      return;
    }

    _storagePtr->optRuntime->shutdown();

    while (_storagePtr->loopExecutor->tryRunReadyTurn())
    {
    }

    _storagePtr->optRuntime.reset();
    _storagePtr->loopExecutor = nullptr;
  }

  rt::CoreRuntime& CliRuntime::core()
  {
    return openCore(library::LibrarySchemaPolicy::InitializeIfEmpty);
  }

  rt::CoreRuntime& CliRuntime::openCore(library::LibrarySchemaPolicy const schemaPolicy)
  {
    if (!_storagePtr->optRuntime)
    {
      if (_options.root.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "failed to open library: the library root must not be empty");
      }

#ifdef _WIN32
      // The root also names the database. Check it before opening storage,
      // without rewriting the spelling that the storage layer receives.
      validateWindowsLibraryRoot(_options.root);
#endif
      // CliRuntime is invocation-thread confined; lazy construction binds the
      // loop executor to the thread that enters the first command callback.
      auto executorPtr = std::make_unique<async::LoopExecutor>();
      auto* const loopExecutor = executorPtr.get();

      // This is the only platform location the CLI resolves; it resolves its
      // music root and nothing else. An absent one costs nothing: cover reads
      // then re-extract from the media files on every request.
      auto const cacheDirectory = [this]
      {
        if (_optCacheDirectory)
        {
          return *_optCacheDirectory;
        }

        auto const cacheDirRes = utility::applicationCacheDirectory();
        return cacheDirRes ? *cacheDirRes : std::filesystem::path{};
      }();
      auto runtimeRes = rt::CoreRuntime::create(std::move(executorPtr),
                                                _options.root,
                                                rt::LibraryPaths{_options.root}.databasePath(),
                                                cacheDirectory,
                                                _musicLibraryPinnedMapBytes,
                                                _musicLibraryDurability,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                schemaPolicy);

      if (!runtimeRes)
      {
        throwCommandError(runtimeRes.error(), "failed to open library: {}", runtimeRes.error().message);
      }

      _storagePtr->optRuntime.emplace(std::move(*runtimeRes));
      _storagePtr->loopExecutor = loopExecutor;
    }

    return *_storagePtr->optRuntime;
  }

  library::MusicLibrary const& CliRuntime::musicLibrary()
  {
    return core().musicLibrary();
  }

  rt::Library const& CliRuntime::library()
  {
    return core().library();
  }

  rt::LibraryJobs& CliRuntime::existingLibraryJobs()
  {
    return openCore(library::LibrarySchemaPolicy::RequireExisting).libraryJobs();
  }

  void CliRuntime::runTask(async::Task<void> task)
  {
    auto& asyncRuntime = core().async();
    auto const completionStatePtr = std::make_shared<TaskCompletionState>(asyncRuntime.callbackExecutor());
    // Arm the guard in the coroutine frame before spawn can reject the unstarted wrapper.
    auto completionFuture =
      asyncRuntime.spawn(publishTaskCompletionAsync(std::move(task), TaskCompletionGuard{completionStatePtr}));

    while (!completionStatePtr->completed)
    {
      _storagePtr->loopExecutor->runOneTurn();
    }

    completionFuture.get();
  }
} // namespace ao::cli
