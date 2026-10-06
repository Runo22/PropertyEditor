#pragma once

#include "rpe/core/rttr_prelude.h"

#include "rpe/gui/PropertyModel.h"

#include <QHash>
#include <QPersistentModelIndex>
#include <QSet>
#include <QString>
#include <QWidget>

#include <functional>

class QTreeView;
class QLineEdit;
class QToolButton;
class QLabel;
class QSortFilterProxyModel;

namespace rpe
{

    class PropertyDelegate;

    // ─────────────────────────────────────────────────────────────────────────────
    //  PropertyEditor — the reusable, embeddable property grid widget.
    //
    //  Drop it into a window, a QDockWidget, or a side panel. Bind a type, then feed
    //  it live data via refresh()/setPropertyValue(), or make it a data editor by
    //  switching to the WriteBack edit policy and giving it an instance provider.
    // ─────────────────────────────────────────────────────────────────────────────
    class PropertyEditor : public QWidget
    {
        Q_OBJECT

    public:
        explicit PropertyEditor(QWidget* parent = nullptr);

        // ── Schema / data ────────────────────────────────────────────────────────
        void bindType(rttr::type type);
        void unbind();
        void refresh(const rttr::instance& obj);
        void setPropertyValue(const QString& path, rttr::variant val);

        // ── Behaviour ────────────────────────────────────────────────────────────
        void setReadOnly(bool ro);
        bool isReadOnly() const;

        void setEditPolicy(EditPolicy p);
        EditPolicy editPolicy() const;

        // Object that WriteBack edits target (and that refresh() reads after a write).
        void setInstanceProvider(std::function<rttr::instance()> provider);

        // Guard wrapped around WriteBack writes when the target object is owned by
        // another thread (see rpe/core/AccessGuard.h).
        void setWriteGuard(AccessGuard guard);

        // Route committed edits to `sink` (mirror mode) — see PropertyModel::setEditSink.
        void setEditSink(std::function<void(const QString&, const rttr::variant&)> sink);

        // Leaf dot-paths that are currently visible (all ancestors expanded). With
        // `onlyExpanded == false`, returns every leaf path regardless of expansion.
        QStringList visibleLeafPaths(bool onlyExpanded = true) const;

        // Convenience for static objects: bind the type and continuously edit `obj`.
        template <class T>
        void editObject(T& obj)
        {
            bindType(rttr::type::get<T>());
            setEditPolicy(EditPolicy::WriteBack);
            setInstanceProvider([ptr = &obj] { return rttr::instance(*ptr); });
            refresh(rttr::instance(obj));
        }

        // ── Pinning (watch list) ─────────────────────────────────────────────────
        // When enabled, the context menu offers "Pin to watch list" / "Unpin" and
        // emits pinRequested/unpinRequested — the host (EntityComponentBrowser +
        // PinnedPropertiesWidget) does the actual pinning. Off by default.
        void setPinningEnabled(bool on)
        {
            _pinningEnabled = on;
        }
        // Tint these paths as pinned in the tree (see PropertyModel::setPinnedPaths).
        void setPinnedPaths(const QSet<QString>& paths);

        // ── Chrome ───────────────────────────────────────────────────────────────
        void setToolbarVisible(bool visible);
        void expandAll();
        // expandAll(), except rows the user has COLLAPSED by hand while this type
        // was bound stay collapsed. Remembered per bound RTTR type for the editor's
        // lifetime, so switching entities (same component) or re-selecting the
        // component keeps the tree the way you left it. Bulk operations (filtering,
        // expandAll itself) are not remembered — only explicit per-row collapses.
        void expandAllExceptCollapsed();

        PropertyModel* model() const
        {
            return _model;
        }
        QTreeView* view() const
        {
            return _view;
        }

        // Drag a number's NAME left/right to change it (on by default) — the
        // pointer shows a horizontal-resize cursor over such names. Shift = fine,
        // Ctrl = coarse; Esc during the drag restores the starting value. The
        // step is the property's Step hint (else 0.1 for floats, 1 for integers),
        // the result kept within its Min/Max hints. Read-only values don't scrub.
        void setDragToScrubEnabled(bool on);
        bool isDragToScrubEnabled() const
        {
            return _scrubEnabled;
        }

    protected:
        bool eventFilter(QObject* obj, QEvent* ev) override;

    signals:
        void propertyEdited(const QString& path, const rttr::variant& newValue);
        // Pin/unpin requests from the context menu (only when pinning is enabled).
        void pinRequested(const QString& path);
        void unpinRequested(const QString& path);

    private slots:
        void _onFilterChanged(const QString& text);
        void _onResetAll();
        void _onContextMenu(const QPoint& pos);

    private:
        void _setupUi();
        void _pushExpansionState();
        // Rows the user collapsed by hand, per bound type name (see
        // expandAllExceptCollapsed).
        QHash<QString, QSet<QString>> _collapsedByType;
        QString _boundTypeName;
        // True while the editor itself expands/collapses in bulk (bind, expandAll,
        // filtering). QTreeView emits a per-row signal even for those, and they
        // must not be mistaken for the user's own choices.
        bool _bulkExpanding = false;
        // Enable the Reset control only while the current component has a frozen value.
        void _updateResetEnabled();
        // Drag-to-scrub (see setDragToScrubEnabled).
        bool _isScrubbable(const QModelIndex& nameIndex) const;
        void _applyScrub(int dx, Qt::KeyboardModifiers mods);
        void _endScrub(bool restore);
        bool _scrubEnabled = true;
        bool _scrubArmed = false; // pressed on a scrubbable name; not dragging yet
        bool _scrubbing = false;  // past the drag threshold
        QPersistentModelIndex _scrubValue; // the value cell (proxy index)
        int _scrubPressX = 0;
        double _scrubStart = 0;
        double _scrubLast = 0;
        // Show/hide the "no reflected properties" hint for the just-bound type.
        void _updateEmptyHint(rttr::type t);

        PropertyModel* _model = nullptr;
        PropertyDelegate* _delegate = nullptr;
        QSortFilterProxyModel* _proxy = nullptr;
        QTreeView* _view = nullptr;
        QWidget* _toolbar = nullptr;
        QLineEdit* _filter = nullptr;
        QToolButton* _resetBtn = nullptr;
        // Explains an empty panel when the bound type reflects no properties, instead
        // of leaving a silent blank (see _updateEmptyHint).
        QLabel* _emptyHint = nullptr;
        bool _pinningEnabled = false;
    };

} // namespace rpe
