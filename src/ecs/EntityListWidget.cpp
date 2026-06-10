#include "rpe/ecs/EntityListWidget.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace rpe {

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
    _requiredCheck->setVisible(false);     // shown only when a required component is set
    layout->addWidget(_requiredCheck);

    _list = new QListWidget(this);
    layout->addWidget(_list, 1);

    connect(_list, &QListWidget::currentItemChanged,
            this,  &EntityListWidget::_onSelectionChanged);
    connect(_filterEdit, &QLineEdit::textChanged, this, &EntityListWidget::_refresh);
    connect(_requiredCheck, &QCheckBox::toggled, this, &EntityListWidget::_refresh);
}

void EntityListWidget::setWorld(flecs::world* world)
{
    _world = world;
    if (_world) _timer->start();
    else        _timer->stop();
    _refresh();
}

void EntityListWidget::setRefreshIntervalMs(int ms) { _timer->setInterval(ms); }

void EntityListWidget::setWorldAccess(AccessGuard guard) { _guard = std::move(guard); }

void EntityListWidget::setRequiredComponent(const QString& componentName, bool enabledByDefault)
{
    _requiredComponent = componentName;
    if (componentName.isEmpty()) {
        _requiredCheck->setVisible(false);
    } else {
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
    if (!_world) { _list->clear(); _lastEntries.clear(); return; }

    const QString filter = _filterEdit->text().trimmed().toLower();
    const bool filterByComp = !_requiredComponent.isEmpty() && _requiredCheck->isChecked();

    // Query all *named* entities (those carrying the (Identifier, Name) pair),
    // optionally constrained to those having the required component. All world
    // reads happen under the guard; the widget rebuild below does not need it.
    QVector<QPair<qulonglong, QString>> entries;
    withGuard(_guard, [&] {
        auto qb = _world->query_builder().with<flecs::Identifier>(flecs::Name);
        flecs::entity requiredComp;
        if (filterByComp) {
            requiredComp = _world->lookup(_requiredComponent.toUtf8().constData());
            if (requiredComp.is_valid())
                qb.with(requiredComp);
        }
        flecs::query<> q = qb.build();

        q.each([&](flecs::entity e) {
            if (!e.is_alive()) return;
            const char* rawName = e.name();
            const QString name  = rawName ? QString::fromUtf8(rawName) : QStringLiteral("(unnamed)");
            const QString label = QStringLiteral("%1  %2").arg(e.id()).arg(name);
            if (!filter.isEmpty() && !label.toLower().contains(filter))
                return;
            entries.append({ static_cast<qulonglong>(e.id()), label });
        });
    });

    // Timer-driven refresh: skip the widget rebuild entirely when the visible
    // set hasn't changed — avoids selection flicker and wasted repaints.
    if (entries == _lastEntries)
        return;
    _lastEntries = entries;

    // Remember current selection so it survives the rebuild.
    flecs::entity_t selectedId = 0;
    if (auto* cur = _list->currentItem())
        selectedId = static_cast<flecs::entity_t>(cur->data(Qt::UserRole).toULongLong());

    _list->blockSignals(true);
    _list->clear();

    bool reselected = false;
    for (const auto& entry : entries) {   // .first/.second: Qt 5.12 has no QPair bindings
        auto* item = new QListWidgetItem(entry.second, _list);
        item->setData(Qt::UserRole, entry.first);
        if (entry.first == selectedId) { _list->setCurrentItem(item); reselected = true; }
    }

    _list->blockSignals(false);

    if (!reselected && selectedId != 0)
        emit entityDeselected();
}

void EntityListWidget::_onSelectionChanged()
{
    auto* item = _list->currentItem();
    if (!item) { emit entityDeselected(); return; }
    const auto id = static_cast<flecs::entity_t>(item->data(Qt::UserRole).toULongLong());
    if (_world) {
        flecs::entity e;
        bool alive = false;
        withGuard(_guard, [&] {
            e     = _world->entity(id);
            alive = e.is_alive();
        });
        if (alive)
            emit entitySelected(e);
    }
}

} // namespace rpe
