#include "rpe/core/FlagsSupport.h"

#include <mutex>
#include <unordered_map>

namespace rpe
{

    namespace
    {
        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<rttr::type::type_id, FlagsBridge::BuildFn> map;
        };

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
        r.map[enumType.get_id()] = build;
    }

    bool FlagsBridge::canBuild(rttr::type enumType)
    {
        if (!enumType.is_valid())
        {
            return false;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        return r.map.find(enumType.get_id()) != r.map.end();
    }

    rttr::variant FlagsBridge::build(rttr::type enumType, int64_t bits)
    {
        if (!enumType.is_valid())
        {
            return {};
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        const auto it = r.map.find(enumType.get_id());
        return it != r.map.end() ? it->second(bits) : rttr::variant();
    }

} // namespace rpe
