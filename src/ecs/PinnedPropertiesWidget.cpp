#include "rpe/ecs/PinnedPropertiesWidget.h"

#include "rpe/core/NamedElement.h"
#include "rpe/core/ReadOnly.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/ecs/ComponentScan.h"
#include "rpe/ecs/EcsMirror.h"
#include "rpe/gui/PropertyDelegate.h"
#include "rpe/gui/PropertyModel.h" // Q_DECLARE_METATYPE(rttr::variant)
#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeRelease.h"
#include "rpe/gui/VariantEditorFactory.h"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <functional>

namespace rpe
{

    namespace
    {
        enum ItemRole
        {
            EntityRole = Qt::UserRole,      // qulonglong
            ComponentRole,                  // QString (full scoped name / pair key)
            PathRole,                       // QString (dot-path)
            LastValueRoleUnused,            // (was the last value — now kept in _lastValues)
            RawIdRole,                      // qulonglong (pair id; 0 for plain components)
            PinReadOnlyRole,                // QString — why it can't be edited (empty: it can)
            PinMissingRole,                 // bool — the path doesn't resolve right now
            PinWholeRole,                   // bool — a whole array / struct (a summary)
        };

        constexpr int kValueColumn = 2; // Entity | Property | Value

        // U+2026 HORIZONTAL ELLIPSIS via an explicit escape: robust against the
        // compiler misreading the source encoding (a raw non-ASCII literal turns
        // into mojibake on MSVC without /utf-8).
        QString placeholderText()
        {
            return QStringLiteral("\u2026");
        }

        // Resolve a pinned leaf's DECLARED rttr type from its component name + dot
        // path, WITHOUT a live value. This lets an editor open the instant a property
        // is pinned \u2014 before the first mirror value lands \u2014 instead of showing a dead
        // "\u2026" cell that can't be edited. Returns an invalid type when it can't resolve
        // (unbridged component, or an array/map [index] segment we don't walk here);
        // callers then fall back to the live value's type.
        rttr::type pinDeclaredType(const QTreeWidgetItem* it)
        {
            const rttr::type invalid = rttr::type::get_by_name(std::string());
            if (!it)
            {
                return invalid;
            }
            rttr::type t = TypeBridge::resolveByName(it->data(0, ComponentRole).toString().toStdString());
            if (!t.is_valid())
            {
                return invalid;
            }
            const QString path = it->data(0, PathRole).toString();
            for (const QString& seg : path.split(QLatin1Char('.'), Qt::SkipEmptyParts))
            {
                if (seg.startsWith(QLatin1Char('['))) // array index / map key \u2014 not walked
                {
                    return invalid;
                }
                const rttr::property p = TypeRenderer::rawType(t).get_property(seg.toStdString());
                if (!p.is_valid())
                {
                    return invalid;
                }
                t = p.get_type();
            }
            return TypeRenderer::rawType(t);
        }

        // The type an editor for this row should be built from: the DECLARED type when
        // resolvable (so pins are editable before any value arrives), otherwise the
        // last mirrored value's type. Invalid when neither is available.
        using LastValues = QHash<const QTreeWidgetItem*, rttr::variant>;

        rttr::type pinLeafType(const QTreeWidgetItem* it, const LastValues& values)
        {
            const rttr::type dt = pinDeclaredType(it);
            if (dt.is_valid() && dt != rttr::type::get<void>())
            {
                return dt;
            }
            if (it)
            {
                const rttr::variant last = values.value(it);
                if (last.is_valid())
                {
                    return TypeRenderer::rawType(last.get_type());
                }
            }
            return rttr::type::get_by_name(std::string()); // invalid
        }

        // ─────────────────────────────────────────────────────────────────────────
        //  PinnedValueDelegate — type-specialized inline editor for the Value column,
        //  reusing the exact same factory as the property grid's PropertyDelegate:
        //  bool → check box, number → spin box, enum → combo, QColor → picker, path →
        //  file/dir picker, string → line edit, etc. The editor type is chosen from
        //  the LAST MIRRORED value's type (all we know here — there's no schema/hints
        //  in the pin list), and a committed value is routed straight to the channel
        //  as a pin edit (no text round-trip through the item model).
        // ─────────────────────────────────────────────────────────────────────────
        class PinnedValueDelegate : public QStyledItemDelegate
        {
        public:
            using Commit = std::function<void(int row, const rttr::variant&)>;
            using Arm = std::function<void(int row)>;

