#include "rpe/core/TypeBridge.h"

#include <mutex>
#include <string>
#include <atomic>
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
            // Bumped on every registration change; atomic so readers skip the mutex.
            std::atomic<uint64_t> generation { 0 };
        };

        Registry& registry()
        {
            static Registry r;
            return r;
        }

        // Normalise scope separators: flecs paths use "." (e.g. "game.Transform"),
        // RTTR names use "::" (e.g. "game::Transform"). Compare them on equal terms
        // by collapsing both to "::".
        std::string normalizeScopes(std::string s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '.')
                {
                    out += "::";
                }
                else if (s[i] == ':' && i + 1 < s.size() && s[i + 1] == ':')
                {
                    out += "::";
                    ++i;
                }
                else
                {
                    out += s[i];
                }
            }
            return out;
        }

        // The final segment, after the last "." or "::" (or the whole string).
        std::string shortName(const std::string& s)
        {
            const auto dc = s.rfind("::");
            const auto dot = s.rfind('.');
            size_t pos = std::string::npos;
            size_t skip = 0;
            if (dc != std::string::npos)
            {
                pos = dc;
                skip = 2;
            }
            if (dot != std::string::npos && (pos == std::string::npos || dot > pos))
            {
                pos = dot;
                skip = 1;
            }
            return pos == std::string::npos ? s : s.substr(pos + skip);
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
        r.generation.fetch_add(1, std::memory_order_relaxed);
    }

    void TypeBridge::registerAlias(rttr::type t, std::string_view flecsName)
    {
        if (!t.is_valid() || flecsName.empty())
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.aliases[std::string(flecsName)] = t.get_id();
        r.generation.fetch_add(1, std::memory_order_relaxed);
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
        r.generation.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t TypeBridge::registryGeneration()
    {
        return registry().generation.load(std::memory_order_relaxed);
    }

    rttr::type TypeBridge::resolveByName(std::string_view flecsName)
    {
        if (flecsName.empty())
        {
            return rttr::type::get_by_name(std::string()); // invalid
        }
        const std::string name(flecsName);

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

        // 2) Exact full-name match, separator-insensitive: a flecs path like
        //    "game.Transform" matches the RTTR type "game::Transform". This is the
        //    UNAMBIGUOUS path — pass the full flecs component path here and two
        //    components that share a short name ("Panel") still resolve correctly.
        const std::string normName = normalizeScopes(name);
        for (const auto& [id, entry] : r.map)
        {
            if (normalizeScopes(entry.type.get_name().to_string()) == normName)
            {
                return entry.type;
            }
        }

        // 2.5) Scoped-SUFFIX match: the flecs path is a trailing, scope-aligned part of
        //      the RTTR full name (flecs "game.Transform" ↔ RTTR "app::game::Transform").
        //      This STILL disambiguates same-leaf types by namespace, unlike the bare
        //      short-name fallback. Deterministic: closest (shortest) full name wins,
        //      ties broken lexicographically — so debug and release always agree.
        {
            const std::string suffix = "::" + normName;
            rttr::type best = rttr::type::get_by_name(std::string());
            std::string bestFull;
            for (const auto& [id, entry] : r.map)
            {
                const std::string full = normalizeScopes(entry.type.get_name().to_string());
                if (full.size() > suffix.size()
                    && full.compare(full.size() - suffix.size(), suffix.size(), suffix) == 0
                    && (!best.is_valid() || full.size() < bestFull.size()
                        || (full.size() == bestFull.size() && full < bestFull)))
                {
                    best = entry.type;
                    bestFull = full;
                }
            }
            if (best.is_valid())
            {
                return best;
            }
        }

        // 3) Short-name (leaf) fallback — AMBIGUOUS when two bridged types share a leaf
        //    ("game::Panel" vs "ui::Panel") and only the leaf was given. Pick the
        //    smallest full name DETERMINISTICALLY (unordered_map order otherwise varies
        //    between builds — the classic "works in debug, wrong in release"). Prefer a
        //    full path (step 2) or a scoped suffix (2.5) so this is never reached.
        const std::string target = shortName(name);
        rttr::type best = rttr::type::get_by_name(std::string());
        std::string bestFull;
        for (const auto& [id, entry] : r.map)
        {
            const std::string full = entry.type.get_name().to_string();
            if (shortName(full) == target && (!best.is_valid() || full < bestFull))
            {
                best = entry.type;
                bestFull = full;
            }
        }
        return best.is_valid() ? best : rttr::type::get_by_name(std::string()); // invalid
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
