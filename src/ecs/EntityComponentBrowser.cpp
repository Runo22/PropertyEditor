#include "rpe/ecs/EntityComponentBrowser.h"

#include "rpe/ecs/EcsMirror.h"
#include "rpe/ecs/EntityListWidget.h"
#include "rpe/gui/PropertyEditor.h"

#include <QCheckBox>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <rttr/instance.h>

namespace rpe
{

    EntityComponentBrowser::EntityComponentBrowser(QWidget* parent)
        : QWidget(parent)
    {
        _setupUi();
        _liveTimer = new QTimer(this);
        _liveTimer->setInterval(20); // 50 Hz default
        connect(_liveTimer, &QTimer::timeout, this, &EntityComponentBrowser::_onLiveUpdate);

        _mirrorTimer = new QTimer(this);
        _mirrorTimer->setInterval(33); // ~30 Hz GUI poll of the mirror
        connect(_mirrorTimer, &QTimer::timeout, this, &EntityComponentBrowser::_onMirrorPoll);
    }

    void EntityComponentBrowser::_setupUi()
    {
        _mainLayout = new QVBoxLayout(this);
        _mainLayout->setContentsMargins(0, 0, 0, 0);
        _mainLayout->setSpacing(2);

        // Create the panels once, parented to `this`. _applyLayout() re-parents
        // them into the chosen splitter tree (and can swap it later).
        _writeCheck = new QCheckBox(tr("Write edits back to world"), this);
        _writeCheck->setToolTip(tr(
            "On: edits modify the live component.\n"
            "Off: edits are pinned as overrides only."));
        _mainLayout->addWidget(_writeCheck);

        _entityList = new EntityListWidget(this);
        _componentList = new ComponentListWidget(this);
        _propertyEditor = new PropertyEditor(this);

        _applyLayout(_browserLayout);

        // The editor needs the *current* component pointer at read/write time.
        // We relink a persistent wrapper so the returned instance stays valid for
        // the duration of the synchronous get/set.
        _propertyEditor->setInstanceProvider([this]() -> rttr::instance {
            void* p = _liveComponentPtr();
            if (!p)
            {
                return rttr::instance();
            }
            _liveWrapper.relink(p);
            return _liveWrapper.instance();
        });

        connect(_entityList, &EntityListWidget::entitySelected, this, &EntityComponentBrowser::_onEntitySelected);
        connect(_entityList, &EntityListWidget::entityDeselected, this, &EntityComponentBrowser::_onEntityDeselected);
        connect(_entityList, &EntityListWidget::entityIdSelected, this, &EntityComponentBrowser::_onEntityIdSelected);
        connect(_componentList, &ComponentListWidget::componentSelected, this, &EntityComponentBrowser::_onComponentSelected);
        connect(_componentList, &ComponentListWidget::componentNameSelected, this, &EntityComponentBrowser::_onComponentNameSelected);
        connect(_componentList, &ComponentListWidget::componentDeselected, this, &EntityComponentBrowser::_onComponentDeselected);
        connect(_propertyEditor, &PropertyEditor::propertyEdited, this, &EntityComponentBrowser::propertyEdited);
        connect(_writeCheck, &QCheckBox::toggled, this, &EntityComponentBrowser::_onWriteToggled);
    }

    void EntityComponentBrowser::_applyLayout(Layout layout)
    {
        _browserLayout = layout;

        // Build the new splitter tree. addWidget() re-parents the panels out of
        // the previous tree, so deleting it afterwards won't take them with it.
        QWidget* root = nullptr;
        if (layout == Layout::Vertical)
        {
            auto* v = new QSplitter(Qt::Vertical);
            v->addWidget(_entityList);
            v->addWidget(_componentList);
            v->addWidget(_propertyEditor);
            v->setStretchFactor(0, 2);
            v->setStretchFactor(1, 2);
            v->setStretchFactor(2, 5);
            v->setChildrenCollapsible(false);
            root = v;
        }
        else
        {
            auto* h = new QSplitter(Qt::Horizontal);
            h->addWidget(_entityList);
            auto* vRight = new QSplitter(Qt::Vertical);
            vRight->addWidget(_componentList);
            vRight->addWidget(_propertyEditor);
            vRight->setStretchFactor(0, 1);
            vRight->setStretchFactor(1, 3);
            vRight->setChildrenCollapsible(false);
            h->addWidget(vRight);
            h->setStretchFactor(0, 1);
            h->setStretchFactor(1, 2);
            h->setChildrenCollapsible(false);
            root = h;
        }

        if (_layoutRoot)
        {
            _mainLayout->removeWidget(_layoutRoot);
            _layoutRoot->deleteLater(); // panels already re-parented into `root`
        }
        _mainLayout->addWidget(root, 1);
        _layoutRoot = root;
    }

    void EntityComponentBrowser::setBrowserLayout(Layout layout)
    {
        if (layout != _browserLayout || !_layoutRoot)
        {
            _applyLayout(layout);
        }
    }

    void EntityComponentBrowser::setWorld(flecs::world* world)
    {
        _world = world;
        _entityList->setWorld(world);
        _liveTimer->stop();
        _componentList->clearEntity();
        _propertyEditor->unbind();
    }

    void EntityComponentBrowser::setLiveUpdateIntervalMs(int ms)
    {
        _liveTimer->setInterval(ms);
    }

    void EntityComponentBrowser::setWorldAccess(AccessGuard guard)
    {
        _guard = guard;
        _entityList->setWorldAccess(guard);
        _componentList->setWorldAccess(guard);
        // WriteBack edits run provider + write under the same guard. The provider
        // (_liveComponentPtr) is therefore NOT guarded itself — guards don't nest.
        _propertyEditor->setWriteGuard(std::move(guard));
    }

