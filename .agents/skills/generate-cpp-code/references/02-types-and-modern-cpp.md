# Types and Modern C++ Snippets

Covers `CONTRIBUTING.md` rules 2.7-2.9, 3.1, 3.2, and most of 3.4.

## Types, aliases, strings, and buffers (2.7)

```cpp
using TrackId = std::uint64_t;

struct Packet final
{
  std::array<std::byte, 4096> bytes{};
  std::uint32_t frameCount = 0;
  std::string displayName{};
};

void writePacket(std::span<std::byte const> bytes);
```

- Use `std::int32_t`, `std::uint64_t`, etc.; use plain `int`/`unsigned` only when required by an external API.
- Prefer `std::string` over owning `char*`.
- Prefer `std::array`/`std::to_array` over raw C arrays for fixed buffers.
- Prefer `using` over `typedef`.

## Casts (2.8)

```cpp
auto const byteCount = static_cast<std::uint32_t>(writtenFrames * frameSize);
auto* data = static_cast<std::byte*>(buffer->buffer->datas[0].data);
auto const* header = reinterpret_cast<TrackHeader const*>(bytes.data());
```

Never use casts to suppress unused warnings. Use the explicit unused-value patterns shown in rule 3.2.8 instead.

## Output and logging (2.9)

```cpp
out << "indexed " << trackCount << " tracks" << '\n';
AUDIO_LOG_INFO("Negotiated format: {}Hz, {} channels", format.sampleRate, format.channels);
```

Use `std::endl` only when a forced flush is intentional. Use project logging macros for runtime diagnostics instead of `std::cout`/`std::cerr`.

## C++20 features (3.1)

Concepts and `std::format`:

```cpp
template<typename T>
  requires std::integral<T>
std::string formatCount(T value)
{
  return std::format("{} items", value);
}
```

`std::span`, ranges algorithms, projections, and views:

```cpp
bool supportsRate(DeviceCapabilities const& caps, std::uint32_t sampleRate)
{
  return std::ranges::contains(caps.sampleRates, sampleRate);
}

std::optional<Device> findDevice(std::span<Device const> devices, DeviceId const& id)
{
  auto const iter = std::ranges::find(devices, id, &Device::id);

  if (iter == devices.end())
  {
    return std::nullopt;
  }

  return *iter;
}

auto visibleNames(std::span<Device const> devices)
{
  return devices | std::views::filter(&Device::isVisible) | std::views::transform(&Device::displayName);
}
```

`[[no_unique_address]]`, prefix/suffix helpers, designated initializers, and `std::jthread`:

```cpp
struct CallbackHolder final
{
  [[no_unique_address]] EmptyCallback callback;
  DeviceId id{};
};

if (path.string().ends_with(".flac"))
{
  return Format::Flac;
}

auto format = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false};

auto worker = std::jthread{
  [](std::stop_token const& token)
  {
    while (!token.stop_requested())
    {
      decodeNextChunk();
    }
  }};
```

## C++17 features and attributes (3.2)

Use `std::optional` for absence, not failure. Every optional variable, member, and parameter uses the `opt` prefix; checks use `if (optValue)` / `if (!optValue)`:

```cpp
std::optional<RouteAnchor> optRouteAnchor(Device const& device);

if (auto optAnchor = optRouteAnchor(device); optAnchor)
{
  useAnchor(*optAnchor);
}
```

Use `std::variant`, `std::string_view`, `if constexpr`, structured bindings, and init-statements when clearer:

```cpp
using PropertyValue = std::variant<bool, float, std::string>;

void setName(std::string_view name);

template<typename T>
void writeValue(T const& value)
{
  if constexpr (std::is_integral_v<T>)
  {
    writeInteger(value);
  }
  else
  {
    writeObject(value);
  }
}

for (auto const& [id, device] : devicesById)
{
  registerDevice(id, device);
}

if (auto const result = openDevice(id); !result)
{
  return std::unexpected(result.error());
}
```

Do not add `[[nodiscard]]`. For unused values, use anonymous parameter comments when a parameter is never used, and `[[maybe_unused]]` when a value is conditionally used:

```cpp
void onDeviceRemoved(DeviceId const& /*id*/)
{
  refreshDevices();
}

void onStateChanged(::pw_stream_state /*oldState*/, ::pw_stream_state newState)
{
  [[maybe_unused]] auto const previousState = newState;
  updateState(newState);
}
```

## General language practices (3.4)

RAII and resource ownership:

```cpp
auto rawProps = ::pw_properties_new(PW_KEY_APP_NAME, "Aobus", nullptr);
auto props = ao::utility::makeUniquePtr<::pw_properties_free>(rawProps);

struct PwLoopDeleter final
{
  void operator()(::pw_thread_loop* loop) const noexcept
  {
    ::pw_thread_loop_destroy(loop);
  }
};

using PwLoopPtr = std::unique_ptr<::pw_thread_loop, PwLoopDeleter>;
```

Override/noexcept and member initialization:

```cpp
class Backend final : public IBackend
{
public:
  explicit Backend(DeviceId id)
    : _id{std::move(id)}
  {
  }

  BackendId backendId() const noexcept override { return kBackendPipeWire; }

private:
  DeviceId _id;
};
```

Initialization style:

```cpp
auto device = Device{.id = DeviceId{"default"}, .displayName = "Default"};
auto bytes = std::vector<std::byte>{};
std::size_t offset = 0;
float volume = 1.0F;
::spa_pod* pod = nullptr;
```

Use traditional return syntax for non-lambda functions and omit empty lambda parameter lists:

```cpp
std::uint32_t frameCount(Format const& format)
{
  auto compute = [&]
  { return format.sampleRate / 100U; };

  return compute();
}
```
