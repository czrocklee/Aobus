// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/utility/AtomicFileTransaction.h"
#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/Error.h>
#include <ao/utility/AtomicFile.h>

#ifdef _WIN32
#include <catch2/catch_message.hpp>
#endif
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <ios>
#endif
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#ifdef _WIN32
#include <thread>
#include <vector>
#endif

#ifdef _WIN32
namespace
{
  std::filesystem::path extendedWindowsPath(std::filesystem::path const& path)
  {
    return std::filesystem::path{L"\\\\?\\" + std::filesystem::absolute(path).wstring()};
  }
} // namespace
#endif

namespace ao::utility::test
{
  namespace
  {
    enum class FailureStage : std::uint8_t
    {
      None,
      Normalize,
      Parent,
      Create,
      Write,
      Synchronize,
      Close,
      Replace,
    };

    struct ScriptedAtomicFileState final
    {
      FailureStage failureStage = FailureStage::None;
      std::string targetContents = "old";
      std::string temporaryContents{};
      bool temporaryExists = false;
      bool cleanupFails = false;
      std::size_t cleanupAttempts = 0;
      std::size_t parentSyncAttempts = 0;
    };

    std::string_view failureStageName(FailureStage const stage)
    {
      switch (stage)
      {
        case FailureStage::None: return "None";
        case FailureStage::Normalize: return "Normalize";
        case FailureStage::Parent: return "Parent";
        case FailureStage::Create: return "Create";
        case FailureStage::Write: return "Write";
        case FailureStage::Synchronize: return "Synchronize";
        case FailureStage::Close: return "Close";
        case FailureStage::Replace: return "Replace";
      }

      std::unreachable();
    }

    Result<> scriptedResult(ScriptedAtomicFileState const& state, FailureStage const stage)
    {
      if (state.failureStage == stage)
      {
        return makeError(Error::Code::IoError, std::string{failureStageName(stage)} + " failure");
      }

      return {};
    }

    class ScriptedTemporaryFile final
    {
    public:
      explicit ScriptedTemporaryFile(ScriptedAtomicFileState& state)
        : _state{&state}
      {
      }

      ~ScriptedTemporaryFile() noexcept
      {
        if (_active && _state->temporaryExists)
        {
          ++_state->cleanupAttempts;

          if (!_state->cleanupFails)
          {
            _state->temporaryExists = false;
          }
        }
      }

      ScriptedTemporaryFile(ScriptedTemporaryFile const&) = delete;
      ScriptedTemporaryFile& operator=(ScriptedTemporaryFile const&) = delete;

      ScriptedTemporaryFile(ScriptedTemporaryFile&& other) noexcept
        : _state{other._state}, _active{std::exchange(other._active, false)}
      {
      }

      ScriptedTemporaryFile& operator=(ScriptedTemporaryFile&&) = delete;

      Result<> writeAll(std::string_view data)
      {
        if (_state->failureStage == FailureStage::Write)
        {
          _state->temporaryContents = data.substr(0, data.size() / 2);
        }
        else
        {
          _state->temporaryContents = data;
        }

        return scriptedResult(*_state, FailureStage::Write);
      }

      Result<> synchronizeData() const { return scriptedResult(*_state, FailureStage::Synchronize); }
      Result<> closeForReplacement() const { return scriptedResult(*_state, FailureStage::Close); }

      Result<> replaceTarget(std::filesystem::path const& /*targetPath*/)
      {
        if (auto const result = scriptedResult(*_state, FailureStage::Replace); !result)
        {
          return result;
        }

        _state->targetContents = _state->temporaryContents;
        _state->temporaryExists = false;
        _active = false;
        return {};
      }

    private:
      ScriptedAtomicFileState* _state;
      bool _active = true;
    };

    class ScriptedAtomicFileOperations final
    {
    public:
      explicit ScriptedAtomicFileOperations(ScriptedAtomicFileState& state)
        : _state{state}
      {
      }

      Result<std::filesystem::path> normalizeTargetPath(std::filesystem::path const& targetPath) const
      {
        if (auto const result = scriptedResult(_state, FailureStage::Normalize); !result)
        {
          return std::unexpected{result.error()};
        }

        return targetPath;
      }

