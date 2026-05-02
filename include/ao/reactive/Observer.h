#pragma once

namespace ao::reactive
{
  template<typename Id, typename... Args>
  class Observerable;

  template<typename Id, typename... Args>
  class Observer
  {
  public:
    virtual ~Observer() = default;

  protected:
    virtual void onAttached() {};
    virtual void onBeginInsert(Id id) {};
    virtual void onEndInsert(Id id, Args&&...) {};
    virtual void onBeginUpdate(Id id, Args&&...) {};
    virtual void onEndUpdate(Id id, Args&&...) {};
    virtual void onBeginRemove(Id id, Args&&...) {};
    virtual void onEndRemove(Id id) {};
    virtual void onBeginClear() {};
    virtual void onEndClear() {};
    virtual void onDetached() {};

    friend class Observerable<Args...>;
  };
}
