// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <any>
#include <functional>
#include <memory>
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
    ao::Result<Reply<Command>> execute(Command const& command)
    {
      auto it = _handlers.find(std::type_index(typeid(Command)));
      if (it == _handlers.end())
      {
        AO_THROW_FORMAT(ao::Exception, "no handler registered for {}", typeid(Command).name());
      }
      auto& handler = *std::any_cast<std::shared_ptr<Handler<Command>>>(it->second);
      return handler(command);
    }

    template<class Command>
    void registerHandler(Handler<Command> handler)
    {
      auto const [it, inserted] =
        _handlers.try_emplace(std::type_index(typeid(Command)), std::make_shared<Handler<Command>>(std::move(handler)));

      if (!inserted)
      {
        AO_THROW_FORMAT(ao::Exception, "duplicate handler for {}", typeid(Command).name());
      }
    }

  private:
    std::unordered_map<std::type_index, std::any> _handlers;
  };
}
