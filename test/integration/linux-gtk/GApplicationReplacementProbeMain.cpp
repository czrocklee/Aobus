// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <gio/gio.h>
#include <glib-unix.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
  using namespace std::string_view_literals;

  constexpr auto kApplicationId = "org.aobus.test.GApplicationReplacement";
  constexpr auto kInstanceOption = "--aobus-gapplication-instance"sv;
  constexpr auto kScenarioOption = "--aobus-probe-child"sv;

  struct GObjectDeleter final
  {
    template<typename T>
    void operator()(T* const object) const noexcept
    {
      ::g_object_unref(object);
    }
  };

  struct GErrorDeleter final
  {
    void operator()(::GError* const error) const noexcept { ::g_error_free(error); }
  };

  struct GVariantDeleter final
  {
    void operator()(::GVariant* const variant) const noexcept { ::g_variant_unref(variant); }
  };

  struct GMainLoopDeleter final
  {
    void operator()(::GMainLoop* const loop) const noexcept { ::g_main_loop_unref(loop); }
  };

  struct GFreeDeleter final
  {
    void operator()(char* const data) const noexcept { ::g_free(data); }
  };

  template<typename T>
  using GObjectPtr = std::unique_ptr<T, GObjectDeleter>;

  using GErrorPtr = std::unique_ptr<::GError, GErrorDeleter>;
  using GVariantPtr = std::unique_ptr<::GVariant, GVariantDeleter>;
  using GMainLoopPtr = std::unique_ptr<::GMainLoop, GMainLoopDeleter>;
  using GCharPtr = std::unique_ptr<char, GFreeDeleter>;

  [[noreturn]] void throwGlibError(std::string_view const context, ::GError* const rawError)
  {
    auto errorPtr = GErrorPtr{rawError};
    auto const* const message = errorPtr ? errorPtr->message : "unknown GLib error";
    throw std::runtime_error{std::format("{}: {}", context, message)};
  }

  void requireProbe(bool const condition, std::string_view const message)
  {
    if (!condition)
    {
      throw std::runtime_error{std::string{message}};
    }
  }

  std::string queryNameOwner(::GDBusConnection* const connection)
  {
    ::GError* rawError = nullptr;
    auto replyPtr = GVariantPtr{::g_dbus_connection_call_sync(connection,
                                                              "org.freedesktop.DBus",
                                                              "/org/freedesktop/DBus",
                                                              "org.freedesktop.DBus",
                                                              "GetNameOwner",
                                                              ::g_variant_new("(s)", kApplicationId),
                                                              G_VARIANT_TYPE("(s)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              -1,
                                                              nullptr,
                                                              &rawError)};

    if (!replyPtr)
    {
      throwGlibError("failed to query GApplication name owner", rawError);
    }

    char const* owner = nullptr;
    ::g_variant_get(replyPtr.get(), "(&s)", &owner);
    return owner;
  }

  struct RegistrationObservation final
  {
    std::string state;
    std::string ownerBefore;
    std::string ownerAfter;
    std::string connectionName;
  };

  struct RegisteredApplication final
  {
    GObjectPtr<::GApplication> appPtr;
    RegistrationObservation observation;
  };

  RegisteredApplication registerApplication(::GApplicationFlags const flags, std::string_view const expectedOwner)
  {
    auto ownerBefore = std::string{"-"};
    auto sessionConnectionPtr = GObjectPtr<::GDBusConnection>{};

    if (!expectedOwner.empty())
    {
      ::GError* rawError = nullptr;
      sessionConnectionPtr.reset(::g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &rawError));

      if (!sessionConnectionPtr)
      {
        throwGlibError("failed to connect to the private session bus", rawError);
      }

      ownerBefore = queryNameOwner(sessionConnectionPtr.get());
      requireProbe(ownerBefore == expectedOwner, "the original GApplication owner changed before registration");
    }

    auto appPtr = GObjectPtr<::GApplication>{::g_application_new(kApplicationId, flags)};

    if (::GError* rawError = nullptr; ::g_application_register(appPtr.get(), nullptr, &rawError) == FALSE)
    {
      throwGlibError("failed to register GApplication probe instance", rawError);
    }

    auto* const connection = ::g_application_get_dbus_connection(appPtr.get());
    requireProbe(connection != nullptr, "registered GApplication has no D-Bus connection");

    auto const* const connectionName = ::g_dbus_connection_get_unique_name(connection);
    requireProbe(connectionName != nullptr, "registered GApplication connection has no unique name");

    auto const isRemote = ::g_application_get_is_remote(appPtr.get()) != FALSE;
    auto observation = RegistrationObservation{.state = isRemote ? "remote" : "primary",
                                               .ownerBefore = std::move(ownerBefore),
                                               .ownerAfter = queryNameOwner(connection),
                                               .connectionName = connectionName};
    return {.appPtr = std::move(appPtr), .observation = std::move(observation)};
  }

  void printObservation(RegistrationObservation const& observation)
  {
    std::println(
      "{}\t{}\t{}\t{}", observation.state, observation.ownerBefore, observation.ownerAfter, observation.connectionName);
    std::ignore = std::fflush(stdout);
  }

  ::gboolean stopOwnerLoop(::gint /*descriptor*/, ::GIOCondition /*condition*/, ::gpointer data)
  {
    auto* const loop = static_cast<::GMainLoop*>(data);
    auto byte = std::array<char, 1>{};

    for (;;)
    {
      auto const readSize = ::read(STDIN_FILENO, byte.data(), byte.size());

      if (readSize >= 0 || errno != EINTR)
      {
        break;
      }
    }

    ::g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
  }

  std::uint32_t applicationFlags(bool const replace)
  {
    std::uint32_t flags = G_APPLICATION_ALLOW_REPLACEMENT;

    if (replace)
    {
      flags |= G_APPLICATION_REPLACE;
    }

    return flags;
  }

  std::int32_t runInstance(std::string_view const role, std::string_view const expectedOwner)
  {
    auto const replace = role == "replace";
    auto registered = registerApplication(static_cast<::GApplicationFlags>(applicationFlags(replace)), expectedOwner);
    printObservation(registered.observation);

    if (role != "owner")
    {
      return 0;
    }

    auto loopPtr = GMainLoopPtr{::g_main_loop_new(nullptr, FALSE)};
    auto const sourceId = ::g_unix_fd_add(
      STDIN_FILENO, static_cast<::GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), &stopOwnerLoop, loopPtr.get());
    requireProbe(sourceId != 0, "failed to install owner control-pipe source");
    ::g_main_loop_run(loopPtr.get());

    if (auto* const source = ::g_main_context_find_source_by_id(nullptr, sourceId); source != nullptr)
    {
      ::g_source_destroy(source);
    }

    return 0;
  }

  class [[nodiscard]] ProbeInstance final
  {
  public:
    ProbeInstance(std::filesystem::path const& executablePath,
                  std::string_view const role,
                  std::string_view const expectedOwner)
    {
      auto const flags = static_cast<::GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
      auto launcherPtr = GObjectPtr<::GSubprocessLauncher>{::g_subprocess_launcher_new(flags)};
      auto arguments = std::vector{
        executablePath.string(), std::string{kInstanceOption}, std::string{role}, std::string{expectedOwner}};
      auto argumentPointers = std::vector<char const*>{};
      argumentPointers.reserve(arguments.size() + 1);

      for (auto const& argument : arguments)
      {
        argumentPointers.push_back(argument.c_str());
      }

      argumentPointers.push_back(nullptr);
      ::GError* rawError = nullptr;
      _processPtr.reset(::g_subprocess_launcher_spawnv(launcherPtr.get(), argumentPointers.data(), &rawError));

      if (!_processPtr)
      {
        throwGlibError("failed to spawn GApplication probe instance", rawError);
      }

      _outputPtr.reset(::g_data_input_stream_new(::g_subprocess_get_stdout_pipe(_processPtr.get())));
    }

    ~ProbeInstance()
    {
      if (_processPtr && !_waited)
      {
        ::g_subprocess_force_exit(_processPtr.get());
        ::GError* rawError = nullptr;
        std::ignore = ::g_subprocess_wait(_processPtr.get(), nullptr, &rawError);

        if (rawError != nullptr)
        {
          ::g_error_free(rawError);
        }
      }
    }

    ProbeInstance(ProbeInstance const&) = delete;
    ProbeInstance& operator=(ProbeInstance const&) = delete;
    ProbeInstance(ProbeInstance&&) = delete;
    ProbeInstance& operator=(ProbeInstance&&) = delete;

    std::string readObservationLine()
    {
      ::GError* rawError = nullptr;
      ::gsize lineSize = 0;
      auto linePtr = GCharPtr{::g_data_input_stream_read_line(_outputPtr.get(), &lineSize, nullptr, &rawError)};

      if (!linePtr)
      {
        if (rawError != nullptr)
        {
          throwGlibError("failed to read GApplication probe observation", rawError);
        }

        throw std::runtime_error{"GApplication probe instance exited without an observation"};
      }

      return {linePtr.get(), lineSize};
    }

    void stop()
    {
      auto* const input = ::g_subprocess_get_stdin_pipe(_processPtr.get());
      constexpr auto kStop = "stop\n"sv;
      ::gsize writtenSize = 0;
      ::GError* rawError = nullptr;

      if (::g_output_stream_write_all(input, kStop.data(), kStop.size(), &writtenSize, nullptr, &rawError) == FALSE)
      {
        throwGlibError("failed to signal GApplication owner probe", rawError);
      }

      requireProbe(writtenSize == kStop.size(), "GApplication owner control pipe accepted a partial command");
      rawError = nullptr;

      if (::g_output_stream_close(input, nullptr, &rawError) == FALSE)
      {
        throwGlibError("failed to close GApplication owner control pipe", rawError);
      }
    }

    void waitForSuccess()
    {
      ::GError* rawError = nullptr;
      auto const succeeded = ::g_subprocess_wait_check(_processPtr.get(), nullptr, &rawError) != FALSE;
      _waited = true;

      if (!succeeded)
      {
        auto errorPtr = GErrorPtr{rawError};
        auto const* const message = errorPtr ? errorPtr->message : "unknown subprocess failure";
        throw std::runtime_error{
          std::format("GApplication probe instance failed: {}; stderr: {}", message, readStandardError())};
      }
    }

  private:
    std::string readStandardError()
    {
      auto* const errorStream = ::g_subprocess_get_stderr_pipe(_processPtr.get());
      auto output = std::string{};
      auto buffer = std::array<char, 1024>{};

      for (;;)
      {
        ::GError* rawError = nullptr;
        auto const readSize = ::g_input_stream_read(errorStream, buffer.data(), buffer.size(), nullptr, &rawError);

        if (readSize < 0)
        {
          throwGlibError("failed to read GApplication probe standard error", rawError);
        }

        if (readSize == 0)
        {
          return output;
        }

        output.append(buffer.data(), static_cast<std::size_t>(readSize));
      }
    }

    GObjectPtr<::GSubprocess> _processPtr;
    GObjectPtr<::GDataInputStream> _outputPtr;
    bool _waited = false;
  };

  RegistrationObservation parseObservation(std::string const& line)
  {
    auto fields = std::array<std::string, 4>{};
    std::size_t start = 0;

    for (std::size_t index = 0; index < fields.size(); ++index)
    {
      auto const separator = line.find('\t', start);

      if (index + 1 == fields.size())
      {
        requireProbe(separator == std::string::npos, "GApplication probe observation has extra fields");
        fields[index] = line.substr(start);
        break;
      }

      requireProbe(separator != std::string::npos, "GApplication probe observation is missing fields");
      fields[index] = line.substr(start, separator - start);
      start = separator + 1;
    }

    return {.state = std::move(fields[0]),
            .ownerBefore = std::move(fields[1]),
            .ownerAfter = std::move(fields[2]),
            .connectionName = std::move(fields[3])};
  }

  std::filesystem::path currentExecutablePath()
  {
    auto error = std::error_code{};
    auto const executablePath = std::filesystem::read_symlink("/proc/self/exe", error);

    if (error)
    {
      throw std::runtime_error{std::format("failed to resolve GApplication probe executable: {}", error.message())};
    }

    return executablePath;
  }

  void verifyOriginalOwner(RegistrationObservation const& observation)
  {
    requireProbe(observation.state == "primary", "original GApplication instance is not primary");
    requireProbe(observation.ownerBefore == "-", "original GApplication instance reported an unexpected prior owner");
    requireProbe(observation.ownerAfter == observation.connectionName,
                 "original GApplication instance does not own the application ID");
  }

  std::int32_t runOrdinaryRemoteScenario()
  {
    auto owner = ProbeInstance{currentExecutablePath(), "owner", ""};
    auto const ownerObservation = parseObservation(owner.readObservationLine());
    verifyOriginalOwner(ownerObservation);

    auto ordinary = ProbeInstance{currentExecutablePath(), "ordinary", ownerObservation.connectionName};
    auto const ordinaryObservation = parseObservation(ordinary.readObservationLine());
    ordinary.waitForSuccess();

    requireProbe(ordinaryObservation.state == "remote", "ordinary second GApplication instance is not remote");
    requireProbe(ordinaryObservation.ownerBefore == ownerObservation.connectionName,
                 "ordinary second instance did not observe the original owner before registration");
    requireProbe(ordinaryObservation.ownerAfter == ownerObservation.connectionName,
                 "ordinary second instance changed the application ID owner");
    requireProbe(ordinaryObservation.connectionName != ownerObservation.connectionName,
                 "ordinary second instance reused the original D-Bus connection");

    owner.stop();
    owner.waitForSuccess();
    std::println("ordinary-remote: remote=yes owner-unchanged=yes");
    return 0;
  }

  std::int32_t runReplacementScenario()
  {
    auto owner = ProbeInstance{currentExecutablePath(), "owner", ""};
    auto const ownerObservation = parseObservation(owner.readObservationLine());
    verifyOriginalOwner(ownerObservation);

    auto replacement = ProbeInstance{currentExecutablePath(), "replace", ownerObservation.connectionName};
    auto const replacementObservation = parseObservation(replacement.readObservationLine());
    replacement.waitForSuccess();

    requireProbe(replacementObservation.state == "primary", "replacement GApplication instance is not primary");
    requireProbe(replacementObservation.ownerBefore == ownerObservation.connectionName,
                 "replacement did not observe the original live owner before registration");
    requireProbe(replacementObservation.ownerAfter == replacementObservation.connectionName,
                 "replacement GApplication instance does not own the application ID");
    requireProbe(replacementObservation.connectionName != ownerObservation.connectionName,
                 "replacement reused the original D-Bus connection");

    owner.stop();
    owner.waitForSuccess();
    std::println("replacement: primary=yes owner-changed=yes");
    return 0;
  }

  std::int32_t runScenario(std::string_view const scenario)
  {
    // The outer probe installs a private session bus before creating any
    // GApplication; every ProbeInstance inherits this isolated bus address.
    auto testBusPtr = GObjectPtr<::GTestDBus>{::g_test_dbus_new(G_TEST_DBUS_NONE)};
    ::g_test_dbus_up(testBusPtr.get());

    try
    {
      std::int32_t exitCode = 2;

      if (scenario == "ordinary-remote")
      {
        exitCode = runOrdinaryRemoteScenario();
      }
      else if (scenario == "replacement")
      {
        exitCode = runReplacementScenario();
      }

      ::g_test_dbus_down(testBusPtr.get());
      return exitCode;
    }
    catch (...)
    {
      ::g_test_dbus_down(testBusPtr.get());
      throw;
    }
  }
} // namespace

int main(int argc, char* argv[])
{
  try
  {
    if (argc == 4 && std::string_view{argv[1]} == kInstanceOption)
    {
      return runInstance(argv[2], argv[3]);
    }

    if (argc == 3 && std::string_view{argv[1]} == kScenarioOption)
    {
      return runScenario(argv[2]);
    }

    return 2;
  }
  catch (std::exception const& error)
  {
    std::println(std::cerr, "GApplication replacement probe failed: {}", error.what());
    return 1;
  }
}
