// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <gio/gio.h>
#include <glibmm/refptr.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::async
{
  class Executor;
}

namespace Gio::DBus
{
  class Connection;
  class NodeInfo;
}

namespace ao::media
{
  /** Owns one private bus connection and its native callback producer.
   *
   * All outputs are deferred to the borrowed executor, which must outlive this
   * session. Output closures own their native values, never this session. The
   * caller closes its output admission before retirement; destruction joins the
   * native producer without requiring executor progress.
   */
  class [[nodiscard]] MprisBusSession final
  {
  public:
    struct InvocationDeleter final
    {
      void operator()(::GDBusMethodInvocation* invocation) const noexcept;
    };

    // An unanswered invocation owns its error path even if its queued closure
    // is destroyed without running. Returning a reply consumes release().
    using Invocation = std::unique_ptr<::GDBusMethodInvocation, InvocationDeleter>;

    struct Outputs final
    {
      std::function<void(Invocation)> handleRequest;
      std::function<void(Glib::RefPtr<Gio::DBus::Connection>, std::string)> acquired;
      std::function<void(std::string)> unavailable;
    };

    MprisBusSession(async::Executor& executor,
                    std::string address,
                    std::string busName,
                    bool uniqueInstance,
                    Glib::RefPtr<Gio::DBus::NodeInfo> nodeInfoPtr,
                    Outputs outputs);
    ~MprisBusSession();

    MprisBusSession(MprisBusSession const&) = delete;
    MprisBusSession& operator=(MprisBusSession const&) = delete;
    MprisBusSession(MprisBusSession&&) = delete;
    MprisBusSession& operator=(MprisBusSession&&) = delete;

    /// Seals native admission asynchronously; destruction supplies the join.
    void retire() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::media
