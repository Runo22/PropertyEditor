#pragma once

#include <QWidget>
#include <flecs.h>

class QListWidget;
class QLineEdit;
class QTimer;

namespace rpe {

// Lists all named entities in a flecs::world.
// Refreshes on a slow timer (entity list rarely changes at high frequency).
class EntityListWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EntityListWidget(QWidget* parent = nullptr);

    void setWorld(flecs::world* world);
    void setRefreshIntervalMs(int ms);

signals:
    void entitySelected(flecs::entity e);
    void entityDeselected();

private slots:
    void _refresh();
    void _onSelectionChanged();
    void _onFilterChanged(const QString& text);

private:
    void _setupUi();

    flecs::world*  _world          = nullptr;
    QListWidget*   _listWidget     = nullptr;
    QLineEdit*     _filterEdit     = nullptr;
    QTimer*        _refreshTimer   = nullptr;
};

} // namespace rpe
