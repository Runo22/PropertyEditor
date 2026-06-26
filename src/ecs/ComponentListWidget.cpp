#include "rpe/ecs/ComponentListWidget.h"

#include "rpe/core/TypeBridge.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace rpe
{

    namespace
    {
        // Draws a right-aligned "×" remove affordance on each component row and
        // reports clicks on it via a callback. No Q_OBJECT/signals (kept to a plain
        // delegate) — the owning widget supplies the callback. The × is only drawn
        // and hit-tested when `enabled` is true.
        class RemoveButtonDelegate : public QStyledItemDelegate
        {
        public:
            using QStyledItemDelegate::QStyledItemDelegate;

            bool enabled = false;
            std::function<void(const QModelIndex&)> onRemove;

            static QRect glyphRect(const QStyleOptionViewItem& opt)
            {
                const int s = opt.rect.height();
                return QRect(opt.rect.right() - s, opt.rect.top(), s, s);
            }

            void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override
            {
                // Reserve room on the right so the text doesn't run under the ×.
                QStyleOptionViewItem o(opt);
                if (enabled)
                {
                    o.rect.adjust(0, 0, -o.rect.height(), 0);
                }
                QStyledItemDelegate::paint(p, o, index);
                if (!enabled)
                {
                    return;
                }
                const QRect g = glyphRect(opt);
                const bool hover = opt.state & QStyle::State_MouseOver;
                p->save();
                QColor c = opt.palette.color(QPalette::Text);
                c.setAlpha(hover ? 230 : 120);
                p->setPen(c);
                const int m = g.height() / 3;
                const QRect x = g.adjusted(m, m, -m, -m);
                p->drawLine(x.topLeft(), x.bottomRight());
                p->drawLine(x.topRight(), x.bottomLeft());
                p->restore();
            }

            bool editorEvent(QEvent* ev, QAbstractItemModel*, const QStyleOptionViewItem& opt, const QModelIndex& index) override
            {
                if (!enabled || ev->type() != QEvent::MouseButtonRelease)
                {
                    return false;
                }
                auto* me = static_cast<QMouseEvent*>(ev);
                if (me->button() == Qt::LeftButton && glyphRect(opt).contains(me->pos()))
                {
                    if (onRemove)
                    {
                        onRemove(index);
                    }
                    return true; // consume — don't also change selection
                }
                return false;
            }
        };
    } // namespace

    ComponentListWidget::ComponentListWidget(QWidget* parent)
        : QWidget(parent)
    {
        _setupUi();
    }

    void ComponentListWidget::_setupUi()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        // Header row: "Components" + a right-aligned "+" add button.
        auto* headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        auto* header = new QLabel(tr("Components"), this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
        headerRow->addWidget(header, 1);

        _addBtn = new QToolButton(this);
        _addBtn->setText(QStringLiteral("+"));
        _addBtn->setToolTip(tr("Add a component to the selected entity"));
        _addBtn->setAutoRaise(true);
        _addBtn->setVisible(false); // shown only when editing is enabled
        headerRow->addWidget(_addBtn, 0);
        layout->addLayout(headerRow);

        _list = new QListWidget(this);
        _list->setMouseTracking(true); // so the × can highlight on hover
        auto* del = new RemoveButtonDelegate(_list);
        del->onRemove = [this](const QModelIndex& idx) {
            const QString name = idx.data(Qt::DisplayRole).toString();
            if (!name.isEmpty())
            {
                emit removeComponentRequested(name);
            }
        };
        _list->setItemDelegate(del);
        _rowDelegate = del;
        layout->addWidget(_list, 1);

        connect(_list, &QListWidget::currentItemChanged, this, &ComponentListWidget::_onSelectionChanged);
        connect(_addBtn, &QToolButton::clicked, this, &ComponentListWidget::_onAddClicked);
    }

    void ComponentListWidget::setComponentEditingEnabled(bool on)
    {
        _editingEnabled = on;
        _addBtn->setVisible(on);
        // _rowDelegate is always a RemoveButtonDelegate (set in _setupUi).
        static_cast<RemoveButtonDelegate*>(_rowDelegate)->enabled = on;
        _list->viewport()->update();
    }

    void ComponentListWidget::setAddableComponents(const QStringList& names)
    {
        _addable = names;
    }

    void ComponentListWidget::_onAddClicked()
    {
        QMenu menu(this);
        if (_addable.isEmpty())
        {
            menu.addAction(tr("(no components available)"))->setEnabled(false);
        }
        else
        {
            for (const QString& n : _addable)
            {
                connect(menu.addAction(n), &QAction::triggered, this, [this, n] { emit addComponentRequested(n); });
            }
        }
        menu.exec(_addBtn->mapToGlobal(_addBtn->rect().bottomLeft()));
    }

    void ComponentListWidget::setEntity(flecs::world* world, flecs::entity e)
    {
        _components.clear();
        _mirrorNames.clear();

        // Collect under the guard (entity/component reads touch the world);
        // populate the widget afterwards without holding it.
        QStringList names;
        withGuard(_guard, [&] {
            if (!world || !e.is_alive())
            {
                return;
            }
            e.each([&](flecs::id id) {
                if (!id.is_entity())
                {
                    return;
                }
                flecs::entity comp = id.entity();
                const char* raw = comp.name();
                if (!raw || raw[0] == '\0')
                {
                    return;
                }

                // Only components we can actually inspect/edit (a TypeBridge wrapper
                // turns the raw component pointer into an RTTR instance). resolveByName
                // bridges flecs' short component name to the registered RTTR type, which
                // may be scoped or registered under a different name.
                const rttr::type t = TypeBridge::resolveByName(raw);
                if (!t.is_valid())
                {
                    return;
                }

                _components.append(ComponentInfo { id, t });
                names.append(QString::fromUtf8(raw));
            });
        });

        _list->blockSignals(true);
        _list->clear();
        for (int i = 0; i < names.size(); ++i)
        {
            auto* item = new QListWidgetItem(names[i], _list);
            item->setData(Qt::UserRole, i);
        }
        _list->blockSignals(false);
        if (_list->count() > 0)
        {
            _list->setCurrentRow(0); // auto-select first → drives the editor
        }
        else
        {
            emit componentDeselected();
        }
    }

    void ComponentListWidget::setWorldAccess(AccessGuard guard)
    {
        _guard = std::move(guard);
    }

    void ComponentListWidget::setComponentNames(const QStringList& names)
    {
        if (names == _mirrorNames)
        {
            return; // unchanged → keep selection, no flicker
        }
        _mirrorNames = names;
        _components.clear(); // mirror mode has no world-backed infos

        const QString prevSel = _list->currentItem() ? _list->currentItem()->text() : QString();

        _list->blockSignals(true);
        _list->clear();
        QListWidgetItem* reselect = nullptr;
        for (const QString& n : names)
        {
            auto* item = new QListWidgetItem(n, _list);
            if (n == prevSel)
            {
                reselect = item;
            }
        }
        _list->blockSignals(false);

        if (reselect)
        {
            _list->setCurrentItem(reselect);
        }
        else if (_list->count())
        {
            _list->setCurrentRow(0);
        }
        else
        {
            emit componentDeselected();
        }
    }

    QString ComponentListWidget::currentComponentName() const
    {
        auto* item = _list->currentItem();
        return item ? item->text() : QString();
    }

    void ComponentListWidget::clearEntity()
    {
        _components.clear();
        _mirrorNames.clear();
        _list->clear();
        emit componentDeselected();
    }

    void ComponentListWidget::_onSelectionChanged()
    {
        auto* item = _list->currentItem();
        if (!item)
        {
            emit componentDeselected();
            return;
        }
        emit componentNameSelected(item->text()); // world-free; mirror mode
        const int idx = item->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < _components.size())
        {
            emit componentSelected(_components[idx]);
        }
    }

} // namespace rpe
