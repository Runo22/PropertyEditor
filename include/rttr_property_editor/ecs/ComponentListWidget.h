#pragma once

#include <QWidget>
#include <flecs.h>
#include <rttr/type>

class QListWidget;
class QListWidgetItem;

namespace rpe {

struct ComponentInfo {
    flecs::id   id;
    rttr::type  rttrType;
    void*       ptr = nullptr;
};

// Lists the RTTR-discoverable components of the currently selected entity.
// Auto-discovery: matches Flecs component name to rttr::type::get_by_name().
class ComponentListWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ComponentListWidget(QWidget* parent = nullptr);

    // Call when entity changes. Rebuilds the component list.
    void setEntity(flecs::world* world, flecs::entity e);
    void clearEntity();

signals:
    void componentSelected(ComponentInfo info);
    void componentDeselected();

private slots:
    void _onSelectionChanged();

private:
    void _setupUi();

    QListWidget*            _listWidget = nullptr;
    QVector<ComponentInfo>  _components;
};

} // namespace rpe
