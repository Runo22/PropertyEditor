#include "rpe/ecs/EntityListWidget.h"

#include "rpe/core/TypeBridge.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace rpe
{

    namespace
    {
        // Short (unscoped) form of a name: the segment after the last "::".
        QString shortName(const QString& s)
        {
            const int pos = s.lastIndexOf(QStringLiteral("::"));
            return pos >= 0 ? s.mid(pos + 2) : s;
        }

        // Display label for an entity: its name, else its prefab's name + id, else
        // just the id. Matches EcsMirror so direct and mirror modes look identical.
        QString entityLabel(const flecs::entity& e)
        {
            const char* n = e.name();
            if (n && n[0] != '\0')
            {
                return QString::fromUtf8(n);
            }
            const flecs::entity prefab = e.target(flecs::IsA);
            if (prefab.is_valid())
            {
                const char* pn = prefab.name();
                if (pn && pn[0] != '\0')
                {
                    return QStringLiteral("%1  #%2").arg(QString::fromUtf8(pn)).arg(e.id());
                }
            }
            return QStringLiteral("#%1").arg(e.id());
        }
    } // namespace

    EntityListWidget::EntityListWidget(QWidget* parent)
        : QWidget(parent)
    {
        _setupUi();
        _timer = new QTimer(this);
        _timer->setInterval(500);
        connect(_timer, &QTimer::timeout, this, &EntityListWidget::_refresh);
    }

    void EntityListWidget::_setupUi()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        auto* header = new QLabel(tr("Entities"), this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
        layout->addWidget(header);

        _filterEdit = new QLineEdit(this);
        _filterEdit->setPlaceholderText(tr("Filter entities…"));
        _filterEdit->setClearButtonEnabled(true);
        layout->addWidget(_filterEdit);

        _requiredCheck = new QCheckBox(this);
        _requiredCheck->setVisible(false); // shown only when a required component is set
        layout->addWidget(_requiredCheck);

        _list = new QListWidget(this);
        layout->addWidget(_list, 1);

        connect(_list, &QListWidget::currentItemChanged, this, &EntityListWidget::_onSelectionChanged);
        connect(_filterEdit, &QLineEdit::textChanged, this, &EntityListWidget::_refresh);
        connect(_requiredCheck, &QCheckBox::toggled, this, &EntityListWidget::_refresh);
    }

    void EntityListWidget::setWorld(flecs::world* world)
    {
        _world = world;
        if (_world)
        {
            _timer->start();
        }
        else
        {
            _timer->stop();
        }
        _refresh();
    }

    void EntityListWidget::setRefreshIntervalMs(int ms)
    {
        _timer->setInterval(ms);
    }

    void EntityListWidget::setWorldAccess(AccessGuard guard)
    {
        _guard = std::move(guard);
    }

    void EntityListWidget::stopAutoRefresh()
    {
        _timer->stop();
    }

    void EntityListWidget::setRequiredComponent(const QString& componentName, bool enabledByDefault)
    {
        _requiredComponent = componentName;
        if (componentName.isEmpty())
        {
            _requiredCheck->setVisible(false);
        }
        else
        {
            _requiredCheck->setText(tr("Only entities with %1").arg(componentName));
            // Block the toggled→_refresh signal so we refresh exactly once below.
            QSignalBlocker block(_requiredCheck);
            _requiredCheck->setChecked(enabledByDefault);
            _requiredCheck->setVisible(true);
        }
        _refresh();
    }

    void EntityListWidget::_refresh()
    {
        if (!_world)
        {
            // Mirror mode: entries arrive via setEntries(). A filter-text or
            // required-toggle change must NOT wipe the list — just re-apply the text
            // filter over the entries we already have (clearing the box restores
            // them all). The required-component filter is applied upstream by the
            // mirror, so there is nothing to re-query here.
            _applyTextFilter();
            return;
        }

        const bool filterByComp = !_requiredComponent.isEmpty() && _requiredCheck->isChecked();

        // Show every entity carrying at least one bridged component (the ones the
        // inspector can actually display), optionally constrained to those having
        // the required component. Labelled by name / prefab name / id — to match
        // EcsMirror exactly. All world reads happen under the guard; the widget
        // rebuild below does not need it.
        const QString reqShort = filterByComp ? shortName(_requiredComponent) : QString();
        QVector<QPair<qulonglong, QString>> entries;
        withGuard(_guard, [&] {
            flecs::query<> q = _world->query_builder().with(flecs::Any).build();

            q.each([&](flecs::entity e) {
                if (!e.is_alive())
                {
                    return;
                }
                bool hasBridged = false;
                bool hasReq = reqShort.isEmpty();
                e.each([&](flecs::id id) {
                    if (!id.is_entity())
                    {
                        return;
                    }
                    const char* cn = id.entity().name();
                    if (!cn || cn[0] == '\0')
                    {
                        return;
                    }
                    if (TypeBridge::resolveByName(cn).is_valid())
                    {
                        hasBridged = true;
                    }
                    if (!reqShort.isEmpty() && shortName(QString::fromUtf8(cn)) == reqShort)
                    {
                        hasReq = true;
                    }
                });
                if (!hasBridged || !hasReq)
                {
                    return;
                }
                entries.append({ static_cast<qulonglong>(e.id()), entityLabel(e) });
            });
        });

        // Keep the full set; the text filter is applied on top so clearing it
        // restores every entity without re-querying.
        _sourceEntries = entries;
        _applyTextFilter();
    }

    void EntityListWidget::setEntries(const QVector<QPair<qulonglong, QString>>& entries)
    {
        // External feed (mirror mode): remember the full set, then apply the text
        // filter. Editing/clearing the filter re-derives from _sourceEntries.
        _sourceEntries = entries;
        _applyTextFilter();
    }

    void EntityListWidget::_applyTextFilter()
    {
        const QString filter = _filterEdit->text().trimmed().toLower();
        if (filter.isEmpty())
        {
            _applyEntries(_sourceEntries);
            return;
        }
        QVector<QPair<qulonglong, QString>> filtered;
        for (const auto& e : _sourceEntries)
        {
            if (e.second.toLower().contains(filter))
            {
                filtered.append(e);
            }
        }
        _applyEntries(filtered);
    }

    void EntityListWidget::_applyEntries(const QVector<QPair<qulonglong, QString>>& entries)
    {
        // Skip the rebuild when the visible set is unchanged — avoids flicker.
        if (entries == _lastEntries)
        {
            return;
        }
        _lastEntries = entries;

        // Remember current selection so it survives the rebuild.
        qulonglong selectedId = 0;
        if (auto* cur = _list->currentItem())
        {
            selectedId = cur->data(Qt::UserRole).toULongLong();
        }

        _list->blockSignals(true);
        _list->clear();

        bool reselected = false;
        for (const auto& entry : entries)
        { // .first/.second: Qt 5.12 has no QPair bindings
            auto* item = new QListWidgetItem(entry.second, _list);
            item->setData(Qt::UserRole, entry.first);
            if (entry.first == selectedId)
            {
                _list->setCurrentItem(item);
                reselected = true;
            }
        }

        _list->blockSignals(false);

        if (!reselected && selectedId != 0)
        {
            emit entityDeselected();
        }
    }

    void EntityListWidget::_onSelectionChanged()
    {
        auto* item = _list->currentItem();
        if (!item)
        {
            emit entityDeselected();
            return;
        }
        const auto id = item->data(Qt::UserRole).toULongLong();
        emit entityIdSelected(id); // world-free; used by mirror mode
        if (_world)
        {
            flecs::entity e;
            bool alive = false;
            withGuard(_guard, [&] {
                e = _world->entity(static_cast<flecs::entity_t>(id));
                alive = e.is_alive();
            });
            if (alive)
            {
                emit entitySelected(e);
            }
        }
    }

} // namespace rpe
