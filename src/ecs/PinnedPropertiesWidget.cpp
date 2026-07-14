#include "rpe/ecs/PinnedPropertiesWidget.h"

#include "rpe/core/TypeRenderer.h"
#include "rpe/ecs/EcsMirror.h"
#include "rpe/gui/PropertyModel.h" // Q_DECLARE_METATYPE(rttr::variant)

#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace rpe
{

    namespace
    {
        enum ItemRole
        {
            EntityRole = Qt::UserRole,      // qulonglong
            ComponentRole,                  // QString (full scoped name)
            PathRole,                       // QString (dot-path)
            LastValueRole,                  // rttr::variant (last mirrored value)
        };

        constexpr int kValueColumn = 2; // Entity | Property | Value

        // U+2026 HORIZONTAL ELLIPSIS via an explicit escape: robust against the
        // compiler misreading the source encoding (a raw non-ASCII literal turns
        // into mojibake on MSVC without /utf-8).
        QString placeholderText()
        {
            return QStringLiteral("\u2026");
        }

        // Parse an edited cell back into a variant matching the last known value's
        // type. The bridge's coerce() finishes the job on the sim thread (string →
        // path, exact integer widths, …); this only picks the broad category.
        rttr::variant textToVariant(const QString& text, const rttr::variant& last)
        {
            const rttr::type t = last.is_valid() ? TypeRenderer::rawType(last.get_type()) : rttr::type::get<void>();
            if (t == rttr::type::get<bool>())
            {
                const QString s = text.trimmed().toLower();
                return rttr::variant(s == QLatin1String("true") || s == QLatin1String("1"));
            }
            if (t.is_arithmetic())
            {
                bool ok = false;
                if (t == rttr::type::get<float>() || t == rttr::type::get<double>())
                {
                    const double v = text.toDouble(&ok);
                    return ok ? rttr::variant(v) : rttr::variant();
                }
                if (t == rttr::type::get<unsigned char>() || t == rttr::type::get<unsigned short>()
                    || t == rttr::type::get<unsigned int>() || t == rttr::type::get<unsigned long>()
                    || t == rttr::type::get<unsigned long long>())
                {
                    const qulonglong v = text.toULongLong(&ok);
                    return ok ? rttr::variant(static_cast<uint64_t>(v)) : rttr::variant();
                }
                const qlonglong v = text.toLongLong(&ok);
                return ok ? rttr::variant(static_cast<int64_t>(v)) : rttr::variant();
            }
            return rttr::variant(text); // strings/paths — coerce() converts
        }
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
        layout->addWidget(_tree, 1);

        connect(_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
            // Value edits are anchored to the LAST MIRRORED value's type; until one
            // has arrived there is nothing to parse against, so don't open an editor.
            if (column == kValueColumn && item->data(0, LastValueRole).value<rttr::variant>().is_valid())
            {
                _tree->editItem(item, kValueColumn);
            }
        });
        connect(_tree, &QTreeWidget::itemChanged, this, &PinnedPropertiesWidget::_onItemChanged);
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
        return _findItem({ entity, component, path }) != nullptr;
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
                 item->data(0, PathRole).toString() };
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

    void PinnedPropertiesWidget::pin(qulonglong entity, const QString& entityLabel, const QString& component, const QString& path)
    {
        if (entity == 0 || component.isEmpty() || path.isEmpty() || isPinned(entity, component, path))
        {
            return;
        }
        // Display the leaf component name; the full scoped name stays in the data.
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
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setText(kValueColumn, placeholderText()); // until the first value lands
        _updating = false;

        _pushPins();
        emit pinsChanged();
    }

    void PinnedPropertiesWidget::unpin(qulonglong entity, const QString& component, const QString& path)
    {
        if (QTreeWidgetItem* it = _findItem({ entity, component, path }))
        {
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
                // Skip the refresh while the user is editing this very cell, so the
                // live echo doesn't stomp their typing. An open inline editor is a
                // focused child of the tree (the tree itself has focus otherwise).
                QWidget* fw = QApplication::focusWidget();
                if (fw && fw != _tree && _tree->isAncestorOf(fw) && it == _tree->currentItem())
                {
                    continue;
                }
                it->setText(kValueColumn, TypeRenderer::toDisplayString(u.value));
                it->setData(0, LastValueRole, QVariant::fromValue(u.value));
            }
        }
        _updating = false;
    }

    void PinnedPropertiesWidget::_onItemChanged(QTreeWidgetItem* item, int column)
    {
        if (_updating || column != kValueColumn || !_channel)
        {
            return;
        }
        const rttr::variant last = item->data(0, LastValueRole).value<rttr::variant>();
        if (!last.is_valid())
        {
            // No mirrored value yet — nothing anchors the parse (a "42" would go out
            // as a string and be dropped by coerce for arithmetic targets). Restore
            // the placeholder instead of sending an edit that silently fails.
            _updating = true;
            item->setText(kValueColumn, placeholderText());
            _updating = false;
            return;
        }
        const rttr::variant v = textToVariant(item->text(kValueColumn), last);
        if (!v.is_valid())
        {
            // Unparseable input: restore the last known display instead of sending garbage.
            _updating = true;
            item->setText(kValueColumn, TypeRenderer::toDisplayString(last));
            _updating = false;
            return;
        }
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
