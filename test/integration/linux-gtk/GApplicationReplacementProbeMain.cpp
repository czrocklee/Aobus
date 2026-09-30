// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/unit/linux-gtk/GtkSessionBusTestSupport.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
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

  constexpr auto kApplicationIdPrefix = "org.aobus.test.GApplicationReplacement";
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

  void requireOwnedProbeSessionBus()
  {
    requireProbe(
      ao::gtk::test::isOwnedGtkSessionBus(::g_getenv("DBUS_SESSION_BUS_ADDRESS"), ::g_getenv("AOBUS_OWNED_GTK_BUS")),
      "a portal-owned session bus is required; run ./ao test --gtk");
  }

  std::string makeProbeApplicationId()
  {
    auto guidPtr = GCharPtr{::g_dbus_generate_guid()};
    return std::format("{}.run{}", kApplicationIdPrefix, guidPtr.get());
  }

  GObjectPtr<::GDBusConnection> connectSessionBus()
  {
    ::GError* rawError = nullptr;
    auto connectionPtr = GObjectPtr<::GDBusConnection>{::g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &rawError)};

    if (!connectionPtr)
    {
      throwGlibError("failed to connect to the private session bus", rawError);
    }

    return connectionPtr;
  }

  std::string queryNameOwner(::GDBusConnection* const connection, std::string const& applicationId)
  {
    ::GError* rawError = nullptr;
    auto replyPtr = GVariantPtr{::g_dbus_connection_call_sync(connection,
                                                              "org.freedesktop.DBus",
                                                              "/org/freedesktop/DBus",
                                                              "org.freedesktop.DBus",
                                                              "GetNameOwner",
                                                              ::g_variant_new("(s)", applicationId.c_str()),
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

  RegisteredApplication registerApplication(std::string const& applicationId,
                                            ::GApplicationFlags const flags,
                                            std::string_view const expectedOwner)
  {
    auto ownerBefore = std::string{"-"};
    auto sessionConnectionPtr = GObjectPtr<::GDBusConnection>{};

    if (!expectedOwner.empty())
    {
      sessionConnectionPtr = connectSessionBus();
      ownerBefore = queryNameOwner(sessionConnectionPtr.get(), applicationId);
      requireProbe(ownerBefore == expectedOwner, "the original GApplication owner changed before registration");
    }

    auto appPtr = GObjectPtr<::GApplication>{::g_application_new(applicationId.c_str(), flags)};

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
                                               .ownerAfter = queryNameOwner(connection, applicationId),
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

  std::int32_t runInstance(std::string const& applicationId,
                           std::string_view const role,
                           std::string_view const expectedOwner)
  {
    requireOwnedProbeSessionBus();
    requireProbe(
      ::g_application_id_is_valid(applicationId.c_str()) != FALSE, "GApplication probe application ID is invalid");
    auto const replace = role == "replace";
    auto registered =
      registerApplication(applicationId, static_cast<::GApplicationFlags>(applicationFlags(replace)), expectedOwner);
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
                  std::string const& applicationId,
                  std::string_view const role,
                  std::string_view const expectedOwner)
    {
      auto const flags = static_cast<::GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
      auto launcherPtr = GObjectPtr<::GSubprocessLauncher>{::g_subprocess_launcher_new(flags)};
      auto arguments = std::vector{executablePath.string(),
                                   std::string{kInstanceOption},
                                   applicationId,
                                   std::string{role},
                                   std::string{expectedOwner}};
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

  std::int32_t runOrdinaryRemoteScenario(std::string const& applicationId)
  {
    auto owner = ProbeInstance{currentExecutablePath(), applicationId, "owner", ""};
    auto const ownerObservation = parseObservation(owner.readObservationLine());
    verifyOriginalOwner(ownerObservation);

    auto ordinary = ProbeInstance{currentExecutablePath(), applicationId, "ordinary", ownerObservation.connectionName};
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

  std::int32_t runReplacementScenario(std::string const& applicationId)
  {
    auto owner = ProbeInstance{currentExecutablePath(), applicationId, "owner", ""};
    auto const ownerObservation = parseObservation(owner.readObservationLine());
    verifyOriginalOwner(ownerObservation);

    auto replacement =
      ProbeInstance{currentExecutablePath(), applicationId, "replace", ownerObservation.connectionName};
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

  std::int32_t runInvocationIsolationScenario(std::string const& applicationId)
  {
    auto const executablePath = currentExecutablePath();
    auto owner = ProbeInstance{executablePath, applicationId, "owner", ""};
    auto const ownerObservation = parseObservation(owner.readObservationLine());
    verifyOriginalOwner(ownerObservation);
    auto connectionPtr = connectSessionBus();
    requireProbe(queryNameOwner(connectionPtr.get(), applicationId) == ownerObservation.connectionName,
                 "the previous invocation's owner disappeared before the isolation check");

    constexpr auto kScenarios =
      std::array{std::pair{"ordinary-remote"sv, "ordinary-remote: remote=yes owner-unchanged=yes\n"sv},
                 std::pair{"replacement"sv, "replacement: primary=yes owner-changed=yes\n"sv}};
    constexpr auto kTimeout = std::chrono::seconds{5};

    // Keep the previous name owned while real outer probes choose their own IDs.
    // Isolation must not depend on HUP cleanup or on the old owner's scheduling.
    for (auto const& [scenario, expectedOutput] : kScenarios)
    {
      auto const result = ao::test::runProbeProcess(executablePath, scenario, kTimeout);
      requireProbe(result.hasSuccessfulExit(),
                   std::format("independent {} probe failed: launch error: {}; timed out: {}; stderr: {}",
                               scenario,
                               result.launchError,
                               result.timedOut,
                               result.standardError));
      requireProbe(result.standardError.empty(),
                   std::format("independent {} probe emitted stderr: {}", scenario, result.standardError));
      requireProbe(result.standardOutput == expectedOutput,
                   std::format("independent {} probe reported unexpected output: {}", scenario, result.standardOutput));
      requireProbe(queryNameOwner(connectionPtr.get(), applicationId) == ownerObservation.connectionName,
                   "an independent invocation changed the previous application ID owner");
    }

    owner.stop();
    owner.waitForSuccess();
    std::println("invocation-isolation: ordinary=yes replacement=yes previous-owner-unchanged=yes");
    return 0;
  }

  std::int32_t runScenario(std::string_view const scenario)
  {
    requireOwnedProbeSessionBus();
    // The portal keeps the bus alive beyond this probe and its instances.
    // Borrow it instead of introducing GTestDBus's asynchronous PID watchdog.
    // Each invocation has its own name, even if an earlier owner has not exited.
    auto const applicationId = makeProbeApplicationId();

    if (scenario == "ordinary-remote")
    {
      return runOrdinaryRemoteScenario(applicationId);
    }

    if (scenario == "replacement")
    {
      return runReplacementScenario(applicationId);
    }

    if (scenario == "invocation-isolation")
    {
      return runInvocationIsolationScenario(applicationId);
    }

    return 2;
  }
} // namespace

int main(int argc, char* argv[])
{
  try
  {
    if (argc == 5 && std::string_view{argv[1]} == kInstanceOption)
    {
      return runInstance(argv[2], argv[3], argv[4]);
    }

    if (argc == 3 && std::string_view{argv[1]} == kScenarioOption)
    {
      auto const scenario = std::string_view{argv[2]};

      if (scenario == "unowned-bus" || scenario == "mismatched-bus" || scenario == "unowned-instance")
      {
        // Admission regressions use inert endpoints in this fresh process,
        // before any GIO worker or D-Bus connection can be created.
        ::g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/aobus-inert-probe", TRUE);
        ::g_unsetenv("AOBUS_OWNED_GTK_BUS");

        if (scenario == "mismatched-bus")
        {
          ::g_setenv("AOBUS_OWNED_GTK_BUS", "unix:path=/other-inert-probe", TRUE);
        }

        if (scenario == "unowned-instance")
        {
          return runInstance(kApplicationIdPrefix, "owner", "");
        }
      }

      return runScenario(scenario);
    }

    return 2;
  }
  catch (std::exception const& error)
  {
    std::println(std::cerr, "GApplication replacement probe failed: {}", error.what());
    return 1;
  }
}
