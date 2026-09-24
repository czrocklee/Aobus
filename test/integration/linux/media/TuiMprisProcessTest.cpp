// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/utility/Raii.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fcntl.h>
#include <gio/gio.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers) -- POSIX child signal delivery.
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <thread>
#include <tuple>
#include <vector>

namespace ao::media::test
{
  namespace
  {
    constexpr auto kRootInterface = "org.mpris.MediaPlayer2";
    constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
    constexpr auto kBusInterface = "org.freedesktop.DBus";
    constexpr auto kNamePrefix = "org.mpris.MediaPlayer2.aobus.tui.instance";

    class [[nodiscard]] TerminalDescriptor final
    {
    public:
      explicit TerminalDescriptor(std::int32_t descriptor)
        : _descriptor{descriptor}
      {
        if (_descriptor < 0)
        {
          throw std::runtime_error{"Could not open test PTY"};
        }
      }
      ~TerminalDescriptor() { std::ignore = ::close(_descriptor); }
      TerminalDescriptor(TerminalDescriptor const&) = delete;
      TerminalDescriptor& operator=(TerminalDescriptor const&) = delete;
      TerminalDescriptor(TerminalDescriptor&&) = delete;
      TerminalDescriptor& operator=(TerminalDescriptor&&) = delete;
      std::int32_t get() const noexcept { return _descriptor; }

    private:
      std::int32_t _descriptor;
    };

    struct ChildEnvironment final
    {
      // nullopt is genuinely absent, not an empty environment variable.
      std::optional<std::string> optBusAddress;
      std::filesystem::path runtimeDirectory;
    };

