// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/ConfigStore.h>
#include <ao/yaml/Serialization.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    struct State final
    {
      std::int32_t count = 0;
      std::string name{};
      bool enabled = true;
    };

    struct StateYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, State const& state) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("count", state.count).scalar("name", state.name).scalar("enabled", state.enabled);
        return std::move(writer).finish();
      }

      Result<State> deserialize(ryml::ConstNodeRef node, State const& /*seed*/) const
      {
        constexpr auto kContext = std::string_view{"test state"};
        constexpr auto kKeys = std::to_array<std::string_view>({"count", "name", "enabled"});

        auto state = State{};
        auto reader = yaml::MapReader{node, kKeys, kContext};
        reader.requiredScalar("count", state.count)
          .requiredScalar("name", state.name)
          .requiredScalar("enabled", state.enabled);
        return std::move(reader).finish(std::move(state));
      }
    };

    struct SeededStateYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, State const& state) const
      {
        return StateYamlSchema{}.serialize(node, state);
      }

      Result<State> deserialize(ryml::ConstNodeRef node, State const& seed) const
      {
        constexpr auto kContext = std::string_view{"seeded test state"};
        constexpr auto kKeys = std::to_array<std::string_view>({"count", "name", "enabled"});

        auto state = seed;
        auto reader = yaml::MapReader{node, kKeys, kContext, yaml::UnknownKeyPolicy::Allow};
        reader.optionalScalar("count", state.count)
          .optionalScalar("name", state.name)
          .optionalScalar("enabled", state.enabled);
        return std::move(reader).finish(std::move(state));
      }
    };

    struct RejectingYamlSchema final
    {
      Result<> serialize(ryml::NodeRef /*node*/, State const& /*state*/) const
      {
        return makeError(Error::Code::InvalidState, "intentional schema rejection");
      }

      Result<State> deserialize(ryml::ConstNodeRef /*node*/, State const& /*seed*/) const
      {
        return makeError(Error::Code::FormatRejected, "intentional schema rejection");
      }
    };

    struct ThrowingYamlSchema final
    {
      Result<> serialize(ryml::NodeRef /*node*/, State const& /*state*/) const
      {
        throwException<Exception>("intentional serializer exception");
      }

      Result<State> deserialize(ryml::ConstNodeRef /*node*/, State const& /*seed*/) const
      {
        throwException<Exception>("intentional deserializer exception");
      }
    };

    void writeFile(std::filesystem::path const& path, std::string_view contents)
    {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      REQUIRE(output.is_open());
      output << contents;
    }
  } // namespace

  TEST_CASE("ConfigStore - delegates payload ownership to an explicit schema", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = std::filesystem::path{tempDir.path()} / "config.yaml";
    auto store = ConfigStore{configPath};
    auto const original = State{.count = 42, .name = "line1\nline2 with quotes \"ok\"", .enabled = false};

    REQUIRE(store.save("owned", original, StateYamlSchema{}));
    CHECK(ao::test::readFile(configPath).contains("enabled: false"));

    auto reloaded = ConfigStore{configPath};
    auto restored = State{};
    auto const loaded = reloaded.load("owned", restored, StateYamlSchema{});

    REQUIRE(loaded);
    REQUIRE(*loaded);
    CHECK(restored.count == original.count);
    CHECK(restored.name == original.name);
    CHECK(restored.enabled == original.enabled);
  }

  TEST_CASE("ConfigStore - reports absence without changing the target", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};

    SECTION("ReadWrite mode treats a missing file and group as absence")
    {
      auto store = ConfigStore{tempDir.path() / "missing.yaml"};
      auto target = State{.count = 99, .name = "unchanged", .enabled = false};
      auto const loaded = store.load("absent", target, StateYamlSchema{});

      REQUIRE(loaded);
      CHECK_FALSE(*loaded);
      CHECK(target.count == 99);
      CHECK(target.name == "unchanged");
      CHECK_FALSE(target.enabled);
    }

    SECTION("ReadOnly mode reports a missing backing file")
    {
      auto store = ConfigStore{tempDir.path() / "missing.yaml", ConfigStore::OpenMode::ReadOnly};
      auto target = State{.count = 99};
      auto const loaded = store.load("absent", target, StateYamlSchema{});

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().code == Error::Code::NotFound);
      CHECK(target.count == 99);
    }
  }

  TEST_CASE("ConfigStore - passes the caller seed only to the selected schema", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    writeFile(configPath,
              "owned:\n"
              "  count: 7\n"
              "  futureField: retained-by-newer-writer\n");

    auto store = ConfigStore{configPath};
    auto target = State{.count = 99, .name = "seed name", .enabled = false};
    auto const loaded = store.load("owned", target, SeededStateYamlSchema{});

    REQUIRE(loaded);
    REQUIRE(*loaded);
    CHECK(target.count == 7);
    CHECK(target.name == "seed name");
    CHECK_FALSE(target.enabled);
  }

  TEST_CASE("ConfigStore - failed deserialization leaves the target unchanged", "[runtime][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    SECTION("A schema rejection is contextualized")
    {
      writeFile(configPath,
                "owned:\n"
                "  count: 7\n"
                "  name: changed\n"
                "  enabled: not-a-bool\n");
      auto store = ConfigStore{configPath};
      auto target = State{.count = 99, .name = "unchanged", .enabled = true};
      auto const loaded = store.load("owned", target, StateYamlSchema{});

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().code == Error::Code::FormatRejected);
      CHECK(loaded.error().message.contains("owned"));
      CHECK(target.count == 99);
      CHECK(target.name == "unchanged");
      CHECK(target.enabled);
    }

    SECTION("A schema exception is contained")
    {
      writeFile(configPath, "owned: {}\n");
      auto store = ConfigStore{configPath};
      auto target = State{.count = 99};
      auto const loaded = store.load("owned", target, ThrowingYamlSchema{});

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().code == Error::Code::FormatRejected);
      CHECK(loaded.error().message.contains("intentional deserializer exception"));
      CHECK(target.count == 99);
    }

    SECTION("Malformed YAML is rejected before schema selection")
    {
      constexpr auto kOriginal = std::string_view{"owned: [unterminated"};
      writeFile(configPath, kOriginal);
      auto store = ConfigStore{configPath};
      auto target = State{.count = 99};
      auto const loaded = store.load("owned", target, StateYamlSchema{});

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().code == Error::Code::FormatRejected);
      CHECK(target.count == 99);
      CHECK(ao::test::readFile(configPath) == kOriginal);
    }
  }

  TEST_CASE("ConfigStore - batch saves are atomic across schemas", "[runtime][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto store = ConfigStore{configPath};
    REQUIRE(store.save("retained", State{.count = 11, .name = "before"}, StateYamlSchema{}));
    auto const originalContents = ao::test::readFile(configPath);

    SECTION("A returned serialization error commits no groups")
    {
      auto const result = store.saveTogether(configWrite("staged", State{.count = 22}, StateYamlSchema{}),
                                             configWrite("broken", State{}, RejectingYamlSchema{}));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidState);
      CHECK(result.error().message.contains("broken"));
      CHECK(ao::test::readFile(configPath) == originalContents);
    }

    SECTION("A thrown serialization error commits no groups")
    {
      auto const result = store.saveTogether(configWrite("staged", State{.count = 22}, StateYamlSchema{}),
                                             configWrite("broken", State{}, ThrowingYamlSchema{}));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidState);
      CHECK(result.error().message.contains("intentional serializer exception"));
      CHECK(ao::test::readFile(configPath) == originalContents);
    }

    auto reloaded = ConfigStore{configPath};
    auto const staged = reloaded.contains("staged");
    auto const broken = reloaded.contains("broken");
    REQUIRE(staged);
    REQUIRE(broken);
    CHECK_FALSE(*staged);
    CHECK_FALSE(*broken);
  }

  TEST_CASE("ConfigStore - successful batch saves replace independent groups", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto store = ConfigStore{configPath};

    REQUIRE(
      store.saveTogether(configWrite("first", State{.count = 1, .name = "one"}, StateYamlSchema{}),
                         configWrite("second", State{.count = 2, .name = "two", .enabled = false}, StateYamlSchema{})));
    REQUIRE(store.save("first", State{.count = 3, .name = "replacement"}, StateYamlSchema{}));

    auto reloaded = ConfigStore{configPath};
    auto first = State{};
    auto second = State{};
    auto firstLoaded = reloaded.load("first", first, StateYamlSchema{});
    auto secondLoaded = reloaded.load("second", second, StateYamlSchema{});

    REQUIRE(firstLoaded);
    REQUIRE(*firstLoaded);
    REQUIRE(secondLoaded);
    REQUIRE(*secondLoaded);
    CHECK(first.count == 3);
    CHECK(first.name == "replacement");
    CHECK(second.count == 2);
    CHECK(second.name == "two");
    CHECK_FALSE(second.enabled);
  }

  TEST_CASE("ConfigStore - failed replacement preserves the live document", "[runtime][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto const backupPath = tempDir.path() / "config.backup.yaml";
    auto store = ConfigStore{configPath};
    REQUIRE(store.save("retained", State{.count = 11, .name = "before"}, StateYamlSchema{}));
    auto const originalContents = ao::test::readFile(configPath);

    std::filesystem::rename(configPath, backupPath);
    std::filesystem::create_directory(configPath);
    auto const failed = store.save("staged", State{.count = 22}, StateYamlSchema{});

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == Error::Code::IoError);
    CHECK(ao::test::readFile(backupPath) == originalContents);

    std::filesystem::remove(configPath);
    std::filesystem::rename(backupPath, configPath);
    REQUIRE(store.save("later", State{.count = 33}, StateYamlSchema{}));

    auto reloaded = ConfigStore{configPath};
    auto retained = State{};
    auto later = State{};
    auto retainedLoaded = reloaded.load("retained", retained, StateYamlSchema{});
    auto laterLoaded = reloaded.load("later", later, StateYamlSchema{});
    REQUIRE(retainedLoaded);
    REQUIRE(*retainedLoaded);
    REQUIRE(laterLoaded);
    REQUIRE(*laterLoaded);
    CHECK(retained.count == 11);
    CHECK(later.count == 33);
    CHECK_FALSE(*reloaded.contains("staged"));
  }

  TEST_CASE("ConfigStore - saves preserve rejected backing documents", "[runtime][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    for (auto const original : {std::string_view{},
                                std::string_view{"owned: [unterminated"},
                                std::string_view{"- first\n- second\n"},
                                std::string_view{"owned: {}\nowned: {}\n"}})
    {
      CAPTURE(original);
      writeFile(configPath, original);
      auto store = ConfigStore{configPath};
      auto const result = store.save("replacement", State{.count = 7}, StateYamlSchema{});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(ao::test::readFile(configPath) == original);
    }
  }

  TEST_CASE("ConfigStore - later saves preserve owned serialized strings", "[runtime][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    {
      auto store = ConfigStore{configPath};
      REQUIRE(store.save("first", State{.name = "temporary value"}, StateYamlSchema{}));
      REQUIRE(store.save("second", State{.count = 7}, StateYamlSchema{}));

      auto liveFirst = State{};
      auto const liveLoaded = store.load("first", liveFirst, StateYamlSchema{});
      REQUIRE(liveLoaded);
      REQUIRE(*liveLoaded);
      CHECK(liveFirst.name == "temporary value");
    }

    auto reloaded = ConfigStore{configPath};
    auto first = State{};
    auto const loaded = reloaded.load("first", first, StateYamlSchema{});
    REQUIRE(loaded);
    REQUIRE(*loaded);
    CHECK(first.name == "temporary value");
  }

  TEST_CASE("ConfigStore - removes groups and enforces read-only mode", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    SECTION("Removal is exact, durable, and idempotent")
    {
      auto store = ConfigStore{configPath};
      REQUIRE(store.saveTogether(configWrite("removed", State{.count = 1}, StateYamlSchema{}),
                                 configWrite("retained", State{.count = 2}, StateYamlSchema{})));
      REQUIRE(store.removeGroup("removed"));
      REQUIRE(store.removeGroup("removed"));

      auto reloaded = ConfigStore{configPath};
      auto removed = reloaded.contains("removed");
      auto retained = reloaded.contains("retained");
      REQUIRE(removed);
      REQUIRE(retained);
      CHECK_FALSE(*removed);
      CHECK(*retained);
    }

    SECTION("Removing an absent group does not create a file")
    {
      auto store = ConfigStore{configPath};
      REQUIRE(store.removeGroup("missing"));
      CHECK_FALSE(std::filesystem::exists(configPath));
    }

    SECTION("Read-only stores reject writes")
    {
      auto store = ConfigStore{configPath, ConfigStore::OpenMode::ReadOnly};
      CHECK_THROWS_AS(store.save("group", State{}, StateYamlSchema{}), Exception);
      CHECK_THROWS_AS(store.removeGroup("group"), Exception);
    }
  }
} // namespace ao::rt::test
