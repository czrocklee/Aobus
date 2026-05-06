// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "BusLog.h"

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/utility/Log.h>

#include <any>
#include <chrono>
#include <functional>
#include <memory>
#include <source_location>
#include <typeindex>
#include <unordered_map>

namespace ao::app
{
  class CommandBus final
  {
  public:
    template<class Command>
    using Reply = typename Command::Reply;

    template<class Command>
    using Handler = std::move_only_function<ao::Result<Reply<Command>>(Command const&)>;

    template<class Command>
    ao::Result<Reply<Command>> execute(Command const& command,
                                       std::source_location const src = std::source_location::current())
    {
      auto it = _handlers.find(std::type_index(typeid(Command)));
      if (it == _handlers.end())
      {
        APP_LOG_ERROR("cmd ✗ {}: no handler registered ({}:{})",
                      detail::busTypeName<Command>(),
                      detail::shortFileName(src),
                      src.line());
        AO_THROW_FORMAT(ao::Exception, "no handler registered for {}", typeid(Command).name());
      }

      auto& handler = *std::any_cast<std::shared_ptr<Handler<Command>>>(it->second);

      if (ao::log::Log::getAppLogger()->should_log(spdlog::level::trace))
      {
        auto const t0 = std::chrono::steady_clock::now();
        auto result = handler(command);
        auto const dt = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0);
        if (result)
        {
          APP_LOG_TRACE("cmd ✓ {} ({:.3}ms) ({}:{})",
                        detail::busTypeName<Command>(),
                        dt.count(),
                        detail::shortFileName(src),
                        src.line());
        }
        else
        {
          APP_LOG_DEBUG("cmd ✗ {}: {} ({}:{})",
                        detail::busTypeName<Command>(),
                        result.error().message,
                        detail::shortFileName(src),
                        src.line());
        }
        return result;
      }

      return handler(command);
    }

    template<class Command>
    void registerHandler(Handler<Command> handler, std::source_location const src = std::source_location::current())
    {
      auto const [it, inserted] =
        _handlers.try_emplace(std::type_index(typeid(Command)), std::make_shared<Handler<Command>>(std::move(handler)));

      if (!inserted)
      {
        APP_LOG_WARN("cmd ⇍ {}: handler already registered ({}:{})",
                     detail::busTypeName<Command>(),
                     detail::shortFileName(src),
                     src.line());
        AO_THROW_FORMAT(ao::Exception, "duplicate handler for {}", typeid(Command).name());
      }

      APP_LOG_DEBUG("cmd ⇧ {} ({}:{})", detail::busTypeName<Command>(), detail::shortFileName(src), src.line());
    }

  private:
    std::unordered_map<std::type_index, std::any> _handlers;
  };
}
