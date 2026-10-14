#include "rpe/core/FlagsSupport.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace rpe
{

    namespace
    {
        // Keyed by type id — an ADDRESS, which RTTR may hand to another type once a
        // plugin's type is freed — so each entry keeps its type's name and only
        // answers for a type of that name. (A reloaded plugin registers again.)
        struct Entry
        {
            std::string name;
            FlagsBridge::BuildFn build = nullptr;
        };
        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<rttr::type::type_id, Entry> map;
        };

        FlagsBridge::BuildFn lookup(Registry& r, rttr::type t)
        {
            const auto it = r.map.find(t.get_id());
            if (it == r.map.end() || it->second.name != t.get_name().to_string())
            {
                return nullptr;
            }
            return it->second.build;
        }

        Registry& registry()
        {
            static Registry r;
            return r;
        }
    } // namespace

    void FlagsBridge::registerEntry(rttr::type enumType, BuildFn build)
    {
        if (!enumType.is_valid() || !build)
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.map[enumType.get_id()] = Entry { enumType.get_name().to_string(), build };
    }

    bool FlagsBridge::canBuild(rttr::type enumType)
    {
        if (!enumType.is_valid())
        {
            return false;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        return lookup(r, enumType) != nullptr;
    }

    rttr::variant FlagsBridge::build(rttr::type enumType, int64_t bits)
    {
        if (!enumType.is_valid())
        {
            return {};
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        const FlagsBridge::BuildFn fn = lookup(r, enumType);
        return fn ? fn(bits) : rttr::variant();
    }

} // namespace rpe