      Result<> createParentDirectories(std::filesystem::path const& /*parentPath*/) const
      {
        return scriptedResult(_state, FailureStage::Parent);
      }

      Result<ScriptedTemporaryFile> createPrivateTemporaryFile(std::filesystem::path const& /*parentPath*/) const
      {
        if (auto const result = scriptedResult(_state, FailureStage::Create); !result)
        {
          return std::unexpected{result.error()};
        }

        _state.temporaryExists = true;
        return ScriptedTemporaryFile{_state};
      }

      void synchronizeParentDirectoryBestEffort(std::filesystem::path const& /*parentPath*/) noexcept
      {
        ++_state.parentSyncAttempts;
      }

    private:
      ScriptedAtomicFileState& _state;
    };

    bool isTemporaryArtifact(std::filesystem::path const& path)
    {
      auto const name = path.filename().string();
      return name.starts_with(".temp.") || name.starts_with(".ao.tmp.");
    }
  } // namespace

  TEST_CASE("AtomicFile transaction - preserves the target on every pre-replacement failure",
            "[utility][unit][atomicfile]")
  {
    constexpr auto kFailureStages = std::array{
      FailureStage::Normalize,
      FailureStage::Parent,
      FailureStage::Create,
      FailureStage::Write,
      FailureStage::Synchronize,
      FailureStage::Close,
      FailureStage::Replace,
    };

    for (auto const stage : kFailureStages)
    {
      CAPTURE(static_cast<std::uint8_t>(stage));
      auto state = ScriptedAtomicFileState{.failureStage = stage};
      auto operations = ScriptedAtomicFileOperations{state};

      auto const result = detail::runAtomicReplacement(operations, "/state/config.yaml", "new contents");

      CHECK_FALSE(result);
      CHECK(state.targetContents == "old");
      CHECK(state.parentSyncAttempts == 0);

      auto const temporaryWasCreated = stage >= FailureStage::Write;
      auto const expectedCleanupAttempts = temporaryWasCreated ? std::size_t{1} : std::size_t{0};
      CHECK(state.cleanupAttempts == expectedCleanupAttempts);
      CHECK_FALSE(state.temporaryExists);
    }
  }

  TEST_CASE("AtomicFile transaction - retains the primary error when temporary cleanup fails",
            "[utility][unit][atomicfile]")
  {
    auto state = ScriptedAtomicFileState{.failureStage = FailureStage::Write, .cleanupFails = true};
    auto operations = ScriptedAtomicFileOperations{state};

    auto const result = detail::runAtomicReplacement(operations, "/state/config.yaml", "new contents");

    REQUIRE_FALSE(result);
    CHECK(result.error().message == "Write failure");
    CHECK(state.targetContents == "old");
    CHECK(state.cleanupAttempts == 1);
    CHECK(state.temporaryExists);
  }

  TEST_CASE("AtomicFile transaction - publishes complete contents before best-effort parent synchronization",
            "[utility][unit][atomicfile]")
  {
    auto state = ScriptedAtomicFileState{};
    auto operations = ScriptedAtomicFileOperations{state};

    auto const result = detail::runAtomicReplacement(operations, "/state/config.yaml", "new contents");

    REQUIRE(result);
    CHECK(state.targetContents == "new contents");
    CHECK_FALSE(state.temporaryExists);
    CHECK(state.cleanupAttempts == 0);
    CHECK(state.parentSyncAttempts == 1);
  }

