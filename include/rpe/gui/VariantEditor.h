#pragma once

#include "rpe/core/rttr_prelude.h"

#include "rpe/core/RttrVariantWrapper.h"

#include <QWidget>


class QLabel;

namespace rpe {

class PropertyEditor;

// ─────────────────────────────────────────────────────────────────────────────
//  VariantEditor — SECOND, independent feature.
//
//  Give it any registered struct wrapped in an rttr::variant and it builds a
//  property editor/inspector for it. Two flavours:
//    • setVariant(v)        — edits an owned copy; read it back with variant().
//    • setLinked(type, ptr) — edits an external object in place (write-through).
//
//  This shares the same grid/delegate as the ECS browser but is wired through
//  RttrVariantWrapper and has no dependency on flecs.
// ─────────────────────────────────────────────────────────────────────────────
class VariantEditor : public QWidget
{
    Q_OBJECT
public:
    explicit VariantEditor(QWidget* parent = nullptr);

    // Edit an owned copy of `v`. Retrieve the (possibly edited) value via variant().
    void setVariant(const rttr::variant& v);

    // Edit an external object in place; caller guarantees its lifetime.
    void setLinked(rttr::type type, void* obj);

    template <class T>
    void edit(T& obj) { setLinked(rttr::type::get<T>(), &obj); }

    void clear();

    // Re-read values from the source (use when the linked object changed externally).
    void refreshFromSource();

    bool isReadOnly() const;
    void setReadOnly(bool ro);

    const rttr::variant&      variant() const { return _wrapper.value(); }
    const RttrVariantWrapper& wrapper() const { return _wrapper; }
    PropertyEditor*           editor()  const { return _editor; }

signals:
    // Emitted after an edit mutates the wrapped value.
    void valueChanged(const QString& path, const rttr::variant& newValue);

private:
    void _rebind();

    RttrVariantWrapper _wrapper;
    PropertyEditor*    _editor    = nullptr;
    QLabel*            _typeLabel = nullptr;
};

} // namespace rpe
