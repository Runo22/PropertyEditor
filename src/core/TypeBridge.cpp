#include "rpe/core/TypeBridge.h"

#include <mutex>
#include <unordered_map>

namespace rpe {

namespace {

struct Registry {
    std::mutex                                            mutex;
    std::unordered_map<rttr::type::type_id, TypeBridge::Wrapper> map;
};

Registry& registry()
{
    static Registry r;
    return r;
}

} // namespace

void TypeBridge::registerWrapper(rttr::type t, Wrapper w)
{
    if (!t.is_valid() || !w) return;
    auto& r = registry();
    std::lock_guard<std::mutex> lk(r.mutex);
    r.map[t.get_id()] = std::move(w);
}

rttr::variant TypeBridge::wrap(rttr::type t, void* obj)
{
    if (!t.is_valid() || !obj) return {};
    auto& r = registry();
    std::lock_guard<std::mutex> lk(r.mutex);
    const auto it = r.map.find(t.get_id());
    return it != r.map.end() ? it->second(obj) : rttr::variant();
}

bool TypeBridge::has(rttr::type t)
{
    if (!t.is_valid()) return false;
    auto& r = registry();
    std::lock_guard<std::mutex> lk(r.mutex);
    return r.map.find(t.get_id()) != r.map.end();
}

} // namespace rpe
