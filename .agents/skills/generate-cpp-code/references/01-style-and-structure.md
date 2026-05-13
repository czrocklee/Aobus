# Style and Structure Snippets

Covers `CONTRIBUTING.md` rules 1.1 and 2.1-2.6.

## C++ target (1.1)

- Write C++23 code without modules.
- Prefer standard-library C++23 facilities already used by the project over custom replacements.

## Formatting and control-block spacing (2.1)

```cpp
void refresh(Device const& device)
{
  auto const route = buildRoute(device);

  if (!route)
  {
    report(route.error());
    return;
  }

  apply(*route);
}
```

```cpp
TEST_CASE("Format negotiation keeps passthrough", "[audio]")
{
  auto format = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16};

  SECTION("matching capabilities")
  {
    REQUIRE(canPassthrough(format));
  }
}

TEST_CASE("Format negotiation rejects unsupported rates", "[audio]")
{
  REQUIRE_FALSE(canUseRate(12345));
}
```

Keep a comment written specifically for an `if` directly above the `if`:

```cpp
// Keep the stale callback from mutating the current playback session.
if (generation != _playbackGeneration)
{
  return;
}
```

## Naming conventions (2.2)

```cpp
namespace ao::library
{
  constexpr std::size_t kMaxInlineTags = 8;

  enum class LoadState
  {
    Idle,
    Loading,
    Failed
  };

  struct TrackInfo final
  {
    std::uint64_t trackId = 0;
    std::string displayTitle{};
  };

  class TrackStore final
  {
  public:
    void addTrack(TrackInfo const& track);
    std::size_t trackCount() const noexcept { return _tracks.size(); }

  private:
    std::vector<TrackInfo> _tracks;
  };
} // namespace ao::library
```

## Headers and includes (2.3, 2.4)

Headers use `#pragma once`:

```cpp
#pragma once

#include <ao/audio/Format.h>

#include <span>
#include <string_view>
```

Implementation include order is paired header/project headers, third-party headers, then standard library:

```cpp
#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/audio/backend/detail/PipeWireShared.h>
#include <ao/utility/Log.h>

extern "C"
{
#include <pipewire/pipewire.h>
}

#include <array>
#include <format>
#include <mutex>
```

## Member order (2.5)

Header access sections are `public` -> `protected` -> `private`; members inside a section are nested types/aliases, member functions, static functions, data members, static data members, friends:

```cpp
class PlayerController final
{
public:
  using Callback = std::function<void(Status const&)>;

  PlayerController(Player& player, Callback callback);
  void start();
  Status status() const;

  static std::string_view name() noexcept;

private:
  struct PendingCommand final;

  void publish(Status const& status);

  Player& _player;
  Callback _callback;
  std::optional<PendingCommand> _optPendingCommand;

  static constexpr std::size_t kMaxPendingCommands = 4;
};
```

Keep `.cpp` definitions in the same order as the header declarations.

## Namespaces and linkage (2.6)

```cpp
namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    std::uint64_t bytesPerSecond(Format const& format) noexcept
    {
      return static_cast<std::uint64_t>(format.sampleRate) * format.channels * kBytesPer24BitSample;
    }
  } // namespace
} // namespace ao::audio
```

Prefix external C APIs with `::`, but use `std::` for C functions/types that the C++ standard library provides:

```cpp
auto* stream = ::pw_stream_new(core.get(), "Aobus Playback", props.release());
auto const size = std::strlen(name);
std::memcpy(destination.data(), source.data(), destination.size());
```