    class [[nodiscard]] TuiProcess final
    {
    public:
      TuiProcess(std::filesystem::path const& root, ChildEnvironment const& environment, std::string_view mode)
        : _terminal{::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK)}
      {
        if (::grantpt(_terminal.get()) != 0 || ::unlockpt(_terminal.get()) != 0)
        {
          throw std::runtime_error{"Could not prepare test PTY"};
        }

        auto terminalName = std::array<char, 256>{};

        if (::ptsname_r(_terminal.get(), terminalName.data(), terminalName.size()) != 0)
        {
          throw std::runtime_error{"Could not resolve test PTY"};
        }

        auto slave = TerminalDescriptor{::open(terminalName.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
        auto size = ::winsize{.ws_row = 30, .ws_col = 110, .ws_xpixel = 0, .ws_ypixel = 0};

        // NOLINTNEXTLINE(misc-include-cleaner) -- <sys/ioctl.h> provides this platform ioctl macro.
        if (::ioctl(slave.get(), TIOCSWINSZ, &size) != 0)
        {
          throw std::runtime_error{"Could not size test PTY"};
        }

        auto launcherPtr =
          utility::makeUniquePtr<::g_object_unref>(::g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE));
        auto* const launcher = launcherPtr.get();
        auto const library = root / "library";
        std::filesystem::create_directories(library);
        ::g_subprocess_launcher_set_cwd(launcher, root.c_str());

        for (auto const* key : {"HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME"})
        {
          auto const directory = root / key;
          std::filesystem::create_directories(directory);
          ::g_subprocess_launcher_setenv(launcher, key, directory.c_str(), TRUE);
        }

        ::g_subprocess_launcher_setenv(launcher, "XDG_RUNTIME_DIR", environment.runtimeDirectory.c_str(), TRUE);
        ::g_subprocess_launcher_setenv(launcher, "TERM", "xterm-256color", TRUE);
        ::g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
        // A fallback regression must not discover the user's X11 session bus.
        ::g_subprocess_launcher_unsetenv(launcher, "DISPLAY");
        ::g_subprocess_launcher_unsetenv(launcher, "WAYLAND_DISPLAY");
        ::g_subprocess_launcher_unsetenv(launcher, "DBUS_STARTER_ADDRESS");
        ::g_subprocess_launcher_unsetenv(launcher, "DBUS_STARTER_BUS_TYPE");

        if (environment.optBusAddress)
        {
          ::g_subprocess_launcher_setenv(
            launcher, "DBUS_SESSION_BUS_ADDRESS", environment.optBusAddress->c_str(), TRUE);
        }
        else
        {
          ::g_subprocess_launcher_unsetenv(launcher, "DBUS_SESSION_BUS_ADDRESS");
        }

        // Launcher owns each duplicate, including on failed spawn. No fork child
        // callback runs C++ after GIO has started threads in the test process.
        for (int destination = 0; destination < 3; ++destination)
        {
          int const descriptor = ::fcntl(slave.get(), F_DUPFD_CLOEXEC, 3);

          if (descriptor < 0)
          {
            throw std::runtime_error{"Could not duplicate test PTY"};
          }

          ::g_subprocess_launcher_take_fd(launcher, descriptor, destination);
        }

        auto arguments =
          std::vector<std::string>{AOBUS_TUI_EXECUTABLE, "--library", library.string(), "--cover-art-mode", "off"};

        if (!mode.empty())
        {
          arguments.emplace_back("--system-media");
          arguments.emplace_back(mode);
        }

        auto pointers = std::vector<char const*>{};

        for (auto const& argument : arguments)
        {
          pointers.push_back(argument.c_str());
        }

        pointers.push_back(nullptr);
        ::GError* error = nullptr;
        _process = ::g_subprocess_launcher_spawnv(launcher, pointers.data(), &error);
        auto const errorPtr = detail::ErrorPtr{error};

        if (_process == nullptr)
        {
          throw std::runtime_error{error != nullptr ? error->message : "Could not spawn TUI"};
        }

        try
        {
          // GSubprocess stops exposing its identifier after exit. Retain the
          // value while available, rather than borrowing it during a bus query.
          auto const* identifier = ::g_subprocess_get_identifier(_process);
          _identifier = identifier != nullptr ? identifier : "";
          _wait = std::async(
            std::launch::async, [process = _process] { return ::g_subprocess_wait(process, nullptr, nullptr); });
        }
        catch (...)
        {
          ::g_subprocess_force_exit(_process);
          std::ignore = ::g_subprocess_wait(_process, nullptr, nullptr);
          ::g_object_unref(_process);
          throw;
        }
      }

      ~TuiProcess()
      {
        // Kill before joining the waiter on every failure path. PTY output is
        // nonblocking and read only in bounded batches, never by a joining reader.
        if (_wait.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready)
        {
          ::g_subprocess_force_exit(_process);
        }

        _wait.wait();
        ::g_object_unref(_process);
      }

      TuiProcess(TuiProcess const&) = delete;
      TuiProcess& operator=(TuiProcess const&) = delete;
      TuiProcess(TuiProcess&&) = delete;
      TuiProcess& operator=(TuiProcess&&) = delete;

      void drain()
      {
        auto buffer = std::array<char, 4096>{};

        for (std::int32_t batch = 0; batch < 16; ++batch)
        {
          auto const count = ::read(_terminal.get(), buffer.data(), buffer.size());

          if (count > 0)
          {
            _output.append(buffer.data(), static_cast<std::size_t>(count));

            if (_output.size() > 65536)
            {
              _output.erase(0, _output.size() - 65536);
            }
          }
          else if (count < 0 && errno == EINTR)
          {
            continue;
          }
          else
          {
            break;
          }
        }
      }

      template<typename Predicate>
      void await(Predicate predicate)
      {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};

        while (true)
        {
          drain();

          if (predicate())
          {
            return;
          }

          if (std::chrono::steady_clock::now() >= deadline || hasExited())
          {
            throw std::runtime_error{"TUI condition not reached; terminal tail: " + _output};
          }

          // External process readiness has no owner executor to drive. This is
          // bounded observation polling, not a delay used as a success oracle.
          std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
      }

      bool hasExited() { return _wait.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready; }
      std::string const& output() const noexcept { return _output; }
      std::string const& identifier() const noexcept { return _identifier; }
      void awaitScreen()
      {
        await(
          [this]
          {
            if (!_output.contains("\x1b[?1049h"))
            {
              return false;
            }

            auto attributes = ::termios{};

            if (::tcgetattr(_terminal.get(), &attributes) != 0)
            {
              throw std::runtime_error{"Could not inspect test PTY input mode"};
            }

            // Alternate-screen entry alone may precede raw input setup. Observe
            // the owned PTY's actual mode so Ctrl-C is delivered as an input byte,
            // not consumed by canonical input or terminal signal handling.
            return (attributes.c_lflag & (ICANON | ISIG)) == 0;
          });
      }
      void sendSignal(int number) { ::g_subprocess_send_signal(_process, number); }

      void sendKeyboardExit()
      {
        if (::write(_terminal.get(), "\x03", 1) != 1)
        {
          throw std::runtime_error{"Could not send TUI keyboard exit"};
        }
      }

      void requireExit(bool success = true)
      {
        await([this] { return hasExited(); });
        drain();
        INFO("terminal tail: " << _output);
        REQUIRE(::g_subprocess_get_if_exited(_process));

        if (success)
        {
          CHECK(::g_subprocess_get_exit_status(_process) == 0);
        }
        else
        {
          CHECK(::g_subprocess_get_exit_status(_process) != 0);
        }
      }

    private:
      TerminalDescriptor _terminal;
      ::GSubprocess* _process = nullptr;
      std::future<::gboolean> _wait;
      std::string _identifier;
      std::string _output;
    };