            PinnedValueDelegate(QTreeWidget* tree, const LastValues* values, Commit commit, Arm arm, QObject* parent)
                : QStyledItemDelegate(parent)
                , _tree(tree)
                , _values(values)
                , _commit(std::move(commit))
                , _arm(std::move(arm))
            {
            }

            void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
            {
                const QTreeWidgetItem* it = _tree->topLevelItem(index.row());
                if (index.column() == kValueColumn && it && it->data(0, PinMissingRole).toBool())
                {
                    // Not there right now: the "—" in the disabled text colour.
                    QStyleOptionViewItem o(option);
                    o.palette.setColor(QPalette::Text, o.palette.color(QPalette::Disabled, QPalette::Text));
                    QStyledItemDelegate::paint(painter, o, index);
                    return;
                }
                if (index.column() != kValueColumn || !it || it->data(0, PinReadOnlyRole).toString().isEmpty())
                {
                    QStyledItemDelegate::paint(painter, option, index);
                    return;
                }
                // Same read-only look as the property grid: full-cell background, the
                // value in the remaining width, a faint lock at the right edge.
                QStyleOptionViewItem o(option);
                initStyleOption(&o, index);
                paintItemReservingRight(painter, o, readOnlyLockWidth(option));
                paintReadOnlyLock(painter, option);
            }

            QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
            {
                TypeUseScope scope; // see rpe/core/TypeRelease.h
                const QTreeWidgetItem* row = _tree->topLevelItem(index.row());
                if (row && (!row->data(0, PinReadOnlyRole).toString().isEmpty() || row->data(0, PinMissingRole).toBool()))
                {
                    return nullptr; // read-only, or not there now: never an editor
                }
                // Drive the editor from the leaf's declared type (resolvable straight
                // after pinning) and fall back to the mirrored value's type. No hint
                // metadata in the pin list → default numeric ranges.
                const rttr::type t = pinLeafType(_tree->topLevelItem(index.row()), *_values);
                if (!t.is_valid())
                {
                    return nullptr;
                }
                QWidget* editor = varedit::makeEditor(t, QString(), {}, parent);
                if (editor)
                {
                    // An editor is actually open on this row now: pause live refresh
                    // for it until it closes (see the closeEditor connection).
                    _arm(index.row());
                    varedit::commitOnFocusLeave(const_cast<PinnedValueDelegate*>(this), editor);
                }
                return editor;
            }

            void setEditorData(QWidget* editor, const QModelIndex& index) const override
            {
                // Seed from the live value if one has arrived; otherwise the editor
                // opens at its type default (empty line edit, 0, first enum, …).
                TypeUseScope scope;
                varedit::setEditorData(editor, _valueAt(index));
                varedit::rememberOpeningValue(editor, pinLeafType(_tree->topLevelItem(index.row()), *_values));
            }

            void setModelData(QWidget* editor, QAbstractItemModel*, const QModelIndex& index) const override
            {
                TypeUseScope scope;
                const rttr::type t = pinLeafType(_tree->topLevelItem(index.row()), *_values);
                rttr::variant edit;
                {
                    const rttr::variant v = varedit::readEditorData(editor, t);
                    if (!v.is_valid() || varedit::unchangedSinceOpen(editor, v))
                    {
                        return; // left unchanged → no write (see unchangedSinceOpen)
                    }
                    // An enum travels as its integer (see PropertyDelegate::setModelData).
                    edit = v.get_type().is_enumeration() ? rttr::variant(static_cast<int64_t>(TypeRenderer::enumBits(v))) : v;
                }
                _commit(index.row(), edit);
            }

            void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override
            {
                editor->setGeometry(option.rect);
            }

        private:
            // Pins are a FLAT top-level list, so the visual row maps 1:1 to a
            // top-level item.
            rttr::variant _valueAt(const QModelIndex& index) const
            {
                QTreeWidgetItem* it = _tree->topLevelItem(index.row());
                return it ? _values->value(it) : rttr::variant();
            }

            QTreeWidget* _tree;
            const LastValues* _values;
            Commit _commit;
            Arm _arm;
        };
    } // namespace

    PinnedPropertiesWidget::PinnedPropertiesWidget(QWidget* parent)
        : QWidget(parent)
    {
        _setupUi();
        // A plugin unloading: drop the last values (a value read through a plugin's
        // property calls into it when destroyed). The rows keep their text; the next
        // poll refills the values.
        _releaseHandle = addTypeReleaseHandler([this](const std::vector<rttr::type>&) { _lastValues.clear(); });
        _timer = new QTimer(this);
        _timer->setInterval(33); // ~30 Hz, same cadence as the browser's mirror poll
        connect(_timer, &QTimer::timeout, this, &PinnedPropertiesWidget::pollNow);
    }

