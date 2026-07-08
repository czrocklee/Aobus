// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/rt/ConfigStore.h>
#include <ao/utility/StrongType.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    enum class Color : std::uint8_t
    {
      Red,
      Green,
      Blue,
    };

    enum class Flags : std::uint8_t
    {
      None = 0,
      Read = 1,
      Write = 2,
      Execute = 4,
    };

    constexpr Flags operator|(Flags a, Flags b)
    {
      return static_cast<Flags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }

    struct Inner final
    {
      std::int32_t value = 0;
      std::string label{};
    };

    struct ComplexAggregate final
    {
      std::int32_t count = 0;
      double rate = 0.0;
      bool enabled = true;
      std::string name{};
      Color color = Color::Red;
      std::optional<int> optScore{};
      std::optional<std::string> optNote{};
      std::vector<Inner> items{};
      std::map<std::string, int, std::less<>> scores{};
      Inner nested{};
    };

    struct AllOptional final
    {
      std::optional<int> optInt{};
      std::optional<double> optDouble{};
      std::optional<std::string> optString{};
      std::optional<Color> optColor{};
    };

    struct WithEnums final
    {
      Color simple = Color::Red;
      Flags bitmask = Flags::Read;
    };

    struct TestIdTag final
    {};
    using TestId = utility::StrongType<std::uint32_t, TestIdTag>;

    struct WithTaggedId final
    {
      TestId id{};
      std::string label{};
    };

    struct TestStringTag final
    {};
    using TestStringId = utility::StrongType<std::string, TestStringTag>;

    struct WithStrongId final
    {
      TestStringId id{};
      std::int32_t count = 0;
    };

    struct WithEmptyContainers final
    {
      std::vector<int> numbers{};
      std::map<std::string, int, std::less<>> dictionary{};
    };

    struct DeepNested final
    {
      ComplexAggregate level1{};
      std::vector<ComplexAggregate> more{};
    };
  } // namespace

  TEST_CASE("ConfigStore - persists aggregate values and defaults", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = std::filesystem::path{tempDir.path()} / "config.yaml";
    auto configStore = ConfigStore{configPath};

    SECTION("Round-trip preserves all fields")
    {
      auto const original =
        ComplexAggregate{.count = 42,
                         .rate = 3.14,
                         .enabled = false,
                         .name = "test-config",
                         .color = Color::Blue,
                         .optScore = 95,
                         .optNote = "excellent",
                         .items = {Inner{.value = 1, .label = "first"}, Inner{.value = 2, .label = "second"}},
                         .scores = {{"alpha", 10}, {"beta", 20}},
                         .nested = Inner{.value = 99, .label = "nested-inner"}};

      configStore.save("complex", original);
      REQUIRE(configStore.flush());
      CHECK(ao::test::readFile(configPath).find("enabled: false") != std::string::npos);

      auto reloaded = ConfigStore{configPath};
      auto loaded = ComplexAggregate{};
      REQUIRE(reloaded.load("complex", loaded));

      CHECK(loaded.count == 42);
      CHECK(loaded.rate == 3.14);
      CHECK(loaded.enabled == false);
      CHECK(loaded.name == "test-config");
      CHECK(loaded.color == Color::Blue);
      REQUIRE(loaded.optScore);
      CHECK(*loaded.optScore == 95);
      REQUIRE(loaded.optNote);
      CHECK(*loaded.optNote == "excellent");
      REQUIRE(loaded.items.size() == 2);
      CHECK(loaded.items[0].value == 1);
      CHECK(loaded.items[0].label == "first");
      CHECK(loaded.items[1].value == 2);
      CHECK(loaded.items[1].label == "second");
      CHECK(loaded.scores.size() == 2);
      CHECK(loaded.scores["alpha"] == 10);
      CHECK(loaded.scores["beta"] == 20);
      CHECK(loaded.nested.value == 99);
      CHECK(loaded.nested.label == "nested-inner");
    }

    SECTION("Default-constructed aggregate round-trips preserving defaults")
    {
      auto const original = ComplexAggregate{};
      configStore.save("complex", original);
      REQUIRE(configStore.flush());
      CHECK(ao::test::readFile(configPath).find("enabled: true") != std::string::npos);

      auto reloaded = ConfigStore{configPath};
      auto loaded = ComplexAggregate{.count = -1, .name = "garbage"};
      REQUIRE(reloaded.load("complex", loaded));

      CHECK(loaded.count == 0);
      CHECK(loaded.rate == 0.0);
      CHECK(loaded.enabled == true);
      CHECK(loaded.name.empty());
      CHECK(loaded.color == Color::Red);
      CHECK_FALSE(loaded.optScore);
      CHECK_FALSE(loaded.optNote);
      CHECK(loaded.items.empty());
      CHECK(loaded.scores.empty());
      CHECK(loaded.nested.value == 0);
      CHECK(loaded.nested.label.empty());
    }
  }

  TEST_CASE("ConfigStore - round-trips enum values", "[runtime][unit][config]")
  {
    auto tempDir = ao::test::TempDir{};
    auto configStore = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};

    SECTION("All enum values round-trip")
    {
      auto const original = WithEnums{.simple = Color::Green, .bitmask = Flags::Read | Flags::Write};

      configStore.save("enums", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = WithEnums{};
      REQUIRE(reloaded.load("enums", loaded));

      CHECK(loaded.simple == Color::Green);
      CHECK(loaded.bitmask == (Flags::Read | Flags::Write));
    }

    SECTION("Enum default value (zero) round-trips")
    {
      auto const original = WithEnums{.simple = Color::Red, .bitmask = Flags::None};

      configStore.save("enums", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = WithEnums{.simple = Color::Blue, .bitmask = Flags::Execute};
      REQUIRE(reloaded.load("enums", loaded));

      CHECK(loaded.simple == Color::Red);
      CHECK(loaded.bitmask == Flags::None);
    }
  }

  TEST_CASE("ConfigStore - round-trips optional values and empty states", "[runtime][unit][config]")
  {
    auto tempDir = ao::test::TempDir{};
    auto configStore = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};

    SECTION("All optionals with values")
    {
      auto const original =
        AllOptional{.optInt = 42, .optDouble = std::numbers::e, .optString = "hello", .optColor = Color::Green};

      configStore.save("opt", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = AllOptional{};
      REQUIRE(reloaded.load("opt", loaded));

      REQUIRE(loaded.optInt);
      CHECK(*loaded.optInt == 42);
      REQUIRE(loaded.optDouble);
      CHECK(*loaded.optDouble == std::numbers::e);
      REQUIRE(loaded.optString);
      CHECK(*loaded.optString == "hello");
      REQUIRE(loaded.optColor);
      CHECK(*loaded.optColor == Color::Green);
    }

    SECTION("All optionals empty (nullopt)")
    {
      auto const original = AllOptional{};

      configStore.save("opt", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = AllOptional{.optInt = 100, .optString = "dirty"};
      REQUIRE(reloaded.load("opt", loaded));

      CHECK_FALSE(loaded.optInt);
      CHECK_FALSE(loaded.optDouble);
      CHECK_FALSE(loaded.optString);
      CHECK_FALSE(loaded.optColor);
    }

    SECTION("Mixed optional states")
    {
      auto const original = AllOptional{.optInt = 7, .optColor = Color::Blue};

      configStore.save("opt", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = AllOptional{};
      REQUIRE(reloaded.load("opt", loaded));

      REQUIRE(loaded.optInt);
      CHECK(*loaded.optInt == 7);
      CHECK_FALSE(loaded.optDouble);
      CHECK_FALSE(loaded.optString);
      REQUIRE(loaded.optColor);
      CHECK(*loaded.optColor == Color::Blue);
    }
  }

  TEST_CASE("ConfigStore - round-trips strong value types", "[runtime][unit][config]")
  {
    auto tempDir = ao::test::TempDir{};
    auto configStore = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};

    SECTION("TaggedInteger round-trip via aggregate")
    {
      auto const original = WithTaggedId{.id = TestId{12345}, .label = "tagged-test"};

      configStore.save("tagged", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = WithTaggedId{};
      REQUIRE(reloaded.load("tagged", loaded));

      CHECK(loaded.id.raw() == 12345);
      CHECK(loaded.label == "tagged-test");
    }

    SECTION("TaggedInteger default value (0)")
    {
      auto const original = WithTaggedId{};

      configStore.save("tagged", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = WithTaggedId{.id = TestId{999}};
      REQUIRE(reloaded.load("tagged", loaded));

      CHECK(loaded.id.raw() == 0);
    }

    SECTION("StrongId round-trip via aggregate")
    {
      auto const original = WithStrongId{.id = TestStringId{"my-unique-id"}, .count = 5};

      configStore.save("strong", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = WithStrongId{};
      REQUIRE(reloaded.load("strong", loaded));

      CHECK(loaded.id.raw() == "my-unique-id");
      CHECK(loaded.count == 5);
    }

    SECTION("StrongId empty string")
    {
      auto const original = WithStrongId{};
      // default-constructed StrongId has empty string
      configStore.save("strong", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = WithStrongId{.id = TestStringId{"not-empty"}};
      REQUIRE(reloaded.load("strong", loaded));

      CHECK(loaded.id.raw().empty());
    }
  }

  TEST_CASE("ConfigStore - round-trips containers and nested aggregates", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto configStore = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};

    SECTION("Empty containers round-trip")
    {
      auto const original = WithEmptyContainers{};

      configStore.save("empty", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = WithEmptyContainers{.numbers = {42}, .dictionary = {{"z", 99}}};
      REQUIRE(reloaded.load("empty", loaded));

      CHECK(loaded.numbers.empty());
      CHECK(loaded.dictionary.empty());
    }

    SECTION("Deeply nested aggregates")
    {
      auto const original =
        DeepNested{.level1 = {.count = 10, .optScore = 50, .items = {Inner{.value = 1, .label = "a"}}},
                   .more = {ComplexAggregate{.count = 20, .optScore = 60},
                            ComplexAggregate{.count = 30, .name = "second", .color = Color::Green}}};

      configStore.save("deep", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = DeepNested{};
      REQUIRE(reloaded.load("deep", loaded));

      CHECK(loaded.level1.count == 10);
      REQUIRE(loaded.level1.optScore);
      CHECK(*loaded.level1.optScore == 50);
      REQUIRE(loaded.level1.items.size() == 1);
      CHECK(loaded.level1.items[0].value == 1);
      CHECK(loaded.level1.items[0].label == "a");

      REQUIRE(loaded.more.size() == 2);
      CHECK(loaded.more[0].count == 20);
      REQUIRE(loaded.more[0].optScore);
      CHECK(*loaded.more[0].optScore == 60);
      CHECK(loaded.more[1].count == 30);
      CHECK(loaded.more[1].name == "second");
      CHECK(loaded.more[1].color == Color::Green);
    }
  }

  TEST_CASE("ConfigStore - handles missing keys, overwrites, and scalar edge cases", "[runtime][unit][config]")
  {
    auto tempDir = ao::test::TempDir{};
    auto configStore = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};

    SECTION("Loading non-existent key does not modify object")
    {
      auto obj = ComplexAggregate{.count = 42, .name = "unchanged"};

      CHECK(configStore.load("nonexistent", obj));

      CHECK(obj.count == 42);
      CHECK(obj.name == "unchanged");
    }

    SECTION("Loading from non-existent file returns success in ReadWrite mode")
    {
      auto missingStore = ConfigStore{std::filesystem::path{tempDir.path()} / "does_not_exist.yaml"};
      auto obj = ComplexAggregate{.count = 99};

      CHECK(missingStore.load("anything", obj));

      CHECK(obj.count == 99);
    }

    SECTION("Loading from non-existent file returns NotFound in ReadOnly mode")
    {
      auto missingStore =
        ConfigStore{std::filesystem::path{tempDir.path()} / "does_not_exist.yaml", ConfigStore::OpenMode::ReadOnly};
      auto obj = ComplexAggregate{.count = 99};

      auto result = missingStore.load("anything", obj);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::NotFound);

      CHECK(obj.count == 99);
    }

    SECTION("Malformed YAML is rejected as a format error")
    {
      auto const configPath = std::filesystem::path{tempDir.path()} / "bad.yaml";
      {
        auto out = std::ofstream{configPath};
        out << "complex: [unterminated";
      }

      auto malformedStore = ConfigStore{configPath};
      auto obj = ComplexAggregate{.count = 99};

      auto result = malformedStore.load("complex", obj);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(obj.count == 99);
    }

    SECTION("Failed file inspection is not cached as loaded")
    {
      auto failingStore = ConfigStore{std::filesystem::path{tempDir.path()} / std::string(300, 'x')};
      auto obj = ComplexAggregate{.count = 99};

      auto firstResult = failingStore.load("anything", obj);
      REQUIRE(!firstResult);
      CHECK(firstResult.error().code == Error::Code::IoError);

      auto secondResult = failingStore.load("anything", obj);
      REQUIRE(!secondResult);
      CHECK(secondResult.error().code == Error::Code::IoError);
      CHECK(obj.count == 99);
    }

    SECTION("Overwriting an existing key replaces data")
    {
      auto const first = ComplexAggregate{.count = 1, .name = "first"};
      configStore.save("overwrite", first);

      auto const second = ComplexAggregate{.count = 2, .name = "second", .optScore = 88};
      configStore.save("overwrite", second);

      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = ComplexAggregate{};
      REQUIRE(reloaded.load("overwrite", loaded));

      CHECK(loaded.count == 2);
      CHECK(loaded.name == "second");
      REQUIRE(loaded.optScore);
      CHECK(*loaded.optScore == 88);
    }

    SECTION("Multiple independent keys in same file")
    {
      auto const agg = ComplexAggregate{.count = 100, .name = "agg-data"};
      configStore.save("complex", agg);

      auto const enums = WithEnums{.simple = Color::Blue, .bitmask = Flags::Read | Flags::Execute};
      configStore.save("enums", enums);

      auto const opt = AllOptional{.optInt = -5, .optString = "partial"};
      configStore.save("opt", opt);

      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};

      auto loadedAgg = ComplexAggregate{};
      REQUIRE(reloaded.load("complex", loadedAgg));
      CHECK(loadedAgg.count == 100);
      CHECK(loadedAgg.name == "agg-data");

      auto loadedEnums = WithEnums{};
      REQUIRE(reloaded.load("enums", loadedEnums));
      CHECK(loadedEnums.simple == Color::Blue);
      int const expectedFlags = static_cast<std::int32_t>(Flags::Read) | static_cast<std::int32_t>(Flags::Execute);
      CHECK(static_cast<int>(loadedEnums.bitmask) == expectedFlags);

      auto optLoaded = AllOptional{};
      REQUIRE(reloaded.load("opt", optLoaded));
      REQUIRE(optLoaded.optInt);
      CHECK(*optLoaded.optInt == -5);
      REQUIRE(optLoaded.optString);
      CHECK(*optLoaded.optString == "partial");
      CHECK_FALSE(optLoaded.optDouble);
    }

    SECTION("String with special characters")
    {
      auto const original = ComplexAggregate{.name = "line1\nline2\twith\"quotes\"and/slashes"};

      configStore.save("complex", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = ComplexAggregate{};
      REQUIRE(reloaded.load("complex", loaded));

      CHECK(loaded.name == original.name);
    }

    SECTION("Numeric boundary values")
    {
      auto const original = ComplexAggregate{.count = std::numeric_limits<int>::min(),
                                             .rate = std::numeric_limits<double>::max(),
                                             .optScore = std::numeric_limits<int>::max()};

      configStore.save("complex", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = ComplexAggregate{};
      REQUIRE(reloaded.load("complex", loaded));

      CHECK(loaded.count == std::numeric_limits<int>::min());
      CHECK(loaded.rate == std::numeric_limits<double>::max());
      REQUIRE(loaded.optScore);
      CHECK(*loaded.optScore == std::numeric_limits<int>::max());
    }

    SECTION("Boolean values")
    {
      auto const original = ComplexAggregate{.enabled = false};

      configStore.save("complex", original);
      REQUIRE(configStore.flush());

      auto reloaded = ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml"};
      auto loaded = ComplexAggregate{.enabled = true};
      REQUIRE(reloaded.load("complex", loaded));

      CHECK(loaded.enabled == false);
    }
  }

  TEST_CASE("ConfigStore - flush uses saved string copies", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = std::filesystem::path{tempDir.path()} / "config.yaml";

    {
      auto configStore = ConfigStore{configPath};
      {
        auto const original = ComplexAggregate{.name = "temporary string that will go out of scope"};
        configStore.save("test", original);
      } // original is destroyed here

      // Flush should still work and use the copied string in the arena
      REQUIRE(configStore.flush());
    }

    auto reloaded = ConfigStore{configPath};
    auto loaded = ComplexAggregate{};
    REQUIRE(reloaded.load("test", loaded));
    CHECK(loaded.name == "temporary string that will go out of scope");
  }

  TEST_CASE("ConfigStore - read-only mode rejects writes and reports missing files", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto configStore =
      ConfigStore{std::filesystem::path{tempDir.path()} / "config.yaml", ConfigStore::OpenMode::ReadOnly};

    SECTION("save() on ReadOnly throws")
    {
      auto obj = ComplexAggregate{};

      CHECK_THROWS_AS(configStore.save("key", obj), ao::Exception);
    }

    SECTION("flush() on ReadOnly throws")
    {
      CHECK_THROWS_AS(configStore.flush(), ao::Exception);
    }

    SECTION("load() on ReadOnly with missing file returns NotFound")
    {
      auto obj = ComplexAggregate{.count = 99};

      auto result = configStore.load("anything", obj);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(obj.count == 99);
    }
  }
} // namespace ao::rt::test