    CallResult busCall(BusClient& client, char const* method, ::GVariant* parameters, ::GVariantType const* replyType)
    {
      ::GError* error = nullptr;
      auto* const reply = ::g_dbus_connection_call_sync(client.nativeConnection(),
                                                        kBusInterface,
                                                        "/org/freedesktop/DBus",
                                                        kBusInterface,
                                                        method,
                                                        parameters,
                                                        replyType,
                                                        G_DBUS_CALL_FLAGS_NONE,
                                                        1000,
                                                        nullptr,
                                                        &error);
      return CallResult{reply, error};
    }

    std::vector<std::string> tuiNames(BusClient& client)
    {
      auto reply = busCall(client, "ListNames", nullptr, G_VARIANT_TYPE("(as)"));
      REQUIRE(reply);
      auto namesPtr = takeVariant(::g_variant_get_child_value(reply.value(), 0));
      auto names = std::vector<std::string>{};

      for (::gsize index = 0; index < ::g_variant_n_children(namesPtr.get()); ++index)
      {
        auto namePtr = takeVariant(::g_variant_get_child_value(namesPtr.get(), index));
        auto name = std::string{::g_variant_get_string(namePtr.get(), nullptr)};

        if (name.starts_with(kNamePrefix))
        {
          names.push_back(name);
        }
      }

      return names;
    }

    void awaitNoNames(BusClient& client)
    {
      // Process exit closes its socket, but the daemon may not have handled the
      // HUP yet. This positive absence observation must not stop at child exit.
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};

      while (!tuiNames(client).empty())
      {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
    }

    std::string awaitName(TuiProcess& process, BusClient& client)
    {
      auto names = std::vector<std::string>{};
      process.await(
        [&]
        {
          names = tuiNames(client);
          return !names.empty();
        });
      REQUIRE(names.size() == 1);
      auto owner = busCall(
        client, "GetConnectionUnixProcessID", ::g_variant_new("(s)", names.front().c_str()), G_VARIANT_TYPE("(u)"));
      REQUIRE(owner);
      ::guint32 identifier = 0;
      ::g_variant_get(owner.value(), "(u)", &identifier);
      REQUIRE_FALSE(process.identifier().empty());
      REQUIRE(std::to_string(identifier) == process.identifier());
      // A name can be acquired before FTXUI installs SignalExitWatcher. A
      // property reply must run through the real owner executor, proving the
      // event loop is ready before the signal-exit case sends SIGTERM.
      auto pending = client.callAsync(names.front(),
                                      kPropertiesInterface,
                                      "Get",
                                      ::g_variant_new("(ss)", kRootInterface, "Identity"),
                                      G_VARIANT_TYPE("(v)"));
      auto reply = awaitCallWithoutOwnerProgress(pending);
      INFO(reply.errorMessage());
      REQUIRE(reply);
      return names.front();
    }

