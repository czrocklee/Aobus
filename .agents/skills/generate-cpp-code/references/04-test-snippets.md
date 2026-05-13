# Test Snippets

Use these focused examples when adding or editing tests. They complement `CONTRIBUTING.md` formatting and naming rules.

## Catch2 sections and shared setup

```cpp
#include <ao/audio/FormatNegotiator.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

using namespace ao::audio;

TEST_CASE("FormatNegotiator - Build Plan", "[playback][format_negotiator]")
{
  auto sourceFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false};
  auto caps = DeviceCapabilities{};
  caps.sampleRates = {44100, 48000, 96000};

  SECTION("Direct passthrough when formats match")
  {
    auto const plan = FormatNegotiator::buildPlan(sourceFormat, caps);

    REQUIRE(plan.requiresResample == false);
    REQUIRE(plan.deviceFormat.sampleRate == sourceFormat.sampleRate);
  }

  SECTION("Reason contains conversion detail")
  {
    caps.sampleRates = {48000};
    auto const plan = FormatNegotiator::buildPlan(sourceFormat, caps);

    CHECK_THAT(plan.reason, Catch::Matchers::ContainsSubstring("resampling required"));
  }
}
```

## Parameterized tests with `GENERATE()`

```cpp
#include <catch2/generators/catch_generators_all.hpp>

TEST_CASE("Tag reading - basic metadata", "[tag][integration]")
{
  auto const* const format = GENERATE("flac", "m4a", "mp3");
  auto const path = kTestDataDir / ("basic_metadata." + std::string{format});

  auto const file = ao::tag::File::open(path);
  REQUIRE(file != nullptr);

  auto builder = file->loadTrack();
  auto& meta = builder.metadata();

  CHECK(meta.title() == "Test Title");
  CHECK(meta.artist() == "Test Artist");
}
```

## Approximate floating-point checks

```cpp
#include <catch2/catch_approx.hpp>

TEST_CASE("Player volume is reflected in status", "[playback][player]")
{
  auto player = Player{};

  player.setVolume(0.6F);

  REQUIRE(player.status().volume == Catch::Approx(0.6F));
}
```

## FakeIt mocking pattern

```cpp
#include "TestUtility.h"

#include <fakeit.hpp>

using namespace fakeit;

TEST_CASE("Player subscribes to backend devices", "[playback][player]")
{
  auto mockProvider = Mock<IBackendProvider>{};
  auto onDevicesChanged = IBackendProvider::OnDevicesChangedCallback{};

  When(Method(mockProvider, subscribeDevices))
    .AlwaysDo(
      [&](IBackendProvider::OnDevicesChangedCallback const& callback)
      {
        onDevicesChanged = callback;

        return Subscription{};
      });

  auto player = Player{};
  player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

  REQUIRE(onDevicesChanged);
}
```

## Integration-test filesystem setup

```cpp
namespace fs = std::filesystem;

namespace
{
  fs::path const kTestDataDir = fs::path{TAG_TEST_DATA_DIR};
}

TEST_CASE("Cover art extraction", "[tag][integration]")
{
  auto const tempDir = fs::temp_directory_path() / "rs_tag_test_XXXXXX";
  fs::create_directories(tempDir);

  auto env = ao::lmdb::Environment{tempDir, {.flags = MDB_CREATE, .maxDatabases = 20}};

  fs::remove_all(tempDir);
}
```

## Test formatting reminders

- Separate top-level `TEST_CASE` blocks and nested `SECTION` blocks with blank lines.
- Use `REQUIRE` for preconditions that make the rest of the test invalid, and `CHECK` for independent assertions.
- Keep reusable setup small and local unless multiple test files need it.
