#pragma once

#include "ComponentListWidget.h"

#include <QWidget>
#include <flecs.h>
#include <rttr/type>
#include <rttr/variant>

class QSplitter;
class QTimer;

namespace rpe {

class EntityListWidget;
class ComponentListWidget;
class PropertyEditor;

// Three-panel browser: entity list → component list → property editor.
// Uses auto-discovery (no manual component registration required).
class EntityComponentBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit EntityComponentBrowser(QWidget* parent = nullptr);

    void setWorld(flecs::world* world);

    // Live update rate for the property editor (default 50 Hz = 20ms).
    void setLiveUpdateIntervalMs(int ms);

    PropertyEditor*    propertyEditor() const { return _propertyEditor; }
    EntityListWidget*  entityList()     const { return _entityList; }
    ComponentListWidget* componentList()const { return _componentList; }

signals:
    void propertyEdited(const QString& path, const rttr::variant& newValue);

private slots:
    void _onEntitySelected(flecs::entity e);
    void _onEntityDeselected();
    void _onComponentSelected(ComponentInfo info);
    void _onComponentDeselected();
    void _onLiveUpdate();

private:
    void _setupUi();

    flecs::world*        _world         = nullptr;
    EntityListWidget*    _entityList    = nullptr;
    ComponentListWidget* _componentList = nullptr;
    PropertyEditor*      _propertyEditor= nullptr;
    QTimer*              _liveTimer     = nullptr;
    QSplitter*           _hSplitter     = nullptr;
    QSplitter*           _vSplitter     = nullptr;

    flecs::entity        _selectedEntity;
    ComponentInfo        _selectedComponent;
};

} // namespace rpe