    void requireQuit(TuiProcess& process, BusClient& client, std::string const& name)
    {
      auto pending = client.callAsync(name, kRootInterface, "Quit", nullptr, G_VARIANT_TYPE("()"));
      auto reply = awaitCallWithoutOwnerProgress(pending);
      INFO(reply.errorMessage());
      REQUIRE(reply);
      CHECK(::g_variant_is_of_type(reply.value(), G_VARIANT_TYPE("()")));
      process.requireExit();
      awaitNoNames(client);
    }

    std::string socketAddress(std::filesystem::path const& path)
    {
      auto escapedPtr = utility::makeUniquePtr<::g_free>(::g_dbus_address_escape_value(path.c_str()));
      return "unix:path=" + std::string{escapedPtr.get()};
    }
  } // namespace

  TEST_CASE("TUI system media - default and explicit auto acknowledge Quit before clean process exit",
            "[tui][integration][mpris][concurrency]")
  {
    auto const* const mode = GENERATE("", "auto");
    auto directory = ao::test::TempDir{};
    auto bus = PrivateBus{};
    auto client = BusClient{bus.address()};
    auto process = TuiProcess{directory.path(), {bus.address(), directory.path() / "runtime"}, mode};
    auto const name = awaitName(process, client);
    auto pending = client.callAsync(
      name, kPropertiesInterface, "GetAll", ::g_variant_new("(s)", kRootInterface), G_VARIANT_TYPE("(a{sv})"));
    auto reply = awaitCallWithoutOwnerProgress(pending);
    REQUIRE(reply);
    auto propertiesPtr = takeVariant(::g_variant_get_child_value(reply.value(), 0));
    char const* identity = nullptr;
    ::gboolean canQuit = FALSE;
    ::gboolean canRaise = TRUE;
    REQUIRE(::g_variant_lookup(propertiesPtr.get(), "Identity", "&s", &identity));
    CHECK(std::string_view{identity} == "Aobus TUI");
    REQUIRE(::g_variant_lookup(propertiesPtr.get(), "CanQuit", "b", &canQuit));
    REQUIRE(::g_variant_lookup(propertiesPtr.get(), "CanRaise", "b", &canRaise));
    CHECK(canQuit == TRUE);
    CHECK(canRaise == FALSE);
    auto desktopEntryPtr = takeVariant(::g_variant_lookup_value(propertiesPtr.get(), "DesktopEntry", nullptr));
    CHECK_FALSE(desktopEntryPtr);
    // Pending-write policy is covered by ExitController unit tests; an empty
    // library process cannot prove the submitted-write/remote-Quit race.
    requireQuit(process, client, name);
  }

  TEST_CASE("TUI system media - keyboard and signal exits release the real process bus name",
            "[tui][integration][mpris]")
  {
    auto const keyboard = GENERATE(true, false);
    auto directory = ao::test::TempDir{};
    auto bus = PrivateBus{};
    auto client = BusClient{bus.address()};
    auto process = TuiProcess{directory.path(), {bus.address(), directory.path() / "runtime"}, "auto"};
    std::ignore = awaitName(process, client);
    process.awaitScreen();

    if (keyboard)
    {
      process.sendKeyboardExit();
    }
    else
    {
      process.sendSignal(SIGTERM);
    }

    process.requireExit();
    awaitNoNames(client);
  }

  TEST_CASE("TUI system media - explicit off runs normally without owning a media name", "[tui][integration][mpris]")
  {
    auto directory = ao::test::TempDir{};
    auto bus = PrivateBus{};
    auto client = BusClient{bus.address()};
    auto process = TuiProcess{directory.path(), {bus.address(), directory.path() / "runtime"}, "off"};
    process.awaitScreen();
    // Readiness/end snapshots are bounded black-box absence evidence, not proof
    // against arbitrary delayed publication. App's off branch owns the stronger
    // no-adapter invariant; do not replace it with a timing-based test oracle.
    CHECK(tuiNames(client).empty());
    process.sendKeyboardExit();
    process.requireExit();
    CHECK(tuiNames(client).empty());
  }

