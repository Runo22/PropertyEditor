#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QString>
#include <QStringList>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  RttrVariantWrapper — a copyable handle around a single rttr value that you
    //  want to inspect/edit through the property editor.
    //
    //  This is the glue for the "RTTR variant connection" feature. Two modes:
    //
    //   • Owned  : holds the value by value (setVariant). Edits mutate the internal
    //              copy; read it back with value(). Works for any registered type —
    //              no TypeBridge registration needed.
    //   • Linked : points at an external object via (type, void*) using TypeBridge
    //              (makeLinked). Edits write straight through to that object. The
    //              type must have been registered with rpe::TypeBridge::registerType.
    //
    //  In both modes the wrapper keeps the storage that instance() references, so
    //  the returned rttr::instance stays valid for as long as the wrapper lives.
    // ─────────────────────────────────────────────────────────────────────────────
    class RttrVariantWrapper
    {
    public:
        RttrVariantWrapper() = default;

        // Own a copy of `v`.
        explicit RttrVariantWrapper(rttr::variant v);

        // Point at an external object (requires TypeBridge::registerType<T>()).
        static RttrVariantWrapper makeLinked(rttr::type type, void* obj);

        // ── State ────────────────────────────────────────────────────────────────
        bool isValid() const;
        bool isLinked() const
        {
            return _linked;
        }
        rttr::type type() const;
        QString typeName() const;

        // A live rttr::instance for traversal. Valid while this wrapper lives.
        rttr::instance instance();

        // Re-point a linked wrapper at a new object address (e.g. after the ECS
        // component moved in memory). No-op for owned wrappers.
        void relink(void* obj);

        // Owned value access (only meaningful when !isLinked()).
        const rttr::variant& value() const
        {
            return _owned;
        }
        rttr::variant& value()
        {
            return _owned;
        }

        void clear();

        // ── Property access by dot path (see RttrBridge for grammar) ─────────────
        QStringList topLevelPropertyNames() const;
        rttr::variant get(const QString& path) const;
        bool set(const QString& path, const rttr::variant& value);

        QString displayString() const;

    private:
        rttr::variant _owned;  // storage when !_linked
        rttr::variant _access; // typed-pointer wrapper when _linked
        rttr::type _type = rttr::type::get<void>();
        bool _linked = false;
    };

} // namespace rpe