    PinnedPropertiesWidget::~PinnedPropertiesWidget()
    {
        removeTypeReleaseHandler(_releaseHandle);
        TypeUseScope scope;
        _lastValues.clear();
    }

    void PinnedPropertiesWidget::_setupUi()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        _title = new QLabel(tr("Pinned properties"), this);
        _title->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
        layout->addWidget(_title);

        _tree = new QTreeWidget(this);
        _tree->setColumnCount(3);
        _tree->setHeaderLabels({ tr("Entity"), tr("Property"), tr("Value") });
        _tree->setRootIsDecorated(false);
        _tree->setAlternatingRowColors(true);
        _tree->setUniformRowHeights(true);
        _tree->setTextElideMode(Qt::ElideMiddle); // long names shorten in the middle
        _tree->header()->setStretchLastSection(true);
        _tree->header()->resizeSection(0, 110);
        _tree->header()->resizeSection(1, 170);
        _tree->setContextMenuPolicy(Qt::CustomContextMenu);
        // No built-in edit triggers: editing is started explicitly below, ONLY for
        // the Value column — otherwise a double-click on the Property column would
        // let the label be renamed (ignored by logic, but visually corrupting).
        _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        // Type-specialized inline editor on the Value column (bool → check box, enum
        // → combo, number → spin box, …), just like the main property grid. A commit
        // is written back to the display and queued to the sim thread as a pin edit.
        auto* delegate = new PinnedValueDelegate(
            _tree,
            &_lastValues,
            [this](int row, const rttr::variant& v) { _commitValueEdit(row, v); },
            [this](int row) { _editingItem = _tree->topLevelItem(row); }, // an editor opened
            _tree);
        _tree->setItemDelegateForColumn(kValueColumn, delegate);
        // The editor is closed (commit or cancel) → live polling may refresh the row
        // again. Covers modal-picker editors too: they stay open across the dialog.
        connect(delegate, &QAbstractItemDelegate::closeEditor, this, [this] { _editingItem = nullptr; });
        layout->addWidget(_tree, 1);