    void EntityComponentBrowser::setEntityComponentFilter(const QString& name, bool enabled)
    {
        _entityList->setRequiredComponent(name, enabled);
    }

    void EntityComponentBrowser::setEditPolicy(EditPolicy p)
    {
        _propertyEditor->setEditPolicy(p);
        _writeCheck->setChecked(p == EditPolicy::WriteBack);
    }

    void EntityComponentBrowser::_onWriteToggled(bool on)
    {
        _propertyEditor->setEditPolicy(on ? EditPolicy::WriteBack : EditPolicy::Override);
    }

    // NOTE: touches the world; callers must already hold the world guard (the
    // model's write path holds it when invoking the instance provider, and the
    // browser's own call sites wrap it explicitly). Not guarded here so that
    // guards never nest — a plain mutex stays sufficient.
    void* EntityComponentBrowser::_liveComponentPtr() const
    {
        if (!_world || !_selectedEntity.is_alive() || !_selectedComponent.rttrType.is_valid())
        {
            return nullptr;
        }
        return _selectedEntity.get_mut(_selectedComponent.id.raw_id());
    }

    void EntityComponentBrowser::_onEntitySelected(flecs::entity e)
    {
        _selectedEntity = e;
        _liveTimer->stop();
        _componentList->setEntity(_world, e); // auto-selects first component
        emit entitySelected(e);
    }

    void EntityComponentBrowser::_onEntityDeselected()
    {
        _liveTimer->stop();
        _selectedEntity = {};
        _componentList->clearEntity();
        _propertyEditor->unbind();
        emit entityDeselected();
    }

    void EntityComponentBrowser::_onComponentSelected(ComponentInfo info)
    {
        _selectedComponent = info;
        if (!info.rttrType.is_valid())
        {
            return;
        }

        _propertyEditor->bindType(info.rttrType);
        withGuard(_guard, [&] {
            if (void* p = _liveComponentPtr())
            {
                _liveWrapper = RttrVariantWrapper::makeLinked(info.rttrType, p);
                _propertyEditor->refresh(_liveWrapper.instance());
            }
        });
        _liveTimer->start();
        emit componentSelected(info);
    }

    void EntityComponentBrowser::_onComponentDeselected()
    {
        _liveTimer->stop();
        _selectedComponent = {};
        _propertyEditor->unbind();
        emit componentDeselected();
    }

    void EntityComponentBrowser::_onLiveUpdate()
    {
        // Pointer lookup AND the value read (refresh) stay inside one guarded
        // section: the component could move or be removed between the two
        // otherwise. Painting later reads only the model's cached values.
        withGuard(_guard, [&] {
            if (void* p = _liveComponentPtr())
            {
                _liveWrapper.relink(p);
                _propertyEditor->refresh(_liveWrapper.instance());
            }
        });
    }

    // ── mirror mode ──────────────────────────────────────────────────────────────

    void EntityComponentBrowser::setMirror(EcsMirror* mirror)
    {
        // Hold the shared channel, not the EcsMirror itself: the mirror may be
        // destroyed on the sim thread before this widget; the channel survives via
        // this shared_ptr so polling stays valid.
        _channel = mirror ? mirror->channel() : nullptr;
        _world = nullptr; // mirror mode never touches the world directly
        _liveTimer->stop();

        // Editor edits are queued to the sim thread; values flow back via the channel.
        if (_channel)
        {
            _propertyEditor->setEditPolicy(EditPolicy::Override);
            auto ch = _channel; // capture the shared_ptr (not 'this' indirection)
            _propertyEditor->setEditSink([ch](const QString& path, const rttr::variant& v) {
                ch->queueEdit(path, v);
            });
            _entityList->stopAutoRefresh();
            _mirrorTimer->start();
        }
        else
        {
            _propertyEditor->setEditSink({});
            _mirrorTimer->stop();
        }
    }

    void EntityComponentBrowser::_pushInterest()
    {
        if (!_channel)
        {
            return;
        }
        const QStringList paths = _mirrorComponent.isEmpty()
            ? QStringList()
            : _propertyEditor->visibleLeafPaths(_openFieldsOnly);
        _channel->setInterest(_mirrorEntity, _mirrorComponent, paths);
    }

    void EntityComponentBrowser::_onMirrorPoll()
    {
        if (!_channel)
        {
            return;
        }

        QVector<MirrorChannel::EntityEntry> ents;
        if (_channel->pollEntities(ents))
        {
            QVector<QPair<qulonglong, QString>> rows;
            rows.reserve(ents.size());
            for (const auto& e : ents)
            {
                rows.append({ e.id, e.label });
            }
            _entityList->setEntries(rows);
        }

        QStringList comps;
        if (_channel->pollComponents(comps))
        {
            _componentList->setComponentNames(comps);
        }

        for (auto& u : _channel->pollValues())
        {
            _propertyEditor->setPropertyValue(u.path, u.value);
        }

        // Re-send interest each tick so newly expanded fields start mirroring.
        _pushInterest();
    }

    void EntityComponentBrowser::_onEntityIdSelected(qulonglong id)
    {
        if (!_channel)
        {
            return; // direct mode uses _onEntitySelected
        }
        _mirrorEntity = id;
        _mirrorComponent.clear();
        _propertyEditor->unbind();
        _pushInterest();
    }

    void EntityComponentBrowser::_onComponentNameSelected(const QString& name)
    {
        if (!_channel)
        {
            return; // direct mode uses _onComponentSelected
        }
        _mirrorComponent = name;
        const rttr::type t = rttr::type::get_by_name(name.toStdString());
        if (t.is_valid())
        {
            _propertyEditor->bindType(t); // schema only — no instance/world touch
            _propertyEditor->expandAll();
        }
        _pushInterest();
    }

} // namespace rpe