  TEST_CASE("AtomicFile - writes data atomically with owner-only permissions", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "config.yaml";

    auto const result = writeAtomically(targetPath, "version: 1\n");
    CHECK(result.has_value());

    REQUIRE(std::filesystem::exists(targetPath));

    auto in = std::ifstream{targetPath};
    auto const content = std::string{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    CHECK(content == "version: 1\n");

    CHECK(ao::test::hasPrivateManagedFileAccess(targetPath));
  }

  TEST_CASE("AtomicFile - preserves empty and embedded-null payloads", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = tempDir.path() / "opaque.bin";

    REQUIRE(writeAtomically(targetPath, ""));
    CHECK(ao::test::readFile(targetPath).empty());

    auto const payload = std::string{"left\0right", 10};
    REQUIRE(writeAtomically(targetPath, payload));
    CHECK(ao::test::readFile(targetPath) == payload);
  }

  TEST_CASE("AtomicFile - overwrites existing file", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "state.yaml";

    CHECK(writeAtomically(targetPath, "old").has_value());
    CHECK(writeAtomically(targetPath, "new").has_value());

    auto in = std::ifstream{targetPath};
    auto const content = std::string{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    CHECK(content == "new");
  }

  TEST_CASE("AtomicFile - fails when parent directory is not writable", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const readonlyDir = std::filesystem::path{tempDir.path()} / "readonly";
    std::filesystem::create_directories(readonlyDir);
    auto const denied = ao::test::ScopedDirectoryAccessGuard{readonlyDir, ao::test::DeniedDirectoryAccess::Write};

    if (!denied.effective())
    {
      SKIP("the current process bypasses directory write restrictions");
    }

    auto const targetPath = readonlyDir / "config.yaml";
    auto const result = writeAtomically(targetPath, "version: 1\n");
    CHECK_FALSE(result.has_value());
  }

  TEST_CASE("AtomicFile - fails to overwrite a directory", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "dir";
    std::filesystem::create_directories(targetPath);

    auto const result = writeAtomically(targetPath, "content");
    CHECK_FALSE(result.has_value());

    for (auto const& entry : std::filesystem::directory_iterator{tempDir.path()})
    {
      CHECK_FALSE(isTemporaryArtifact(entry.path()));
    }
  }

#ifdef _WIN32

  TEST_CASE("AtomicFile - supports Windows paths beyond MAX_PATH", "[utility][unit][atomicfile][windows]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto targetPath = tempDir.path();

    for (std::int32_t index = 0; index < 6; ++index)
    {
      targetPath /= std::string(48, static_cast<char>('a' + index));
    }

    targetPath /= "state.yaml";
    REQUIRE(targetPath.wstring().size() > 260);

    auto const result = writeAtomically(targetPath, "long path content");
    auto const errorMessage = result ? std::string{} : result.error().message;
    INFO(errorMessage);
    REQUIRE(result);

    auto input = std::ifstream{extendedWindowsPath(targetPath), std::ios::binary};
    auto const content = std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
    CHECK(content == "long path content");
    input.close();

    auto ec = std::error_code{};
    std::filesystem::remove_all(extendedWindowsPath(tempDir.path()), ec);
    CHECK_FALSE(ec);
  }

  TEST_CASE("AtomicFile - concurrent Windows writers use distinct temp files", "[utility][unit][atomicfile][windows]")
  {
    constexpr std::size_t kWriterCount = 8;
    auto const tempDir = ao::test::TempDir{};
    auto targets = std::array<std::filesystem::path, kWriterCount>{};
    auto contents = std::array<std::string, kWriterCount>{};
    auto succeeded = std::array<bool, kWriterCount>{};
    auto writers = std::vector<std::jthread>{};
    writers.reserve(kWriterCount);

    for (std::size_t index = 0; index < kWriterCount; ++index)
    {
      contents[index] = "writer-" + std::to_string(index);
      targets[index] = tempDir.path() / ("state-" + std::to_string(index) + ".yaml");
      writers.emplace_back([&, index]
                           { succeeded[index] = writeAtomically(targets[index], contents[index]).has_value(); });
    }

    writers.clear();

    for (auto const result : succeeded)
    {
      CHECK(result);
    }

    for (std::size_t index = 0; index < kWriterCount; ++index)
    {
      auto input = std::ifstream{targets[index], std::ios::binary};
      auto const content = std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
      CHECK(content == contents[index]);
    }

    for (auto const& entry : std::filesystem::directory_iterator{tempDir.path()})
    {
      CHECK_FALSE(entry.path().filename().string().starts_with(".ao.tmp."));
    }
  }
#endif
} // namespace ao::utility::test
