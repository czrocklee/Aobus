// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Substrate.h"

#include "council/Hash.h"
#include "council/Model.h"
#include "council/ProcessRunner.h"
#include "council/Serialization.h"
#include <ao/Error.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <print>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::council
{
  namespace
  {
    constexpr auto kDefaultLocalTimeout = std::chrono::minutes{20};
    constexpr auto kTransientPaths = std::to_array<std::string_view>({".cache", "build", "logs"});
    constexpr auto kSandboxHome = std::string_view{"/tmp/aobus-home"};
    constexpr auto kSandboxConfigHome = std::string_view{"/tmp/aobus-home/.config"};
    constexpr auto kSandboxCodexHome = std::string_view{"/tmp/aobus-home/.codex"};
    constexpr auto kSandboxOpencodeConfig = std::string_view{"/tmp/aobus-home/.config/opencode"};
    constexpr auto kSandboxToolPath = std::string_view{"/tmp/aobus-tools"};
    constexpr auto kSandboxPathName = "PATH";
    constexpr auto kRuntimeReadOnlyPaths = std::to_array<std::string_view>({
      "/nix",
      "/usr",
      "/bin",
      "/lib",
      "/lib64",
      "/etc/ssl",
      "/etc/static/ssl",
      "/etc/ca-certificates",
      "/etc/resolv.conf",
      "/etc/hosts",
      "/etc/passwd",
      "/etc/group",
      "/etc/nsswitch.conf",
    });
    constexpr auto kReviewToolNames = std::to_array<std::string_view>({
      "git",
      "rg",
      "sed",
      "awk",
      "grep",
      "find",
      "sort",
      "head",
      "tail",
      "wc",
      "nl",
      "cat",
      "bash",
      "sh",
      "nix-shell",
    });

    void printWarningNoexcept(std::string_view message) noexcept
    {
      try
      {
        std::println(stderr, "warning: {}", message);
      }
      catch (...)
      {
        std::fputs("warning: cleanup warning could not be written\n", stderr);
      }
    }

    template<typename... Args>
    void printFormattedWarningNoexcept(std::format_string<Args...> fmt, Args&&... args) noexcept
    {
      try
      {
        printWarningNoexcept(std::format(fmt, std::forward<Args>(args)...));
      }
      catch (...)
      {
        std::fputs("warning: cleanup warning could not be written\n", stderr);
      }
    }

    bool ignoredCanaryPath(std::filesystem::path const& relative)
    {
      if (relative.empty())
      {
        return false;
      }

      auto const first = relative.begin()->generic_string();
      return std::ranges::contains(kTransientPaths, first);
    }

    std::vector<std::string> transientExcludeArguments()
    {
      auto result = std::vector<std::string>{};

      for (auto const path : kTransientPaths)
      {
        result.push_back(std::format("--exclude=/{}", path));
      }

      return result;
    }

    ProcessRequest localRequest(std::vector<std::string> argv, std::filesystem::path const& cwd)
    {
      return ProcessRequest{
        .argv = std::move(argv),
        .cwd = cwd,
        .standardInput = {},
        .environmentWhitelist = {"PATH", "HOME", "USER", "TMPDIR", "NIX_PATH", "GIT_CONFIG_NOSYSTEM"},
        .environment = {},
        .timeout = kDefaultLocalTimeout,
        .terminationGracePeriod = std::chrono::seconds{2},
      };
    }

    Result<> requireSuccess(ProcessResult const& result, std::string_view operation)
    {
      if (result.status != ProcessStatus::Exited || result.exitCode != 0)
      {
        return makeError(Error::Code::IoError, std::format("{} failed: {}", operation, result.standardError));
      }

      return {};
    }

    void appendDirectory(std::vector<std::string>& argv,
                         std::set<std::string, std::less<>>& directories,
                         std::filesystem::path const& path)
    {
      if (path.empty())
      {
        return;
      }

      auto normalized = path.lexically_normal().string();

      if (normalized == "/")
      {
        return;
      }

      if (directories.insert(normalized).second)
      {
        argv.insert(argv.end(), {"--dir", std::move(normalized)});
      }
    }

    void appendParentDirectories(std::vector<std::string>& argv,
                                 std::set<std::string, std::less<>>& directories,
                                 std::filesystem::path const& path)
    {
      auto const parent = path.lexically_normal().parent_path();

      if (!parent.is_absolute())
      {
        return;
      }

      auto current = std::filesystem::path{};

      for (auto const& part : parent)
      {
        if (part == "/")
        {
          current = part;
          continue;
        }

        current /= part;
        appendDirectory(argv, directories, current);
      }
    }

    void appendReadOnlyBindIfExists(std::vector<std::string>& argv,
                                    std::set<std::string, std::less<>>& directories,
                                    std::filesystem::path const& path)
    {
      if (!std::filesystem::exists(path))
      {
        return;
      }

      appendParentDirectories(argv, directories, path);
      argv.insert(argv.end(), {"--ro-bind", path.string(), path.string()});
    }

    std::filesystem::path envPath(char const* name)
    {
      auto const* value = std::getenv(name);
      return value == nullptr || *value == '\0' ? std::filesystem::path{} : std::filesystem::path{value};
    }

    std::filesystem::path hostHome()
    {
      return envPath("HOME");
    }

    std::string sandboxPath()
    {
      auto result = std::string{kSandboxToolPath};

      if (auto const* value = std::getenv(kSandboxPathName); value != nullptr && *value != '\0')
      {
        result += ':';
        result += value;
      }

      return result;
    }

    bool hasDirectorySeparator(std::string const& value)
    {
      return value.find('/') != std::string::npos;
    }

    std::filesystem::path canonicalExecutable(std::filesystem::path const& path)
    {
      auto error = std::error_code{};

      if (!std::filesystem::exists(path, error) || error)
      {
        return {};
      }

      auto canonical = std::filesystem::weakly_canonical(path, error);
      return error ? path : canonical;
    }

    std::filesystem::path resolveExecutable(std::string const& executable)
    {
      if (executable.empty())
      {
        return {};
      }

      if (hasDirectorySeparator(executable))
      {
        return canonicalExecutable(executable);
      }

      auto const* pathValue = std::getenv("PATH");

      if (pathValue == nullptr)
      {
        return {};
      }

      auto pathList = std::string_view{pathValue};

      while (!pathList.empty())
      {
        auto const separator = pathList.find(':');

        if (auto const directory = pathList.substr(0, separator); !directory.empty())
        {
          auto candidate = canonicalExecutable(std::filesystem::path{directory} / executable);

          if (!candidate.empty())
          {
            return candidate;
          }
        }

        if (separator == std::string_view::npos)
        {
          break;
        }

        pathList.remove_prefix(separator + 1);
      }

      return {};
    }

    bool pathIsUnder(std::filesystem::path const& child, std::filesystem::path const& parent)
    {
      auto normalizedChild = child.lexically_normal();
      auto normalizedParent = parent.lexically_normal();
      auto childIt = normalizedChild.begin();

      for (auto const& parentComponent : normalizedParent)
      {
        if (childIt == normalizedChild.end() || *childIt != parentComponent)
        {
          return false;
        }

        ++childIt;
      }

      return true;
    }

    bool alreadyRuntimeMounted(std::filesystem::path const& path)
    {
      return std::ranges::any_of(
        kRuntimeReadOnlyPaths, [&](auto const mounted) { return pathIsUnder(path, std::filesystem::path{mounted}); });
    }

    std::filesystem::path resolveAgentExecutable(std::vector<std::string>& argv)
    {
      if (argv.empty())
      {
        return {};
      }

      if (auto resolved = resolveExecutable(argv.front()); !resolved.empty())
      {
        argv.front() = resolved.string();
        return resolved;
      }

      return {};
    }

    void appendReviewToolBinds(std::vector<std::string>& argv, std::set<std::string, std::less<>>& directories)
    {
      appendDirectory(argv, directories, std::filesystem::path{kSandboxToolPath});

      for (auto const name : kReviewToolNames)
      {
        auto const resolved = resolveExecutable(std::string{name});

        if (resolved.empty())
        {
          continue;
        }

        auto const sandboxTool = std::filesystem::path{kSandboxToolPath} / name;
        argv.insert(argv.end(), {"--ro-bind", resolved.string(), sandboxTool.string()});
      }
    }

    Result<> removeTransientPaths(std::filesystem::path const& root)
    {
      for (auto const path : kTransientPaths)
      {
        auto error = std::error_code{};
        std::filesystem::remove_all(root / path, error);

        if (error)
        {
          return makeError(
            Error::Code::IoError, std::format("cannot prune transient path '{}': {}", path, error.message()));
        }
      }

      return {};
    }

    // A snapshot copies the parent repo's `.git` verbatim, so any lock file the parent held mid-operation
    // (most notably `index.lock`) is duplicated into the base. Those stale locks would make the base's own
    // `git add`/`git commit` fail immediately. Strip them before touching git in the snapshot.
    void removeStaleGitLocks(std::filesystem::path const& gitDir)
    {
      try
      {
        if (!std::filesystem::exists(gitDir))
        {
          return;
        }

        auto error = std::error_code{};

        for (auto const name : std::to_array<std::string_view>(
               {"index.lock", "HEAD.lock", "config.lock", "packed-refs.lock", "shallow.lock", "ORIG_HEAD.lock"}))
        {
          std::filesystem::remove(gitDir / name, error);
        }

        for (auto const subtree : std::to_array<std::string_view>({"refs", "logs"}))
        {
          auto const base = gitDir / subtree;

          if (!std::filesystem::exists(base))
          {
            continue;
          }

          auto const options = std::filesystem::directory_options::skip_permission_denied;

          for (auto const& entry : std::filesystem::recursive_directory_iterator{base, options})
          {
            if (entry.is_regular_file() && entry.path().extension() == ".lock")
            {
              std::filesystem::remove(entry.path(), error);
            }
          }
        }
      }
      catch (std::exception const& exception)
      {
        std::println(stderr, "warning: could not strip stale git locks: {}", exception.what());
      }
    }
  } // namespace

  Result<TreeFingerprint> TreeCanary::fingerprint(std::filesystem::path const& root)
  {
    try
    {
      auto entries = std::vector<std::filesystem::path>{};
      auto options = std::filesystem::directory_options::skip_permission_denied;

      for (auto iterator = std::filesystem::recursive_directory_iterator{root, options};
           iterator != std::filesystem::recursive_directory_iterator{};
           ++iterator)
      {
        auto const relative = std::filesystem::relative(iterator->path(), root);

        if (ignoredCanaryPath(relative))
        {
          if (iterator->is_directory())
          {
            iterator.disable_recursion_pending();
          }

          continue;
        }

        entries.emplace_back(relative);
      }

      std::ranges::sort(entries, {}, [](auto const& path) { return path.generic_string(); });
      auto hash = Fnv1a64{};

      for (auto const& relative : entries)
      {
        auto const absolute = root / relative;
        auto const status = std::filesystem::symlink_status(absolute);
        hash.mix(relative.generic_string());
        hash.mix(std::format(
          ":{}:{}:", static_cast<std::int32_t>(status.type()), static_cast<std::uint32_t>(status.permissions())));

        if (std::filesystem::is_symlink(status))
        {
          hash.mix(std::filesystem::read_symlink(absolute).generic_string());
        }
        else if (std::filesystem::is_regular_file(status))
        {
          if (auto result = hash.mixFile(absolute); !result)
          {
            return std::unexpected{result.error()};
          }
        }
      }

      return TreeFingerprint{.value = hash.hex(), .entryCount = entries.size()};
    }
    catch (std::exception const& exception)
    {
      return makeError(Error::Code::IoError, exception.what());
    }
  }

  SnapshotProvider::SnapshotProvider(IProcessRunner& runner)
    : _runner{runner}
  {
  }

  Result<std::filesystem::path> SnapshotProvider::createImmutableBase(std::filesystem::path const& repo,
                                                                      std::filesystem::path const& destination)
  {
    if (std::filesystem::exists(destination))
    {
      remove(destination);

      if (std::filesystem::exists(destination))
      {
        return makeError(
          Error::Code::IoError,
          std::format("stale base snapshot at {} could not be removed; remove it manually", destination.string()));
      }
    }

    auto error = std::error_code{};
    std::filesystem::create_directories(destination.parent_path(), error);

    if (error)
    {
      return makeError(Error::Code::IoError, error.message());
    }

    auto btrfsCheck = _runner.run(localRequest({"btrfs", "subvolume", "show", repo.string()}, repo));
    bool btrfsSnapshot = false;

    if (btrfsCheck.status == ProcessStatus::Exited && btrfsCheck.exitCode == 0)
    {
      auto snapshot =
        _runner.run(localRequest({"btrfs", "subvolume", "snapshot", repo.string(), destination.string()}, repo));
      btrfsSnapshot = snapshot.status == ProcessStatus::Exited && snapshot.exitCode == 0;
    }

    if (!btrfsSnapshot)
    {
      auto argv = std::vector<std::string>{"rsync", "-a", "--delete"};
      auto excludes = transientExcludeArguments();
      argv.insert(argv.end(), excludes.begin(), excludes.end());
      argv.push_back(repo.string() + "/");
      argv.push_back(destination.string() + "/");
      auto sync = _runner.run(localRequest(std::move(argv), repo));

      if (auto success = requireSuccess(sync, "snapshot copy"); !success)
      {
        return std::unexpected{success.error()};
      }
    }
    else if (auto pruned = removeTransientPaths(destination); !pruned)
    {
      return std::unexpected{pruned.error()};
    }

    removeStaleGitLocks(destination / ".git");

    auto head =
      _runner.run(localRequest({"git", "-C", destination.string(), "rev-parse", "--verify", "HEAD"}, destination));

    if (head.status == ProcessStatus::Exited && head.exitCode == 0)
    {
      auto baseRef = _runner.run(localRequest(
        {"git", "-C", destination.string(), "update-ref", std::string{kReviewBaseRef}, "HEAD"}, destination));

      if (auto success = requireSuccess(baseRef, "snapshot review base"); !success)
      {
        return std::unexpected{success.error()};
      }
    }

    auto add = _runner.run(localRequest({"git", "-C", destination.string(), "add", "-A"}, destination));

    if (auto success = requireSuccess(add, "snapshot index"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto commit = _runner.run(localRequest({"git",
                                            "-C",
                                            destination.string(),
                                            "-c",
                                            "core.hooksPath=/dev/null",
                                            "-c",
                                            "user.name=Aobus Council",
                                            "-c",
                                            "user.email=council@localhost",
                                            "commit",
                                            "--allow-empty",
                                            "--no-gpg-sign",
                                            "-m",
                                            "council review snapshot"},
                                           destination));

    if (auto success = requireSuccess(commit, "snapshot commit"); !success)
    {
      return std::unexpected{success.error()};
    }

    if (btrfsSnapshot)
    {
      auto readOnly = _runner.run(localRequest(
        {"btrfs", "property", "set", "-ts", destination.string(), "ro", "true"}, destination.parent_path()));

      if (auto success = requireSuccess(readOnly, "immutable snapshot seal"); !success)
      {
        return std::unexpected{success.error()};
      }
    }

    return destination;
  }

  Result<std::filesystem::path> SnapshotProvider::createWorkspace(std::filesystem::path const& base,
                                                                  std::filesystem::path const& destination)
  {
    auto error = std::error_code{};
    std::filesystem::create_directories(destination.parent_path(), error);

    if (error)
    {
      return makeError(Error::Code::IoError, error.message());
    }

    auto sync =
      _runner.run(localRequest({"rsync", "-a", "--delete", base.string() + "/", destination.string() + "/"}, base));

    if (auto success = requireSuccess(sync, "workspace copy"); !success)
    {
      return std::unexpected{success.error()};
    }

    return destination;
  }

  void SnapshotProvider::remove(std::filesystem::path const& path) noexcept
  {
    try
    {
      if (!std::filesystem::exists(path))
      {
        return;
      }

      auto check = _runner.run(localRequest({"btrfs", "subvolume", "show", path.string()}, path.parent_path()));

      if (check.status == ProcessStatus::Exited && check.exitCode == 0)
      {
        [[maybe_unused]] auto const unseal = _runner.run(
          localRequest({"btrfs", "property", "set", "-ts", path.string(), "ro", "false"}, path.parent_path()));
        auto removed = _runner.run(localRequest({"btrfs", "subvolume", "delete", path.string()}, path.parent_path()));

        if (removed.status == ProcessStatus::Exited && removed.exitCode == 0)
        {
          return;
        }

        printFormattedWarningNoexcept("btrfs subvolume delete failed for {}: {}", path.string(), removed.standardError);
      }

      auto error = std::error_code{};
      std::filesystem::remove_all(path, error);

      if (error)
      {
        printFormattedWarningNoexcept("cannot remove {}: {}", path.string(), error.message());
      }
    }
    catch (...)
    {
      printWarningNoexcept("snapshot cleanup failed");
    }
  }

  NamespaceRunner::NamespaceRunner(IProcessRunner& runner)
    : _runner{runner}
  {
  }

  ProcessResult NamespaceRunner::run(std::filesystem::path const& realRepo,
                                     std::filesystem::path const& workspace,
                                     SandboxMounts const& mounts,
                                     ProcessRequest request)
  {
    auto const home = hostHome();

    if (home.empty() || !std::filesystem::exists(home))
    {
      return ProcessResult{
        .status = ProcessStatus::LaunchFailed, .standardError = "HOME is not available for council sandbox"};
    }

    auto directories = std::set<std::string, std::less<>>{};
    auto argv = std::vector<std::string>{"bwrap",
                                         "--die-with-parent",
                                         "--new-session",
                                         "--unshare-pid",
                                         "--unshare-ipc",
                                         "--unshare-uts",
                                         "--tmpfs",
                                         "/tmp"};

    appendParentDirectories(argv, directories, std::filesystem::path{kSandboxHome});
    appendDirectory(argv, directories, std::filesystem::path{kSandboxHome});
    argv.insert(argv.end(), {"--bind", home.string(), std::string{kSandboxHome}});
    appendReviewToolBinds(argv, directories);

    argv.insert(argv.end(),
                {"--setenv",
                 "HOME",
                 std::string{kSandboxHome},
                 "--setenv",
                 "XDG_CONFIG_HOME",
                 std::string{kSandboxConfigHome},
                 "--setenv",
                 "CODEX_HOME",
                 std::string{kSandboxCodexHome},
                 "--setenv",
                 "OPENCODE_CONFIG_DIR",
                 std::string{kSandboxOpencodeConfig},
                 "--setenv",
                 std::string{kSandboxPathName},
                 sandboxPath()});

    for (auto const path : kRuntimeReadOnlyPaths)
    {
      appendReadOnlyBindIfExists(argv, directories, std::filesystem::path{path});
    }

    argv.insert(argv.end(), {"--dev", "/dev", "--proc", "/proc"});
    appendParentDirectories(argv, directories, realRepo);
    argv.insert(argv.end(), {"--bind", workspace.string(), realRepo.string()});

    for (auto const& [host, guest] : mounts.writableBinds)
    {
      appendParentDirectories(argv, directories, guest);
      argv.insert(argv.end(), {"--bind", host.string(), guest.string()});
    }

    auto const resolvedExecutable = resolveAgentExecutable(request.argv);

    if (!resolvedExecutable.empty() && !alreadyRuntimeMounted(resolvedExecutable))
    {
      appendReadOnlyBindIfExists(argv, directories, resolvedExecutable);
    }

    argv.insert(argv.end(), {"--chdir", realRepo.string()});
    argv.insert(argv.end(), request.argv.begin(), request.argv.end());
    request.argv = std::move(argv);
    request.cwd = workspace;
    return _runner.run(request);
  }

  ArtifactStore::ArtifactStore(std::filesystem::path root)
    : _root{std::move(root)}
  {
  }

  std::filesystem::path const& ArtifactStore::root() const noexcept
  {
    return _root;
  }

  Result<> ArtifactStore::write(std::filesystem::path const& relativePath, std::string_view content) const
  {
    auto const path = _root / relativePath;
    auto error = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), error);

    if (error)
    {
      return makeError(Error::Code::IoError, error.message());
    }

    auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};

    if (!file.is_open())
    {
      return makeError(Error::Code::IoError, std::format("cannot write {}", path.string()));
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));

    if (!file)
    {
      return makeError(Error::Code::IoError, std::format("cannot write {}", path.string()));
    }

    file.flush();

    if (!file)
    {
      return makeError(Error::Code::IoError, std::format("cannot flush {}", path.string()));
    }

    file.close();

    if (!file)
    {
      return makeError(Error::Code::IoError, std::format("cannot close {}", path.string()));
    }

    return {};
  }

  Result<> ArtifactStore::append(std::filesystem::path const& relativePath, std::string_view document) const
  {
    return appendYamlDocument(_root / relativePath, document);
  }
} // namespace ao::council