  TEST_CASE("TUI system media - invalid option is rejected by the product parser before screen startup",
            "[tui][integration][mpris]")
  {
    auto directory = ao::test::TempDir{};
    auto bus = PrivateBus{};
    auto client = BusClient{bus.address()};
    auto process = TuiProcess{directory.path(), {bus.address(), directory.path() / "runtime"}, "invalid"};
    process.requireExit(false);
    CHECK(process.output().contains("--system-media"));
    CHECK(process.output().contains("invalid"));
    CHECK_FALSE(process.output().contains("\x1b[?1049h"));
    CHECK(tuiNames(client).empty());
  }

  TEST_CASE("TUI system media - only absent bus environment falls back to the escaped runtime socket",
            "[tui][integration][mpris]")
  {
    auto const* const environmentMode = GENERATE("unset", "empty", "invalid", "explicit", "relative");
    CAPTURE(environmentMode);
    auto directory = ao::test::TempDir{};
    auto const runtime = directory.path() / "runtime space,%=socket";
    std::filesystem::create_directories(runtime);
    auto fallbackBus = PrivateBus{socketAddress(runtime / "bus")};
    auto fallbackClient = BusClient{fallbackBus.address()};
    auto explicitBus = PrivateBus{};
    auto explicitClient = BusClient{explicitBus.address()};
    auto environment = ChildEnvironment{.optBusAddress = std::nullopt, .runtimeDirectory = runtime};

    if (std::string_view{environmentMode} == "empty")
    {
      environment.optBusAddress = "";
    }
    else if (std::string_view{environmentMode} == "invalid")
    {
      environment.optBusAddress = "not-a-dbus-address";
    }
    else if (std::string_view{environmentMode} == "explicit")
    {
      environment.optBusAddress = explicitBus.address();
    }
    else if (std::string_view{environmentMode} == "relative")
    {
      // The socket exists relative to the child's cwd, but only an absolute
      // XDG_RUNTIME_DIR is eligible for automatic bus discovery.
      environment.runtimeDirectory = runtime.filename();
    }

    auto process = TuiProcess{directory.path(), environment, "auto"};

    if (std::string_view{environmentMode} == "empty" || std::string_view{environmentMode} == "invalid" ||
        std::string_view{environmentMode} == "relative")
    {
      process.awaitScreen();
      // As with off mode, these are readiness/end absence snapshots. They do
      // not assert that arbitrary delayed work could never publish a name.
      CHECK(tuiNames(fallbackClient).empty());
      CHECK(tuiNames(explicitClient).empty());
      process.sendKeyboardExit();
      process.requireExit();
    }
    else
    {
      auto& selectedClient = std::string_view{environmentMode} == "explicit" ? explicitClient : fallbackClient;
      auto& otherClient = std::string_view{environmentMode} == "explicit" ? fallbackClient : explicitClient;
      auto const name = awaitName(process, selectedClient);
      CHECK(tuiNames(otherClient).empty());
      requireQuit(process, selectedClient, name);
    }

    CHECK(tuiNames(fallbackClient).empty());
    CHECK(tuiNames(explicitClient).empty());
  }

  TEST_CASE("TUI system media - absent or nonsocket runtime bus leaves the process usable", "[tui][integration][mpris]")
  {
    auto const regularFile = GENERATE(false, true);
    auto directory = ao::test::TempDir{};
    auto const runtime = directory.path() / "runtime";
    std::filesystem::create_directories(runtime);

    if (regularFile)
    {
      auto file = std::ofstream{runtime / "bus"};
      file << "not a socket";
    }

    auto process = TuiProcess{directory.path(), {std::nullopt, runtime}, "auto"};
    process.awaitScreen();
    process.sendKeyboardExit();
    process.requireExit();
  }
} // namespace ao::media::test
