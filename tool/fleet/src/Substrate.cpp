// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Hash.h"
#include <ao/Error.h>
#include <ao/fleet/Model.h>
#include <ao/fleet/ProcessRunner.h>
#include <ao/fleet/Serialization.h>
#include <ao/fleet/Substrate.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    constexpr auto kDefaultLocalTimeout = std::chrono::minutes{20};

    // Single source of truth for transient top-level repository entries: the canary ignores
    // them and snapshot copies exclude them, so the two views can never drift apart.
    constexpr auto kTransientPaths = std::array<std::string_view, 3>{".cache", "build", "logs"};

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
        // Anchored at the transfer root to mirror the canary's top-level-component check.
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
        .terminationGrace = std::chrono::seconds{2},
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

    std::filesystem::path normalizeDiffPath(std::string_view path)
    {
      auto result = std::string{path};

      if (result.starts_with("a/") || result.starts_with("b/"))
      {
        result.erase(0, 2);
      }

      return std::filesystem::path{result}.lexically_normal();
    }

    bool pathCovered(std::filesystem::path const& candidate, std::filesystem::path const& ruler)
    {
      auto const normalizedCandidate = candidate.lexically_normal().generic_string();
      auto normalizedRuler = ruler.lexically_normal().generic_string();

      if (!normalizedRuler.ends_with('/'))
      {
        normalizedRuler += '/';
      }

      return normalizedCandidate == ruler.lexically_normal().generic_string() ||
             normalizedCandidate.starts_with(normalizedRuler);
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
        hash.mix(std::format(":{}:", static_cast<std::uint32_t>(status.permissions())));

        if (std::filesystem::is_symlink(status))
        {
          hash.mix(std::filesystem::read_symlink(absolute).generic_string());
        }
        else if (std::filesystem::is_regular_file(status))
        {
          hash.mixFile(absolute);
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
    // A leftover base from a crashed run would make `btrfs subvolume snapshot` nest the new
    // snapshot inside the stale directory while still exiting 0, silently producing a wrong base.
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
    auto btrfsSnapshot = false;

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

    auto add = _runner.run(localRequest({"git", "-C", destination.string(), "add", "-A"}, destination));

    if (auto success = requireSuccess(add, "snapshot index"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto commitRequest = localRequest({"git",
                                       "-C",
                                       destination.string(),
                                       "-c",
                                       "core.hooksPath=/dev/null",
                                       "-c",
                                       "user.name=Aobus Fleet",
                                       "-c",
                                       "user.email=fleet@localhost",
                                       "commit",
                                       "--allow-empty",
                                       "--no-gpg-sign",
                                       "-m",
                                       "fleet immutable baseline"},
                                      destination);
    auto commit = _runner.run(commitRequest);

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
        // A sealed base snapshot is read-only and rejects both subvolume delete and remove_all.
        [[maybe_unused]] auto const unseal = _runner.run(
          localRequest({"btrfs", "property", "set", "-ts", path.string(), "ro", "false"}, path.parent_path()));
        auto removed = _runner.run(localRequest({"btrfs", "subvolume", "delete", path.string()}, path.parent_path()));

        if (removed.status == ProcessStatus::Exited && removed.exitCode == 0)
        {
          return;
        }

        std::cerr << "warning: btrfs subvolume delete failed for " << path.string() << ": " << removed.standardError
                  << '\n';
      }

      auto error = std::error_code{};
      std::filesystem::remove_all(path, error);

      if (error)
      {
        std::cerr << "warning: cannot remove " << path.string() << ": " << error.message() << '\n';
      }
    }
    catch (...)
    {
      // Cleanup must not throw; keep the fallback warning allocation-free.
      std::cerr << "warning: snapshot cleanup failed\n";
      return;
    }
  }

  NamespaceRunner::NamespaceRunner(IProcessRunner& runner)
    : _runner{runner}
  {
  }

  ProcessResult NamespaceRunner::run(std::filesystem::path const& realRepo,
                                     std::filesystem::path const& workspace,
                                     AuthorityPolicy const& authority,
                                     SandboxMounts const& mounts,
                                     ProcessRequest request)
  {
    auto argv = std::vector<std::string>{"bwrap",
                                         "--die-with-parent",
                                         "--new-session",
                                         "--ro-bind",
                                         "/",
                                         "/",
                                         "--dev-bind",
                                         "/dev",
                                         "/dev",
                                         "--proc",
                                         "/proc",
                                         "--tmpfs",
                                         "/tmp"};

    if (authority.network == NetworkAuthority::Off)
    {
      argv.emplace_back("--unshare-net");
    }

    // Before the workspace bind: when the repository lives under $HOME, the later workspace
    // bind must still override that subtree with the isolated copy.
    if (mounts.bindHome)
    {
      if (auto const* home = std::getenv("HOME"); home != nullptr && *home != '\0')
      {
        argv.emplace_back("--bind");
        argv.emplace_back(home);
        argv.emplace_back(home);
      }
    }

    argv.emplace_back(authority.filesystem == FilesystemAuthority::ReadOnly ? "--ro-bind" : "--bind");
    argv.emplace_back(workspace.string());
    argv.emplace_back(realRepo.string());

    for (auto const& [host, sandbox] : mounts.writableBinds)
    {
      argv.emplace_back("--bind");
      argv.emplace_back(host.string());
      argv.emplace_back(sandbox.string());
    }

    argv.emplace_back("--chdir");
    argv.emplace_back(realRepo.string());
    argv.insert(argv.end(), request.argv.begin(), request.argv.end());
    request.argv = std::move(argv);
    request.cwd = workspace;
    return _runner.run(request);
  }

  PatchExtractor::PatchExtractor(IProcessRunner& runner)
    : _runner{runner}
  {
  }

  Result<PatchArtifact> PatchExtractor::extract(std::filesystem::path const& workspace, std::string candidateId)
  {
    auto intentToAdd = _runner.run(localRequest({"git", "-C", workspace.string(), "add", "-N", "--", "."}, workspace));

    if (auto success = requireSuccess(intentToAdd, "untracked-file discovery"); !success)
    {
      return std::unexpected{success.error()};
    }

    // --no-renames keeps rename/copy detection off regardless of the user git configuration,
    // so every change decomposes into plain add/delete/modify entries.
    auto patchResult = _runner.run(localRequest(
      {"git", "-C", workspace.string(), "diff", "--binary", "--no-ext-diff", "--no-renames", "HEAD", "--"}, workspace));

    if (auto success = requireSuccess(patchResult, "patch extraction"); !success)
    {
      return std::unexpected{success.error()};
    }

    // -z separates entries with NUL and disables path quoting, so paths with spaces, tabs, or
    // non-ASCII bytes parse exactly.
    auto statusResult = _runner.run(localRequest(
      {"git", "-C", workspace.string(), "diff", "--name-status", "--no-renames", "-z", "HEAD", "--"}, workspace));

    if (auto success = requireSuccess(statusResult, "patch status extraction"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto statResult = _runner.run(
      localRequest({"git", "-C", workspace.string(), "diff", "--numstat", "--no-renames", "HEAD", "--"}, workspace));

    if (auto success = requireSuccess(statResult, "patch statistics extraction"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto result = PatchArtifact{.candidateId = std::move(candidateId),
                                .patch = std::move(patchResult.standardOutput),
                                .touchedFiles = {},
                                .addedLines = 0,
                                .removedLines = 0};

    // NUL-delimited stream of alternating status and path tokens: "A\0path\0M\0path\0...".
    auto const& statusOutput = statusResult.standardOutput;

    for (std::size_t position = 0; position < statusOutput.size();)
    {
      auto statusEnd = statusOutput.find('\0', position);

      if (statusEnd == std::string::npos)
      {
        break;
      }

      auto pathEnd = statusOutput.find('\0', statusEnd + 1);

      if (pathEnd == std::string::npos)
      {
        pathEnd = statusOutput.size();
      }

      auto const status = std::string_view{statusOutput}.substr(position, statusEnd - position);
      auto const path = std::string_view{statusOutput}.substr(statusEnd + 1, pathEnd - statusEnd - 1);
      position = pathEnd + 1;

      if (status.empty() || path.empty())
      {
        continue;
      }

      auto operation = ScopeOperation::Modify;

      switch (status.front())
      {
        case 'A': operation = ScopeOperation::Create; break;
        case 'D': operation = ScopeOperation::Delete; break;
        case 'M':
        case 'T': operation = ScopeOperation::Modify; break;
        default:
          // Renames and copies are disabled above; anything else is unexpected and unsafe to map.
          return makeError(Error::Code::NotSupported, std::format("unsupported diff status '{}' for {}", status, path));
      }

      result.touchedFiles.push_back(TouchedFile{.path = normalizeDiffPath(path), .operation = operation});
    }

    auto row = std::string{};
    auto statStream = std::istringstream{statResult.standardOutput};

    while (std::getline(statStream, row))
    {
      auto first = row.find('\t');
      auto second = first == std::string::npos ? first : row.find('\t', first + 1);

      if (first == std::string::npos || second == std::string::npos)
      {
        continue;
      }

      auto parseCount = [](std::string_view text)
      {
        auto value = std::size_t{};
        auto conversion = std::from_chars(text.data(), text.data() + text.size(), value);
        return conversion.ec == std::errc{} ? value : std::size_t{};
      };
      result.addedLines += parseCount(std::string_view{row}.substr(0, first));
      result.removedLines += parseCount(std::string_view{row}.substr(first + 1, second - first - 1));
    }

    return result;
  }

  Result<> PatchExtractor::apply(std::filesystem::path const& workspace, PatchArtifact const& patch)
  {
    auto request =
      localRequest({"git", "-C", workspace.string(), "apply", "--binary", "--whitespace=nowarn", "-"}, workspace);
    request.standardInput = patch.patch;
    auto applied = _runner.run(request);

    if (auto success = requireSuccess(applied, "patch apply"); !success)
    {
      return std::unexpected{success.error()};
    }

    return {};
  }

  PatchGuardResult PatchGuard::inspect(PatchArtifact const& patch,
                                       std::vector<ScopeRule> const& scope,
                                       std::size_t churnLimit,
                                       std::vector<std::filesystem::path> const& rulerPaths)
  {
    if (patch.patch.empty() || patch.touchedFiles.empty())
    {
      return {.failure = FailureReason::NoCandidate, .detail = "candidate produced no patch"};
    }

    if (patch.addedLines + patch.removedLines > churnLimit)
    {
      return {.failure = FailureReason::ChurnExceeded, .detail = "candidate exceeds churn limit"};
    }

    // Unified diff content lines always carry a +/-/space prefix, so a bare marker at the
    // start of a line can only be a diff header; patch text merely containing these words
    // (for example in added source lines) must not trip the guard.
    constexpr auto kForbiddenHeaderPrefixes = std::array<std::string_view, 6>{
      "old mode ",
      "new mode ",
      "new file mode 120000",
      "deleted file mode 120000",
      "rename from ",
      "copy from ",
    };
    auto const hasForbiddenHeader = [&]
    {
      auto const text = std::string_view{patch.patch};

      for (std::size_t lineStart = 0; lineStart < text.size();)
      {
        if (auto const line = text.substr(lineStart); std::ranges::any_of(
              kForbiddenHeaderPrefixes, [&](std::string_view prefix) { return line.starts_with(prefix); }))
        {
          return true;
        }

        auto const lineEnd = text.find('\n', lineStart);
        lineStart = lineEnd == std::string_view::npos ? text.size() : lineEnd + 1;
      }

      return false;
    };

    if (hasForbiddenHeader())
    {
      return {.failure = FailureReason::ScopeViolation, .detail = "mode and symlink changes are forbidden"};
    }

    for (auto const& [path, operation] : patch.touchedFiles)
    {
      // Restores the old fleet's `.*CMakeLists\.txt` rule: build wiring is ruler-protected at
      // every directory level, not only at the paths listed in the registry.
      if (path.filename() == "CMakeLists.txt" ||
          std::ranges::any_of(rulerPaths, [&](auto const& ruler) { return pathCovered(path, ruler); }))
      {
        return {
          .failure = FailureReason::ScopeViolation, .detail = std::format("ruler path changed: {}", path.string())};
      }

      auto const rule = std::ranges::find_if(scope, [&](ScopeRule const& candidate) { return candidate.path == path; });

      if (rule == scope.end())
      {
        return {
          .failure = FailureReason::ScopeViolation, .detail = std::format("out-of-scope path: {}", path.string())};
      }

      if (!rule->operations.contains(operation))
      {
        return {.failure = FailureReason::ScopeViolation,
                .detail = std::format("operation '{}' not allowed for {}", toString(operation), path.string())};
      }
    }

    return {.accepted = true, .failure = FailureReason::None, .detail = "accepted"};
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
    try
    {
      auto const path = _root / relativePath;
      std::filesystem::create_directories(path.parent_path());
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output.write(content.data(), static_cast<std::streamsize>(content.size()));
      output.close();

      if (!output)
      {
        return makeError(Error::Code::IoError, std::format("cannot write {}", path.string()));
      }

      return {};
    }
    catch (std::exception const& exception)
    {
      return makeError(Error::Code::IoError, exception.what());
    }
  }

  Result<> ArtifactStore::append(std::filesystem::path const& relativePath, std::string_view document) const
  {
    auto appended = appendYamlDocument(_root / relativePath, document);

    if (!appended)
    {
      return makeError(Error::Code::IoError, appended.error().message);
    }

    return {};
  }
} // namespace ao::fleet