        connect(_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
            // Only the Value column is editable (editItem on the label columns would
            // let them be renamed). The delegate decides whether a usable editor can
            // be built and, if so, arms the live-refresh pause — so a leaf is now
            // editable the moment it's pinned, not only once a value has arrived.
            if (column == kValueColumn)
            {
                _tree->editItem(item, kValueColumn);
            }
        });
        connect(_tree, &QTreeWidget::customContextMenuRequested, this, &PinnedPropertiesWidget::_onContextMenu);
    }

    void PinnedPropertiesWidget::setTitleVisible(bool visible)
    {
        _title->setVisible(visible);
    }

    void PinnedPropertiesWidget::setMirror(EcsMirror* mirror)
    {
        setChannel(mirror ? mirror->channel() : nullptr);
    }

    void PinnedPropertiesWidget::setChannel(std::shared_ptr<MirrorChannel> channel)
    {
        _channel = std::move(channel);
        _pushPins();
        if (_channel)
        {
            _timer->start();
        }
        else
        {
            _timer->stop();
        }
    }

    QVector<MirrorChannel::PinKey> PinnedPropertiesWidget::pins() const
    {
        QVector<MirrorChannel::PinKey> out;
        out.reserve(_tree->topLevelItemCount());
        for (int i = 0; i < _tree->topLevelItemCount(); ++i)
        {
            out.append(_itemKey(_tree->topLevelItem(i)));
        }
        return out;
    }

    bool PinnedPropertiesWidget::isPinned(qulonglong entity, const QString& component, const QString& path) const
    {
        return _findItem({ entity, component, path, 0 }) != nullptr;
    }

    QSet<QString> PinnedPropertiesWidget::pinnedPaths(qulonglong entity, const QString& component) const
    {
        QSet<QString> out;
        for (int i = 0; i < _tree->topLevelItemCount(); ++i)
        {
            const auto* it = _tree->topLevelItem(i);
            if (it->data(0, EntityRole).toULongLong() == entity
                && it->data(0, ComponentRole).toString() == component)
            {
                out.insert(it->data(0, PathRole).toString());
            }
        }
        return out;
    }

    MirrorChannel::PinKey PinnedPropertiesWidget::_itemKey(const QTreeWidgetItem* item)
    {
        return { item->data(0, EntityRole).toULongLong(),
                 item->data(0, ComponentRole).toString(),
                 item->data(0, PathRole).toString(),
                 item->data(0, RawIdRole).toULongLong() };
    }

    QTreeWidgetItem* PinnedPropertiesWidget::_findItem(const MirrorChannel::PinKey& key) const
    {
        for (int i = 0; i < _tree->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem* it = _tree->topLevelItem(i);
            if (_itemKey(it) == key)
            {
                return it;
            }
        }
        return nullptr;
    }

    void PinnedPropertiesWidget::pin(qulonglong entity, const QString& entityLabel,
                                     const QString& component, const QString& path, qulonglong rawId)
    {
        if (entity == 0 || component.isEmpty() || path.isEmpty() || isPinned(entity, component, path))
        {
            return;
        }
        // Display the leaf component name; the full scoped name stays in the data.
        // A pair key ("Rel (Target)") has no "." to trim — it shows as-is.
        // A pair's key is "Rel (Target)": both shortened, never cut inside them.
        const int open = component.endsWith(QLatin1Char(')')) ? component.indexOf(QStringLiteral(" (")) : -1;
        const QString compLeaf = open > 0 ? scopedLeaf(component.left(open)) + QStringLiteral(" (")
                + scopedLeaf(component.mid(open + 2, component.size() - open - 3)) + QLatin1Char(')')
                                          : scopedLeaf(component);

        _updating = true;
        auto* item = new QTreeWidgetItem(_tree);
        item->setText(0, entityLabel);
        {
            // A named element by its name: "stats.{armor}.value" shows "stats.armor".
            TypeUseScope scope;
            const rttr::type root = rawId != 0 ? rttr::type::get<void>() : TypeBridge::resolveByName(component.toUtf8().constData());
            item->setText(1, QStringLiteral("%1.%2").arg(compLeaf, root.is_valid() ? displayPath(root, path) : path));
        }
        const QString full = QStringLiteral("#%1  %2.%3").arg(entity).arg(component, path);
        item->setToolTip(0, full);
        item->setToolTip(1, full);
        item->setData(0, EntityRole, entity);
        item->setData(0, ComponentRole, component);
        item->setData(0, PathRole, path);
        item->setData(0, RawIdRole, rawId);
        // Same rule as the property grid (rpe::readOnlyReason): a getter-only
        // property, one marked read-only, or a locked type stays non-editable here
        // too — otherwise the watch list would be the back door around it.
        const QString readOnly = rawId != 0
            ? QString()
            : [&] {
                  TypeUseScope scope; // resolve and read with no release in between
                  return readOnlyReason(TypeBridge::resolveByName(component.toUtf8().constData()), path);
              }();
        item->setData(0, PinReadOnlyRole, readOnly);
        if (readOnly.isEmpty())
        {
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
        else
        {
            item->setToolTip(kValueColumn, readOnly);
        }
        {
            // A whole array / struct (when its declared type says so now; else the
            // first value will).
            TypeUseScope scope;
            const rttr::type dt = pinDeclaredType(item);
            if (dt.is_valid() && TypeRenderer::isExpandable(dt))
            {
                _markWhole(item, dt);
            }
        }
        item->setText(kValueColumn, placeholderText()); // until the first value lands
        _updating = false;

        _pushPins();
        emit pinsChanged();
    }

    void PinnedPropertiesWidget::unpin(qulonglong entity, const QString& component, const QString& path)
    {
        if (QTreeWidgetItem* it = _findItem({ entity, component, path, 0 }))
        {
            if (it == _editingItem)
            {
                _editingItem = nullptr;
            }
            {
                TypeUseScope scope;
                _lastValues.remove(it);
            }
            delete it;
            _pushPins();
            emit pinsChanged();
        }
    }

    void PinnedPropertiesWidget::clearPins()
    {
        if (_tree->topLevelItemCount() == 0)
        {
            return;
        }
        _editingItem = nullptr;
        {
            TypeUseScope scope;
            _lastValues.clear();
        }
        _updating = true;
        _tree->clear();
        _updating = false;
        _pushPins();
        emit pinsChanged();
    }

    void PinnedPropertiesWidget::_pushPins()
    {
        if (_channel)
        {
            _channel->setPins(pins());
        }
    }

    void PinnedPropertiesWidget::pollNow()
    {
        TypeUseScope scope; // the polled values may come from a plugin's properties
        if (!_channel)
        {
            return;
        }
        // Drop rows whose watched target no longer exists — the entity was destroyed
        // or the component was removed from it. unpin() is a no-op for a row already
        // gone, so a duplicate report (before setPins takes effect) is harmless.
        for (const auto& k : _channel->pollDeadPins())
        {
            unpin(k.entity, k.component, k.path);
        }
        const auto updates = _channel->pollPinValues();
        if (updates.empty())
        {
            return;
        }
        _updating = true;
        for (const auto& u : updates)
        {
            if (QTreeWidgetItem* it = _findItem(u.key))
            {
                // Never refresh the row with an editor open: overwriting the cell
                // would commit/close (and delete) the editor mid-edit — losing a
                // value being picked from a modal file/color dialog.
                if (it == _editingItem)
                {
                    continue;
                }
                if (u.missing)
                {
                    // The path doesn't resolve now (the array got shorter, the key
                    // went): no stale value, no editor, until it's back.
                    it->setData(0, PinMissingRole, true);
                    it->setText(kValueColumn, QStringLiteral("\u2014"));
                    it->setToolTip(kValueColumn, tr("Not there right now \u2014 an index past the end, or a key "
                                                    "that's gone. It shows again when it's back."));
                    _lastValues.remove(it);
                    continue;
                }
                if (it->data(0, PinMissingRole).toBool())
                {
                    it->setData(0, PinMissingRole, false);
                    it->setToolTip(kValueColumn, it->data(0, PinReadOnlyRole).toString());
                }
                if (!it->data(0, PinWholeRole).toBool() && TypeRenderer::isExpandable(u.value.get_type()))
                {
                    _markWhole(it, u.value.get_type());
                }
                // The text made on the simulation thread when it came with one: a
                // string_view in the copy points into that thread's memory.
                if (!u.text.isEmpty())
                {
                    it->setText(kValueColumn, u.text);
                }
                else
                {
                    it->setText(kValueColumn, it->data(0, PinWholeRole).toBool() ? TypeRenderer::summaryString(u.value)
                                                                                 : TypeRenderer::toDisplayString(u.value));
                }
                _lastValues.insert(it, u.value); // under pollNow's read section
            }
        }
        _updating = false;
    }

    void PinnedPropertiesWidget::_markWhole(QTreeWidgetItem* item, rttr::type type)
    {
        // Shown as a one-line summary of its content, never edited here (a field of
        // it can be pinned on its own for that).
        item->setData(0, PinWholeRole, true);
        if (item->data(0, PinReadOnlyRole).toString().isEmpty())
        {
            const bool container = TypeRenderer::isSequential(type) || TypeRenderer::isAssociative(type);
            const QString why = container ? tr("The whole array \u2014 pin one of its elements to edit it")
                                          : tr("The whole struct \u2014 pin one of its fields to edit it");
            item->setData(0, PinReadOnlyRole, why);
            item->setToolTip(kValueColumn, why);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        }
    }

    void PinnedPropertiesWidget::_commitValueEdit(int row, const rttr::variant& v)
    {
        QTreeWidgetItem* item = _tree->topLevelItem(row);
        if (!item || !_channel)
        {
            return;
        }
        // Echo the committed value immediately (guarded so it isn't mistaken for a
        // user edit), then queue it to the sim thread exactly like the property grid.
        // The next mirror poll re-confirms it (and updates LastValueRole).
        _updating = true;
        {
            TypeUseScope scope; // the display value is the property's own type (an enum)
            item->setText(kValueColumn, TypeRenderer::toDisplayString(bridge::coerce(v, pinLeafType(item, _lastValues))));
        }
        _updating = false;
        _channel->queuePinEdit(_itemKey(item), v);
    }

    void PinnedPropertiesWidget::_onContextMenu(const QPoint& pos)
    {
        QTreeWidgetItem* item = _tree->itemAt(pos);
        QMenu menu(this);
        if (item)
        {
            connect(menu.addAction(tr("Unpin")), &QAction::triggered, this, [this, item] {
                const auto k = _itemKey(item);
                unpin(k.entity, k.component, k.path);
            });
        }
        if (_tree->topLevelItemCount() > 0)
        {
            connect(menu.addAction(tr("Unpin all")), &QAction::triggered, this, &PinnedPropertiesWidget::clearPins);
        }
        if (!menu.isEmpty())
        {
            menu.exec(_tree->viewport()->mapToGlobal(pos));
        }
    }

} // namespace rpe
