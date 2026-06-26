#include "rpe/core/TypeBridge.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace rpe
{

    namespace
    {

        struct Entry
        {
            rttr::type type = rttr::type::get<void>();
            TypeBridge::Wrapper wrap = nullptr;
            TypeBridge::Cloner clone = nullptr;
        };

        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<rttr::type::type_id, Entry> map;
            // Explicit flecs-name → type aliases. The std::string keys live here in
            // rpe_core (never in a plugin), so destroying this map is always safe.
            std::unordered_map<std::string, rttr::type::type_id> aliases;
        };

        Registry& registry()
        {
            static Registry r;
            return r;
        }

        // The segment after the last "::" (or the whole string if none).
        std::string shortName(const std::string& s)
        {
            const auto pos = s.rfind("::");
            return pos == std::string::npos ? s : s.substr(pos + 2);
        }

    } // namespace

    void TypeBridge::registerEntry(rttr::type t, Wrapper wrap, Cloner clone)
    {
        if (!t.is_valid() || !wrap || !clone)
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.map[t.get_id()] = Entry { t, wrap, clone };
    }

    void TypeBridge::registerAlias(rttr::type t, const char* flecsName)
    {
        if (!t.is_valid() || !flecsName || flecsName[0] == '\0')
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.aliases[flecsName] = t.get_id();
    }

    void TypeBridge::unregisterType(rttr::type t)
    {
        if (!t.is_valid())
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.map.erase(t.get_id());
        // Drop any aliases pointing at this type so a stale name can't resolve to a
        // freed (unloaded-plugin) entry.
        for (auto it = r.aliases.begin(); it != r.aliases.end();)
        {
            it = (it->second == t.get_id()) ? r.aliases.erase(it) : std::next(it);
        }
    }

    rttr::type TypeBridge::resolveByName(const char* flecsName)
    {
        if (!flecsName || flecsName[0] == '\0')
        {
            return rttr::type::get_by_name(std::string()); // invalid
        }
        const std::string name = flecsName;

        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);

        // 1) Explicit alias (registerType<T>(name) / registerAlias) wins.
        if (const auto a = r.aliases.find(name); a != r.aliases.end())
        {
            if (const auto e = r.map.find(a->second); e != r.map.end())
            {
                return e->second.type;
            }
        }

        // 2) Exact RTTR name that is also bridged.
        for (const auto& [id, entry] : r.map)
        {
            if (entry.type.get_name().to_string() == name)
            {
                return entry.type;
            }
        }

        // 3) Short name match (flecs short names vs. scoped RTTR registrations).
        const std::string target = shortName(name);
        for (const auto& [id, entry] : r.map)
        {
            if (shortName(entry.type.get_name().to_string()) == target)
            {
                return entry.type;
            }
        }
        return rttr::type::get_by_name(std::string()); // invalid
    }

    rttr::variant TypeBridge::wrap(rttr::type t, void* obj)
    {
        if (!t.is_valid() || !obj)
        {
            return {};
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        const auto it = r.map.find(t.get_id());
        return it != r.map.end() ? it->second.wrap(obj) : rttr::variant();
    }

    rttr::variant TypeBridge::clone(rttr::type t, void* obj)
    {
        if (!t.is_valid() || !obj)
        {
            return {};
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        const auto it = r.map.find(t.get_id());
        return it != r.map.end() ? it->second.clone(obj) : rttr::variant();
    }

    bool TypeBridge::has(rttr::type t)
    {
        if (!t.is_valid())
        {
            return false;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        return r.map.find(t.get_id()) != r.map.end();
    }

    std::vector<rttr::type> TypeBridge::registeredTypes()
    {
        std::vector<rttr::type> out;
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        out.reserve(r.map.size());
        for (const auto& [id, entry] : r.map)
        {
            out.push_back(entry.type);
        }
        return out;
    }

} // namespace rpe
