#include "rpe/core/OptionalSupport.h"

#include <mutex>
#include <unordered_map>

namespace rpe
{

    namespace
    {
        struct Entry
        {
            rttr::type inner = rttr::type::get<void>();
            OptionalBridge::HasValueFn hasValue = nullptr;
            OptionalBridge::EngageFn engage = nullptr;
            OptionalBridge::DisengageFn disengage = nullptr;
        };

        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<rttr::type::type_id, Entry> map;
        };

        Registry& registry()
        {
            static Registry r;
            return r;
        }

        const Entry* find(rttr::type t)
        {
            if (!t.is_valid())
            {
                return nullptr;
            }
            auto& r = registry();
            std::lock_guard<std::mutex> lk(r.mutex);
            const auto it = r.map.find(t.get_id());
            return it != r.map.end() ? &it->second : nullptr;
        }
    } // namespace

    void OptionalBridge::registerEntry(rttr::type optType, rttr::type innerType,
                                       HasValueFn hasValue, EngageFn engage, DisengageFn disengage)
    {
        if (!optType.is_valid() || !hasValue || !engage || !disengage)
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.map[optType.get_id()] = Entry { innerType, hasValue, engage, disengage };
    }

    bool OptionalBridge::isOptional(rttr::type t)
    {
        return find(t) != nullptr;
    }

    rttr::type OptionalBridge::innerType(rttr::type optType)
    {
        const Entry* e = find(optType);
        return e ? e->inner : rttr::type::get<void>();
    }

    bool OptionalBridge::hasValue(const rttr::variant& optVariant)
    {
        const Entry* e = find(optVariant.get_type());
        return e && e->hasValue(optVariant);
    }

    rttr::variant OptionalBridge::engage(rttr::type optType, const rttr::variant& innerValue)
    {
        const Entry* e = find(optType);
        return e ? e->engage(innerValue) : rttr::variant();
    }

    rttr::variant OptionalBridge::disengage(rttr::type optType)
    {
        const Entry* e = find(optType);
        return e ? e->disengage() : rttr::variant();
    }

} // namespace rpe
