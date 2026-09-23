#include "rpe/ecs/PinnedPropertiesWidget.h"

#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/ecs/EcsMirror.h"
#include "rpe/gui/PropertyModel.h" // Q_DECLARE_METATYPE(rttr::variant)
#include "rpe/gui/VariantEditorFactory.h"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
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
            LastValueRole,                  // rttr::variant (last mirrored value)
            RawIdRole,                      // qulonglong (pair id; 0 for plain components)
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
        rttr::type pinLeafType(const QTreeWidgetItem* it)
        {
            const rttr::type dt = pinDeclaredType(it);
            if (dt.is_valid() && dt != rttr::type::get<void>())
            {
                return dt;
            }
            if (it)
            {
                const rttr::variant last = it->data(0, LastValueRole).value<rttr::variant>();
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

            PinnedValueDelegate(QTreeWidget* tree, Commit commit, Arm arm, QObject* parent)
                : QStyledItemDelegate(parent)
                , _tree(tree)
                , _commit(std::move(commit))
                , _arm(std::move(arm))
            {
            }

            QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
            {
                // Drive the editor from the leaf's declared type (resolvable straight
                // after pinning) and fall back to the mirrored value's type. No hint
                // metadata in the pin list → default numeric ranges.
                const rttr::type t = pinLeafType(_tree->topLevelItem(index.row()));
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
                }
                return editor;
            }

            void setEditorData(QWidget* editor, const QModelIndex& index) const override
            {
                // Seed from the live value if one has arrived; otherwise the editor
                // opens at its type default (empty line edit, 0, first enum, …).
                varedit::setEditorData(editor, _valueAt(index));
            }

            void setModelData(QWidget* editor, QAbstractItemModel*, const QModelIndex& index) const override
            {
                const rttr::type t = pinLeafType(_tree->topLevelItem(index.row()));
                const rttr::variant v = varedit::readEditorData(editor, t);
                if (v.is_valid())
                {
                    _commit(index.row(), v);
                }
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
                return it ? it->data(0, LastValueRole).value<rttr::variant>() : rttr::variant();
            }

            QTreeWidget* _tree;
            Commit _commit;
            Arm _arm;
        };
    } // namespace

    PinnedPropertiesWidget::PinnedPropertiesWidget(QWidget* parent)
        : QWidget(parent)
    {
        _setupUi();
        _timer = new QTimer(this);
        _timer->setInterval(33); // ~30 Hz, same cadence as the browser's mirror poll
        connect(_timer, &QTimer::timeout, this, &PinnedPropertiesWidget::pollNow);
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
        const int cut = component.lastIndexOf(QLatin1Char('.'));
        const QString compLeaf = cut >= 0 ? component.mid(cut + 1) : component;

        _updating = true;
        auto* item = new QTreeWidgetItem(_tree);
        item->setText(0, entityLabel);
        item->setText(1, QStringLiteral("%1.%2").arg(compLeaf, path));
        const QString full = QStringLiteral("#%1  %2.%3").arg(entity).arg(component, path);
        item->setToolTip(0, full);
        item->setToolTip(1, full);
        item->setData(0, EntityRole, entity);
        item->setData(0, ComponentRole, component);
        item->setData(0, PathRole, path);
        item->setData(0, RawIdRole, rawId);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
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
                it->setText(kValueColumn, TypeRenderer::toDisplayString(u.value));
                it->setData(0, LastValueRole, QVariant::fromValue(u.value));
            }
        }
        _updating = false;
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
        item->setText(kValueColumn, TypeRenderer::toDisplayString(v));
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
