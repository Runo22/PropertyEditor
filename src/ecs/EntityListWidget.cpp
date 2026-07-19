#include "rpe/ecs/EntityListWidget.h"

#include "rpe/core/TypeBridge.h"

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

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

        _list = new QListWidget(this);
        layout->addWidget(_list, 1);

        connect(_list, &QListWidget::currentItemChanged, this, &EntityListWidget::_onSelectionChanged);
        connect(_filterEdit, &QLineEdit::textChanged, this, &EntityListWidget::_refresh);
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

    void EntityListWidget::setRequiredComponent(const QString& componentName, bool enabled)
    {
        // The filter is configured purely through state now (no UI checkbox) — the
        // host drives it via the browser's Settings. Direct mode applies it in
        // _refresh(); mirror mode applies it on the producer.
        _requiredComponent = componentName;
        _requiredEnabled = enabled;
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

        const bool filterByComp = !_requiredComponent.isEmpty() && _requiredEnabled;

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

    bool EntityListWidget::selectById(qulonglong id)
    {
        _requestedId = id;
        for (int i = 0; i < _list->count(); ++i)
        {
            if (_list->item(i)->data(Qt::UserRole).toULongLong() == id)
            {
                _requestedId = 0;
                _list->setCurrentRow(i); // emits _onSelectionChanged → selection signals
                return true;
            }
        }
        return false; // not present yet — applied by _applyEntries when it appears
    }

    QString EntityListWidget::currentLabel() const
    {
        const auto* cur = _list->currentItem();
        return cur ? cur->text() : QString();
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

    void EntityListWidget::_applyEntries(const QVector<QPair<qulonglong, QString>>& entriesIn)
    {
        // Show entities sorted alphabetically by label (case-insensitive), id as a
        // stable tiebreaker for same-named entities.
        QVector<QPair<qulonglong, QString>> entries = entriesIn;
        const auto less = [](const QPair<qulonglong, QString>& a, const QPair<qulonglong, QString>& b) {
            const int c = a.second.compare(b.second, Qt::CaseInsensitive);
            return c != 0 ? c < 0 : a.first < b.first;
        };
        std::sort(entries.begin(), entries.end(), less);

        // Skip entirely when the visible set is unchanged — avoids flicker.
        if (entries == _lastEntries)
        {
            return;
        }

        // Remember current selection so it survives the update.
        qulonglong selectedId = 0;
        if (auto* cur = _list->currentItem())
        {
            selectedId = cur->data(Qt::UserRole).toULongLong();
        }

        // DIFF update instead of clear+rebuild: both lists are sorted by the same
        // comparator, so a single merge pass finds exactly the added/removed rows.
        // A dynamic world then costs a handful of row operations per publish, not
        // thousands — a full rebuild of a big list (item churn on the GUI thread,
        // serialized process-wide by the Windows debug heap) was a periodic stall.
        _list->blockSignals(true);
        _list->setUpdatesEnabled(false);
        int row = 0; // widget row cursor (= kept + inserted so far)
        int i = 0;   // index into _lastEntries (mirrors the widget's current rows)
        int j = 0;   // index into the new entries
        while (i < _lastEntries.size() || j < entries.size())
        {
            const bool haveOld = i < _lastEntries.size();
            const bool haveNew = j < entries.size();
            if (haveOld && haveNew && _lastEntries[i] == entries[j])
            {
                ++i;
                ++j;
                ++row; // unchanged row — item (and any selection on it) untouched
            }
            else if (haveOld && (!haveNew || less(_lastEntries[i], entries[j])))
            {
                delete _list->takeItem(row); // removed entity
                ++i;
            }
            else
            {
                auto* item = new QListWidgetItem(entries[j].second);
                item->setData(Qt::UserRole, entries[j].first);
                _list->insertItem(row, item); // added entity
                ++j;
                ++row;
            }
        }
        _lastEntries = entries;
        _list->setUpdatesEnabled(true);
        _list->blockSignals(false);

        // Selection: a pending selectById() request wins; else the user's selection
        // (if its row survived); else auto-select the top row so the panel is never
        // blank. Signals are live here, so listeners hear about real changes only
        // (setCurrentItem on the already-current item does not re-emit).
        QListWidgetItem* reselect = nullptr;
        QListWidgetItem* requested = nullptr;
        for (int r = 0; r < _list->count(); ++r)
        {
            auto* it = _list->item(r);
            const qulonglong id = it->data(Qt::UserRole).toULongLong();
            if (id == selectedId)
            {
                reselect = it;
            }
            if (_requestedId != 0 && id == _requestedId)
            {
                requested = it;
            }
        }
        if (requested)
        {
            _requestedId = 0;
            _list->setCurrentItem(requested);
        }
        else if (reselect)
        {
            _list->setCurrentItem(reselect);
        }
        else if (_list->count() > 0)
        {
            _list->setCurrentRow(0);
        }
        else if (selectedId != 0)
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
