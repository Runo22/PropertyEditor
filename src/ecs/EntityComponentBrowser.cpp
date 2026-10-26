#include "rpe/ecs/EntityComponentBrowser.h"

#include "rpe/core/PropertySearch.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRelease.h"
#include "rpe/ecs/ComponentScan.h"
#include "rpe/ecs/EcsMirror.h"
#include "rpe/ecs/EntityListWidget.h"
#include "rpe/ecs/PinnedPropertiesWidget.h"
#include "rpe/gui/PropertyEditor.h"

#include <QCheckBox>
#include <QMenu>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

#include <rttr/instance.h>

namespace rpe
{

    EntityComponentBrowser::EntityComponentBrowser(QWidget* parent)
        : QWidget(parent)
    {
        // Allow a flecs::entity to travel through a queued connection into
        // selectEntity(flecs::entity). Idempotent — safe to call more than once.
        qRegisterMetaType<flecs::entity>("flecs::entity");
        _setupUi();
        _liveTimer = new QTimer(this);
        _liveTimer->setInterval(20); // 50 Hz default
        connect(_liveTimer, &QTimer::timeout, this, &EntityComponentBrowser::_onLiveUpdate);

        // A plugin unloading: drop what we hold of its types now, rebind by name
        // once they are back (see rpe/core/TypeRelease.h).
        _releaseHandle = addTypeReleaseHandler(
            [this](const std::vector<rttr::type>& released) { _onTypesReleased(released); });
        connect(_propertyEditor, &PropertyEditor::boundTypeReleased, this, [this] { _onBoundTypeReleased(); });
        _rebindTimer = new QTimer(this);
        _rebindTimer->setInterval(100);
        connect(_rebindTimer, &QTimer::timeout, this, &EntityComponentBrowser::_tryRebindReleased);

        _mirrorTimer = new QTimer(this);
        _mirrorTimer->setInterval(33); // ~30 Hz GUI poll of the mirror
        connect(_mirrorTimer, &QTimer::timeout, this, &EntityComponentBrowser::_onMirrorPoll);

        // Search all components (scope button + list counts).
        _propertyEditor->setSearchAllAvailable(true);
        connect(_propertyEditor, &PropertyEditor::filterTextChanged, this, &EntityComponentBrowser::_onSearchChanged);
        connect(_propertyEditor, &PropertyEditor::searchAllToggled, this, &EntityComponentBrowser::_onSearchChanged);
        connect(_propertyEditor, &PropertyEditor::currentPathChanged, this, &EntityComponentBrowser::_onGroupRowPicked);
        _searchTimer = new QTimer(this);
        _searchTimer->setInterval(250); // same cadence as the mirror's rounds
        connect(_searchTimer, &QTimer::timeout, this, &EntityComponentBrowser::_runDirectSearch);
        // Direct mode, grouped view: a WriteBack edit lands in the row's own component.
        _propertyEditor->setGroupInstanceProvider([this](const QString& key) -> rttr::instance {
            const ComponentInfo ci = _componentList->componentInfo(key);
            if (!_world || !_selectedEntity.is_alive() || !ci.rttrType.is_valid())
            {
                return rttr::instance();
            }
            void* p = _selectedEntity.get_mut(ci.id.raw_id());
            if (!p)
            {
                return rttr::instance();
            }
            _groupWrapper = RttrVariantWrapper::makeLinked(ci.rttrType, p);
            return _groupWrapper.instance();
        });
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
            "Off: edits stay as local drafts only."));
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
        connect(_entityList, &EntityListWidget::spawnPrefabRequested, this, &EntityComponentBrowser::_onSpawnPrefab);
        connect(_entityList, &EntityListWidget::removeEntityRequested, this, &EntityComponentBrowser::_onRemoveEntity);
        connect(_componentList, &ComponentListWidget::componentSelected, this, &EntityComponentBrowser::_onComponentSelected);
        connect(_componentList, &ComponentListWidget::componentNameSelected, this, &EntityComponentBrowser::_onComponentNameSelected);
        connect(_componentList, &ComponentListWidget::componentDeselected, this, &EntityComponentBrowser::_onComponentDeselected);
        connect(_componentList, &ComponentListWidget::addComponentRequested, this, &EntityComponentBrowser::_onAddComponent);
        connect(_componentList, &ComponentListWidget::removeComponentRequested, this, &EntityComponentBrowser::_onRemoveComponent);
        connect(_componentList, &ComponentListWidget::removeComponentIdRequested, this, &EntityComponentBrowser::_onRemoveComponentId);
        // An Add menu is about to open: have the producer rescan what it offers. The
        // catalog is otherwise rescanned only on a detectable change or a slow
        // safety interval, which can miss a renamed / swapped-in component.
        const auto refreshCatalog = [this] {
            if (_channel)
            {
                _channel->requestCatalogRefresh();
            }
        };
        connect(_componentList, &ComponentListWidget::addPickerWanted, this, refreshCatalog);
        connect(_entityList, &EntityListWidget::addPickerWanted, this, refreshCatalog);
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

        // Re-parenting into a fresh splitter tree rebuilds the focus chain in
        // whatever order the widgets landed, so restate it: Tab walks the browser
        // the way it reads, top-level control first, then the three panels in the
        // order you work through them.
        setTabOrder(_writeCheck, _entityList);
        setTabOrder(_entityList, _componentList);
        setTabOrder(_componentList, _propertyEditor);
    }

    void EntityComponentBrowser::setBrowserLayout(Layout layout)
    {
        _settings.layout = layout;
        if (layout != _browserLayout || !_layoutRoot)
        {
            _applyLayout(layout);
        }
    }

    void EntityComponentBrowser::setWorld(flecs::world* world)
    {
        // Leave mirror mode if it was active (the two modes are exclusive).
        if (_channel)
        {
            _channel.reset();
            _mirrorTimer->stop();
            _propertyEditor->setEditSink({});
            _writeCheck->setVisible(true);
        }

        // Clear the panels BEFORE handing the world to the list: the list auto-selects
        // its top entity, which fills the component panel through _onEntitySelected.
        // Clearing afterwards left that entity highlighted with an empty panel — and
        // clicking it again changed nothing (it was already current).
        _world = world;
        _liveTimer->stop();
        _componentList->clearEntity();
        _propertyEditor->unbind();
        _entityList->setWorld(world);
        _applyEntityFilter(); // re-apply the configured filter to the new mode
    }

    void EntityComponentBrowser::setLiveUpdateIntervalMs(int ms)
    {
        _liveTimer->setInterval(ms);
        _settings.liveUpdateIntervalMs = ms;
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

    void EntityComponentBrowser::_applyEntityFilter()
    {
        const QString name = _settings.requiredComponent;
        const bool enabled = _settings.requiredComponentEnabled && !name.isEmpty();
        _entityList->setRequiredComponent(name, enabled);
        // In mirror mode the producer does the filtering — push the required
        // component (or clear it when disabled) so the entity list actually narrows.
        if (_channel)
        {
            _channel->setRequiredComponent(enabled ? name : QString());
            _channel->requestResync();
        }
    }

    void EntityComponentBrowser::setEditPolicy(EditPolicy p)
    {
        _settings.editPolicy = p;
        _propertyEditor->setEditPolicy(p);
        _writeCheck->setChecked(p == EditPolicy::WriteBack);
    }

    void EntityComponentBrowser::setComponentEditingEnabled(bool on)
    {
        _settings.allowComponentEditing = on;
        _componentList->setComponentEditingEnabled(on);
    }

    void EntityComponentBrowser::setEntityAddingEnabled(bool on)
    {
        _entityAddingEnabled = on;
        _entityList->setEntityAddingEnabled(on);
    }

    void EntityComponentBrowser::setPrefabGroups(const QVector<PrefabGroup>& groups)
    {
        // Remember them: the tag names only reach the producer through a channel,
        // and the host may well configure the browser before handing it the mirror.
        // Dropping them then would leave the picker silently ungrouped.
        _prefabGroups = groups;
        _applyPrefabGroups();
    }

    void EntityComponentBrowser::_applyPrefabGroups()
    {
        QStringList tags;
        QHash<QString, QIcon> icons;
        tags.reserve(_prefabGroups.size());
        for (const PrefabGroup& g : _prefabGroups)
        {
            tags.append(g.tag);
            if (!g.icon.isNull())
            {
                icons.insert(g.tag, g.icon);
            }
        }
        _entityList->setPrefabGroupIcons(icons); // GUI-side rendering
        if (_channel)
        {
            _channel->setPrefabGroupTags(tags); // producer grouping + filtering
            _channel->requestResync();
        }
    }

    void EntityComponentBrowser::setEntityRemovingEnabled(bool on)
    {
        _entityList->setEntityRemovingEnabled(on);
    }

    qulonglong EntityComponentBrowser::_menuEntityId() const
    {
        return _mirrorEntity != 0 ? _mirrorEntity
                                  : (_selectedEntity.is_alive() ? static_cast<qulonglong>(_selectedEntity.id()) : 0);
    }

    int EntityComponentBrowser::addEntityAction(const EntityAction& action)
    {
        const int handle = _nextMenuHandle++;
        _entityActions.append({ handle, action });
        _pushEntityActions();
        return handle;
    }

    void EntityComponentBrowser::removeEntityAction(int handle)
    {
        const auto n = _entityActions.size();
        _entityActions.erase(std::remove_if(_entityActions.begin(), _entityActions.end(),
                                            [handle](const auto& e) { return e.first == handle; }),
                             _entityActions.end());
        if (_entityActions.size() != n)
            _pushEntityActions();
    }

    void EntityComponentBrowser::_pushEntityActions()
    {
        QVector<EntityAction> list;
        list.reserve(_entityActions.size());
        for (const auto& e : _entityActions)
            list.append(e.second);
        _entityList->setContextActions(list);
    }

    int EntityComponentBrowser::addComponentAction(const ComponentAction& action)
    {
        const int handle = _nextMenuHandle++;
        _componentActions.append({ handle, action });
        _pushComponentActions();
        return handle;
    }

    void EntityComponentBrowser::removeComponentAction(int handle)
    {
        const auto n = _componentActions.size();
        _componentActions.erase(std::remove_if(_componentActions.begin(), _componentActions.end(),
                                               [handle](const auto& e) { return e.first == handle; }),
                                _componentActions.end());
        if (_componentActions.size() != n)
            _pushComponentActions();
    }

    void EntityComponentBrowser::_pushComponentActions()
    {
        // Wrap each host action so the widget-level callback (key, rawId) forwards the
        // CURRENTLY selected entity id, read at click time.
        QVector<ComponentListWidget::MenuAction> wrapped;
        wrapped.reserve(_componentActions.size());
        for (const auto& e : _componentActions)
        {
            const auto cb = e.second.callback;
            wrapped.append({ e.second.label, e.second.icon,
                             [this, cb](const QString& key, qulonglong rawId) {
                                 if (cb)
                                     cb(_menuEntityId(), key, rawId);
                             },
                             e.second.menu });
        }
        _componentList->setContextActions(wrapped);
    }

    int EntityComponentBrowser::addEntityMenuHook(EntityMenuHook hook)
    {
        const int handle = _nextMenuHandle++;
        _entityMenuHooks.append({ handle, std::move(hook) });
        _installMenuHooks();
        return handle;
    }

    int EntityComponentBrowser::addComponentMenuHook(ComponentMenuHook hook)
    {
        const int handle = _nextMenuHandle++;
        _componentMenuHooks.append({ handle, std::move(hook) });
        _installMenuHooks();
        return handle;
    }

    void EntityComponentBrowser::removeMenuHook(int handle)
    {
        auto byHandle = [handle](const auto& e) { return e.first == handle; };
        _entityMenuHooks.erase(std::remove_if(_entityMenuHooks.begin(), _entityMenuHooks.end(), byHandle),
                               _entityMenuHooks.end());
        _componentMenuHooks.erase(std::remove_if(_componentMenuHooks.begin(), _componentMenuHooks.end(), byHandle),
                                  _componentMenuHooks.end());
    }

    void EntityComponentBrowser::setEntityMenuHook(EntityMenuHook hook)
    {
        _entityMenuHook = std::move(hook);
        _installMenuHooks();
    }

    void EntityComponentBrowser::setComponentMenuHook(ComponentMenuHook hook)
    {
        _componentMenuHook = std::move(hook);
        _installMenuHooks();
    }

    void EntityComponentBrowser::_installMenuHooks()
    {
        // One widget-level hook per list that runs every registered hook. Each run
        // iterates a COPY, so a hook may add or remove hooks while the menu builds.
        _entityList->setContextMenuHook([this](qulonglong id, QMenu& menu) {
            const EntityMenuHook primary = _entityMenuHook;
            const auto hooks = _entityMenuHooks;
            if (primary)
                primary(id, menu);
            for (const auto& h : hooks)
                if (h.second)
                    h.second(id, menu);
        });
        _componentList->setContextMenuHook([this](const QString& key, qulonglong rawId, QMenu& menu) {
            const qulonglong id = _menuEntityId();
            const ComponentMenuHook primary = _componentMenuHook;
            const auto hooks = _componentMenuHooks;
            if (primary)
                primary(id, key, rawId, menu);
            for (const auto& h : hooks)
                if (h.second)
                    h.second(id, key, rawId, menu);
        });
    }

    void EntityComponentBrowser::_onRemoveEntity(qulonglong entityId)
    {
        if (entityId == 0)
        {
            return;
        }
        if (_channel)
        {
            _channel->queueDestroyEntity(entityId);
            _channel->requestResync();
        }
        else if (_world)
        {
            withGuard(_guard, [&] {
                flecs::entity e = _world->entity(static_cast<flecs::entity_t>(entityId));
                if (e.is_alive())
                {
                    e.destruct();
                }
            });
        }
    }

    void EntityComponentBrowser::_onSpawnPrefab(qulonglong prefabId)
    {
        if (prefabId == 0)
        {
            return;
        }
        if (_channel)
        {
            // Mirror mode: the sim thread instantiates via is_a and runs the host's
            // configurator (EcsMirror::setSpawnConfigurator).
            _channel->queueSpawnPrefab(prefabId);
            _channel->requestResync();
        }
        else if (_world)
        {
            // Direct mode: the GUI owns the world — spawn under the guard, and give
            // the instance a real name from the prefab (minus " Prefab"), uniquified
            // since flecs aborts on duplicate names. Mirror mode does the same on the
            // sim thread (EcsMirror).
            withGuard(_guard, [&] {
                flecs::entity prefab = _world->entity(static_cast<flecs::entity_t>(prefabId));
                flecs::entity ne = _world->entity().is_a(static_cast<flecs::entity_t>(prefabId));
                const char* pn = prefab.name();
                QString base = pn ? QString::fromUtf8(pn) : QString();
                base = base.trimmed();
                if (base.size() > 7 && base.right(7).compare(QStringLiteral(" prefab"), Qt::CaseInsensitive) == 0)
                {
                    base.chop(7);
                    base = base.trimmed();
                }
                if (!base.isEmpty())
                {
                    std::string name = base.toStdString();
                    for (int i = 1; _world->lookup(name.c_str()).is_valid(); ++i)
                    {
                        name = base.toStdString() + " (" + std::to_string(i) + ")";
                    }
                    ne.set_name(name.c_str());
                }
            });
        }
    }

    void EntityComponentBrowser::setSettings(const Settings& s)
    {
        _settings = s;
        setBrowserLayout(s.layout);
        setSnapshotOpenFieldsOnly(s.snapshotOpenFieldsOnly);
        setLiveUpdateIntervalMs(s.liveUpdateIntervalMs);
        if (_mirrorTimer)
        {
            _mirrorTimer->setInterval(s.mirrorPollIntervalMs);
        }
        setEditPolicy(s.editPolicy);
        setComponentEditingEnabled(s.allowComponentEditing);
        _applyEntityFilter();
        _updateAddable(); // hiddenAddNamespaces may have changed
    }

    void EntityComponentBrowser::setHiddenAddNamespaces(const QStringList& namespaces)
    {
        _settings.hiddenAddNamespaces = namespaces;
        _updateAddable();
    }

    void EntityComponentBrowser::_onWriteToggled(bool on)
    {
        _propertyEditor->setEditPolicy(on ? EditPolicy::WriteBack : EditPolicy::LocalEdit);
    }

    // NOTE: touches the world; callers must already hold the world guard (the
    // model's write path holds it when invoking the instance provider, and the
    // browser's own call sites wrap it explicitly). Not guarded here so that
    // guards never nest — a plain mutex stays sufficient.
    EntityComponentBrowser::~EntityComponentBrowser()
    {
        removeTypeReleaseHandler(_releaseHandle);
        TypeUseScope scope;
        _liveWrapper = RttrVariantWrapper();
        _groupWrapper = RttrVariantWrapper();
    }

    void EntityComponentBrowser::_onTypesReleased(const std::vector<rttr::type>& released)
    {
        // Any thread, while no other thread is in a TypeUseScope; the released
        // module is still loaded. Only plain data here — no signals, no widgets.
        if (isReleased(_selectedComponent.rttrType, released))
        {
            // Invalid (not void): _liveComponentPtr then reads nothing — the
            // component's flecs id is about to die with the plugin.
            _selectedComponent.rttrType = rttr::type::get_by_name(std::string());
        }
        if (_liveWrapper.isValid() && isReleased(_liveWrapper.type(), released))
        {
            _liveWrapper = RttrVariantWrapper(); // a variant made by the plugin's code
        }
        if (_groupWrapper.isValid() && isReleased(_groupWrapper.type(), released))
        {
            _groupWrapper = RttrVariantWrapper();
        }
    }

    void EntityComponentBrowser::_onBoundTypeReleased()
    {
        _rebindPending = true;
        _rebindGen = ~quint64(0); // try now
        _tryRebindReleased();
        if (_rebindPending && !_channel)
        {
            _rebindTimer->start(); // mirror mode retries on every poll instead
        }
    }

    void EntityComponentBrowser::_tryRebindReleased()
    {
        if (!_rebindPending)
        {
            _rebindTimer->stop();
            return;
        }
        const quint64 gen = TypeBridge::registryGeneration();
        if (gen == _rebindGen)
        {
            return; // nothing registered since the last try
        }
        _rebindGen = gen;
        if (_groupView)
        {
            _rebuildGroups(); // the grouped view rebuilds from the list, by name
            _rebindPending = false;
            _rebindTimer->stop();
            return;
        }
        if (_channel)
        {
            if (_mirrorEntity == 0 || _mirrorComponent.isEmpty())
            {
                _rebindPending = false;
                return;
            }
            const QString typeName = _pairTypeName.value(_mirrorComponent, _mirrorComponent);
            if (TypeBridge::resolveByName(typeName.toUtf8().constData()).is_valid())
            {
                _onComponentNameSelected(_mirrorComponent); // clears _rebindPending
            }
            return;
        }
        // Direct mode: rebuild the entity's component list; it reselects the same
        // component by name, which binds it again.
        if (!_world || !_selectedEntity.is_alive())
        {
            _rebindPending = false;
            _rebindTimer->stop();
            return;
        }
        _componentList->setEntity(_world, _selectedEntity);
    }

    // ── search all components ────────────────────────────────────────────────────────

    void EntityComponentBrowser::_onSearchChanged()
    {
        _searchText = _propertyEditor->filterText();
        _updateSearchTarget();
        const bool hasEntity = _channel ? _mirrorEntity != 0 : _selectedEntity.is_alive();
        const bool wantGroups = _propertyEditor->isSearchAll() && !_searchText.isEmpty() && hasEntity;
        if (wantGroups && !_groupView)
        {
            _enterGroupView();
        }
        else if (!wantGroups && _groupView)
        {
            _leaveGroupView();
        }
    }

    void EntityComponentBrowser::_updateSearchTarget()
    {
        // Counts from another entity / another text are stale: clear until the next
        // round reports.
        _componentList->setMatchCounts({}, !_searchText.isEmpty());
        if (_channel)
        {
            _channel->setSearch(_mirrorEntity, _searchText);
        }
        else if (!_searchText.isEmpty() && _selectedEntity.is_alive())
        {
            _runDirectSearch();
            _searchTimer->start();
        }
        else
        {
            _searchTimer->stop();
        }
        if (_searchText.isEmpty())
        {
            _componentList->setMatchCounts({}, false);
        }
        if (_groupView)
        {
            _rebuildGroups();
        }
    }

    void EntityComponentBrowser::_runDirectSearch()
    {
        if (_channel || !_world || !_selectedEntity.is_alive() || _searchText.isEmpty())
        {
            _searchTimer->stop();
            return;
        }
        std::vector<MirrorChannel::SearchHit> hits;
        const auto comps = _componentList->dataComponents();
        withGuard(_guard, [&] {
            TypeUseScope scope; // inside the guard: never wait for a release holding it
            std::vector<PropertyHit> found;
            for (const auto& kc : comps)
            {
                const ComponentInfo ci = _componentList->componentInfo(kc.first);
                if (!ci.rttrType.is_valid())
                {
                    continue;
                }
                void* p = _selectedEntity.get_mut(ci.id.raw_id());
                const rttr::variant access = p ? TypeBridge::wrap(ci.rttrType, p) : rttr::variant();
                if (!access.is_valid())
                {
                    continue;
                }
                found.clear();
                searchProperties(rttr::instance(access), ci.rttrType, _searchText, found);
                for (PropertyHit& h : found)
                {
                    hits.push_back({ kc.first, std::move(h.path), std::move(h.value) });
                }
            }
        });
        TypeUseScope scope;
        _applySearchResults(hits);
    }

    void EntityComponentBrowser::_applySearchResults(const std::vector<MirrorChannel::SearchHit>& hits)
    {
        QHash<QString, int> counts;
        for (const auto& h : hits)
        {
            ++counts[h.component];
        }
        _componentList->setMatchCounts(counts, true);
        if (!_groupView)
        {
            return;
        }
        QSet<QString> now;
        for (const auto& h : hits)
        {
            if (_groupKeys.contains(h.component))
            {
                const QString gp = PropertyModel::groupPath(h.component, h.path);
                _propertyEditor->setPropertyValue(gp, h.value);
                now.insert(gp);
            }
        }
        // A row that matched last round but not this one: clear it, so it drops out
        // of the filtered view instead of lingering with a stale value.
        for (const QString& gp : std::as_const(_hitPaths))
        {
            if (!now.contains(gp))
            {
                _propertyEditor->setPropertyValue(gp, rttr::variant());
            }
        }
        _hitPaths = now;
    }

    void EntityComponentBrowser::_enterGroupView()
    {
        _groupView = true;
        _groupKeys.clear(); // force the build
        _groupFocusPath.clear();
        _rebuildGroups();
        _pushInterest();
        _refreshPinnedTint();
    }

    void EntityComponentBrowser::_leaveGroupView()
    {
        _groupView = false;
        _groupKeys.clear();
        _hitPaths.clear();
        const QString focus = _groupFocusPath;
        // Back to the single component — the one picked last in the grouped view —
        // with the filter still applied and the picked row selected.
        if (_channel)
        {
            if (!_mirrorComponent.isEmpty())
            {
                _onComponentNameSelected(_mirrorComponent);
            }
            else
            {
                _propertyEditor->unbind();
            }
        }
        else if (_selectedComponent.rttrType.is_valid())
        {
            _onComponentSelected(_selectedComponent);
        }
        else
        {
            _propertyEditor->unbind();
        }
        if (!focus.isEmpty())
        {
            _propertyEditor->revealPath(focus);
        }
        _refreshPinnedTint();
    }

    void EntityComponentBrowser::_rebuildGroups()
    {
        if (!_groupView)
        {
            return;
        }
        QVector<PropertyModel::Group> groups;
        QStringList keys;
        for (const auto& kc : _componentList->dataComponents())
        {
            rttr::type t = rttr::type::get_by_name(std::string());
            if (_channel)
            {
                t = TypeBridge::resolveByName(_pairTypeName.value(kc.first, kc.first).toUtf8().constData());
            }
            else
            {
                t = _componentList->componentInfo(kc.first).rttrType;
            }
            if (!t.is_valid())
            {
                continue;
            }
            groups.append({ kc.first, kc.second, t });
            keys.append(kc.first);
        }
        if (keys == _groupKeys && _propertyEditor->model()->isGrouped())
        {
            return; // same components — keep the tree (and what's selected in it)
        }
        _groupKeys = keys;
        _hitPaths.clear();
        _propertyEditor->bindGroups(groups);
        const QString current = _channel ? _mirrorComponent : _componentList->currentComponentKey();
        if (!current.isEmpty())
        {
            _propertyEditor->revealGroup(current);
        }
        // Fill the new tree now rather than at the next round.
        if (_channel)
        {
            _channel->requestSearchRefresh();
        }
        else
        {
            _runDirectSearch();
        }
    }

    void EntityComponentBrowser::_onGroupRowPicked(const QString& path)
    {
        QString key, rel;
        if (!_groupView || !PropertyModel::splitGroupPath(path, &key, &rel) || key.isEmpty())
        {
            return;
        }
        _groupFocusPath = rel;
        // Keep the list (and what the browser considers selected) on the row's
        // component, without leaving the grouped view.
        _componentList->setCurrentKeySilently(key);
        if (_channel)
        {
            if (key != _mirrorComponent)
            {
                _mirrorComponent = key;
                _pushInterest();
            }
        }
        else
        {
            const ComponentInfo ci = _componentList->componentInfo(key);
            if (ci.rttrType.is_valid())
            {
                _selectedComponent = ci;
            }
        }
    }

    void* EntityComponentBrowser::_liveComponentPtr() const
    {
        if (!_world || !_selectedEntity.is_alive() || !_selectedComponent.rttrType.is_valid())
        {
            return nullptr;
        }
        return _selectedEntity.get_mut(_selectedComponent.id.raw_id());
    }

    bool EntityComponentBrowser::selectEntity(flecs::entity e)
    {
        return selectEntity(static_cast<qulonglong>(e.id()));
    }

    bool EntityComponentBrowser::selectEntity(qulonglong id)
    {
        // Drive the list exactly as a user click would; the list's selection signal
        // flows back through _onEntitySelected / _onEntityIdSelected to update the
        // panels and re-emit the browser's selection signals.
        return _entityList->selectById(id);
    }

    void EntityComponentBrowser::_onEntitySelected(flecs::entity e)
    {
        _selectedEntity = e;
        _liveTimer->stop();
        _componentList->setEntity(_world, e); // auto-selects first component
        _onSearchChanged(); // search the new entity (and keep the grouped view on it)
        emit entitySelected(e);
        emit entityIdSelected(static_cast<qulonglong>(e.id()));
    }

    void EntityComponentBrowser::_onEntityDeselected()
    {
        _liveTimer->stop();
        _selectedEntity = {};
        // Forget the mirror-mode selection too, otherwise _pushInterest keeps feeding
        // the now-gone entity and the next poll would refill the component panel with
        // its (stale) components — e.g. an entity that a required-component filter has
        // just excluded.
        _mirrorEntity = 0;
        _mirrorComponent.clear();
        _currentComps.clear();
        _currentTags.clear();
        _pairTypeName.clear();
        _pairRawId.clear();
        _componentList->clearEntity();
        _propertyEditor->unbind();
        // No entity, no grouped view (it comes back with the next selection).
        _groupView = false;
        _groupKeys.clear();
        _hitPaths.clear();
        _updateSearchTarget();
        if (_channel)
        {
            _pushInterest(); // interest for entity 0 → producer stops sending components
        }
        _refreshPinnedTint(); // the pin set is per entity+component
        emit entityDeselected();
    }

    void EntityComponentBrowser::_onComponentSelected(ComponentInfo info)
    {
        _rebindPending = false;
        _rebindTimer->stop();
        _selectedComponent = info;
        if (_groupView)
        {
            _propertyEditor->revealGroup(_componentList->currentComponentKey());
            return;
        }
        if (!info.rttrType.is_valid())
        {
            return;
        }

        _propertyEditor->bindType(info.rttrType);
        withGuard(_guard, [&] {
            TypeUseScope scope; // inside the guard: never wait for a release holding it
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
        if (_groupView)
        {
            // The grouped view isn't about the list's selection; a component that
            // left the entity just leaves the view.
            _selectedComponent = {};
            _rebuildGroups();
            return;
        }
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
            TypeUseScope scope; // inside the guard: never wait for a release holding it
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
        // This touches QTimers / widgets, which is only valid on the widget's GUI
        // thread. The mirror is typically created on the simulation thread, so
        // callers often invoke this from there — marshal to the GUI thread.
        // mirror->channel() (the only thing read off `mirror`) is thread-safe.
        if (QThread::currentThread() != this->thread())
        {
            QMetaObject::invokeMethod(
                this, [this, mirror] { setMirror(mirror); }, Qt::QueuedConnection);
            return;
        }

        // Hold the shared channel, not the EcsMirror itself: the mirror may be
        // destroyed on the sim thread before this widget; the channel survives via
        // this shared_ptr so polling stays valid.
        _channel = mirror ? mirror->channel() : nullptr;
        _world = nullptr; // mirror mode never touches the world directly
        _liveTimer->stop();

        // Editor edits are queued to the sim thread; values flow back via the channel.
        if (_channel)
        {
            _propertyEditor->setEditPolicy(EditPolicy::LocalEdit);
            // Address the edit to the entity + component it was made on, NOW: an
            // inline editor commits on focus-out, i.e. inside the very click that
            // selects another entity — by the time the sim applies it, the
            // selection has moved and the same leaf path exists there too.
            auto ch = _channel; // the shared_ptr (the channel outlives the mirror)
            _propertyEditor->setEditSink([this, ch](const QString& path, const rttr::variant& v) {
                QString key, rel;
                if (PropertyModel::splitGroupPath(path, &key, &rel))
                {
                    // A row of the grouped view: its own component, at its own path —
                    // and a search round right away, so the change echoes promptly.
                    ch->queueEdit(_mirrorEntity, key, rel, v);
                    ch->requestSearchRefresh();
                    return;
                }
                ch->queueEdit(_mirrorEntity, _mirrorComponent, path, v);
            });
            // Mirror mode is fed via setEntries(); make sure the list never queries a
            // (possibly stale) world, so a filter change re-filters the feed instead.
            _entityList->setWorld(nullptr);
            _entityList->stopAutoRefresh();
            // The "write back to world" toggle is meaningless in mirror mode (edits
            // always go through the channel's edit queue), so hide it.
            _writeCheck->setVisible(false);
            _mirrorTimer->start();
            _applyEntityFilter();  // push the configured filter to the producer
            _applyPrefabGroups();  // …and the prefab group tags, configured or not yet
        }
        else
        {
            _propertyEditor->setEditSink({});
            _writeCheck->setVisible(true);
            _mirrorTimer->stop();
        }

        // Hand the channel to an attached watch list (pins flow producer-side).
        if (_pinWidget)
        {
            _pinWidget->setChannel(_channel);
        }
        _propertyEditor->setPinningEnabled(_channel && _pinWidget);
        _refreshPinnedTint();
    }

    void EntityComponentBrowser::setPinnedPropertiesWidget(PinnedPropertiesWidget* w)
    {
        if (_pinWidget == w)
        {
            return;
        }
        if (_pinWidget)
        {
            _pinWidget->disconnect(this);
            disconnect(_propertyEditor, &PropertyEditor::pinRequested, this, nullptr);
            disconnect(_propertyEditor, &PropertyEditor::unpinRequested, this, nullptr);
        }
        _pinWidget = w;
        if (w)
        {
            // The widget is host-owned and may be destroyed before this browser
            // (e.g. its dock page closes) — drop the raw pointer then, so selection
            // changes can't dereference a dead widget.
            connect(w, &QObject::destroyed, this, [this] {
                _pinWidget = nullptr;
                _propertyEditor->setPinningEnabled(false);
                _refreshPinnedTint();
            });
            w->setChannel(_channel);
            connect(_propertyEditor, &PropertyEditor::pinRequested, this, [this](const QString& path) {
                // Data-carrying pairs pin too: they have no resolvable flecs name, so
                // the pin carries the pair id (from the row) and the producer resolves
                // it by id + ecs_get_typeid. Plain components pass rawId 0 (by-name).
                QString comp, rel; // a grouped-view row names its own component
                if (!PropertyModel::splitGroupPath(path, &comp, &rel))
                {
                    comp = _mirrorComponent;
                }
                if (_pinWidget && _mirrorEntity != 0 && !comp.isEmpty())
                {
                    _pinWidget->pin(_mirrorEntity, _entityList->currentLabel(), comp, rel, _pairRawId.value(comp, 0));
                }
            });
            connect(_propertyEditor, &PropertyEditor::unpinRequested, this, [this](const QString& path) {
                QString comp, rel;
                if (!PropertyModel::splitGroupPath(path, &comp, &rel))
                {
                    comp = _mirrorComponent;
                }
                if (_pinWidget && _mirrorEntity != 0)
                {
                    _pinWidget->unpin(_mirrorEntity, comp, rel);
                }
            });
            connect(w, &PinnedPropertiesWidget::pinsChanged, this, &EntityComponentBrowser::_refreshPinnedTint);
        }
        _propertyEditor->setPinningEnabled(_channel && _pinWidget);
        _refreshPinnedTint();
    }

    void EntityComponentBrowser::_refreshPinnedTint()
    {
        if (_groupView && _pinWidget && _channel)
        {
            QSet<QString> all; // the grouped view addresses rows by group path
            for (const QString& key : _groupKeys)
            {
                for (const QString& p : _pinWidget->pinnedPaths(_mirrorEntity, key))
                {
                    all.insert(PropertyModel::groupPath(key, p));
                }
            }
            _propertyEditor->setPinnedPaths(all);
            return;
        }
        _propertyEditor->setPinnedPaths((_pinWidget && _channel)
                                            ? _pinWidget->pinnedPaths(_mirrorEntity, _mirrorComponent)
                                            : QSet<QString>());
    }

    void EntityComponentBrowser::_pushInterest()
    {
        if (!_channel)
        {
            return;
        }
        // The grouped view is fed by search rounds, not by watched paths.
        const QStringList paths = (_mirrorComponent.isEmpty() || _groupView)
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

        QVector<MirrorChannel::ComponentRow> compRows;
        if (_channel->pollComponentRows(compRows))
        {
            _currentComps.clear();
            _currentTags.clear();
            _pairTypeName.clear();
            _pairRawId.clear();
            // With no entity selected (e.g. a required-component filter emptied the
            // list), ignore any component feed still in flight for the old entity so
            // the panel doesn't show a non-matching entity's components. (The producer
            // clears the feed when the SELECTED entity becomes invalid — see EcsMirror's
            // gone/failsFilter block — but on a plain deselect it just stops publishing,
            // so this consumer-side gate covers the one-pump window before it does.)
            if (_mirrorEntity != 0)
            {
                _componentList->setComponentRows(compRows);
                for (const auto& r : compRows)
                {
                    if (r.kind == MirrorChannel::RowKind::Data)
                    {
                        _currentComps.append(r.name);
                    }
                    else if (r.kind == MirrorChannel::RowKind::Tag)
                    {
                        _currentTags.insert(r.name);
                    }
                    else if (r.kind == MirrorChannel::RowKind::PairData)
                    {
                        _pairTypeName.insert(r.key(), r.typeName);
                        _pairRawId.insert(r.key(), r.rawId);
                    }
                }
            }
            _updateAddable();
            if (_groupView)
            {
                _rebuildGroups(); // only rebuilds when the set of components changed
            }
        }

        MirrorChannel::SearchResults found;
        if (_channel->pollSearchResults(found) && found.entity == _mirrorEntity && found.text == _searchText)
        {
            TypeUseScope scope; // the hits' values come from the components' properties
            _applySearchResults(found.hits);
        }

        QVector<MirrorChannel::CatalogEntry> catalog;
        if (_channel->pollCatalogEntries(catalog))
        {
            _catalog = catalog;
            _updateAddable();
        }

        QVector<MirrorChannel::PrefabEntry> prefabs;
        if (_channel->pollPrefabs(prefabs))
        {
            _entityList->setAddablePrefabs(prefabs);
        }

        // With no entity selected (e.g. a required-component filter emptied the list),
        // drain the value feed still in flight for the old entity instead of applying
        // it, so it can't land on the next selection.
        if (_mirrorEntity != 0)
        {
            TypeUseScope scope; // the values may come from a plugin's properties
            for (auto& u : _channel->pollValues())
            {
                // Drop values read from a target we've since moved away from: a pump
                // that raced the selection change can still deliver the old entity's
                // leaves, and they share paths with the new one.
                if (u.entity != 0 && (u.entity != _mirrorEntity || u.component != _mirrorComponent))
                {
                    continue;
                }
                _propertyEditor->setPropertyValue(u.path, u.value);
            }
        }
        else
        {
            TypeUseScope scope;
            _channel->pollValues(); // drain
        }

        _tryRebindReleased();

        // Re-send interest each tick so newly expanded fields start mirroring.
        _pushInterest();
    }

    bool EntityComponentBrowser::_isHiddenFromAdd(const QString& path) const
    {
        if (_settings.hiddenAddNamespaces.isEmpty())
        {
            return false;
        }
        // First scope segment, for either separator; a leading "::" (root) is skipped.
        QString p = path;
        while (p.startsWith(QStringLiteral("::")))
        {
            p.remove(0, 2);
        }
        int cut = p.size();
        const int dc = p.indexOf(QStringLiteral("::"));
        const int dot = p.indexOf(QLatin1Char('.'));
        if (dc >= 0)
        {
            cut = dc;
        }
        if (dot >= 0 && dot < cut)
        {
            cut = dot;
        }
        if (cut == p.size())
        {
            return false; // unscoped: no namespace to hide
        }
        const QString top = p.left(cut);
        for (const QString& ns : _settings.hiddenAddNamespaces)
        {
            if (top.compare(ns, Qt::CaseInsensitive) == 0)
            {
                return true;
            }
        }
        return false;
    }

    void EntityComponentBrowser::_updateAddable()
    {
        // Offer every catalogued component the entity does not already have. Both
        // the catalog and the entity's current components are full scoped paths, so
        // compare directly (tags check the tag rows, data the data rows).
        QVector<MirrorChannel::CatalogEntry> addable;
        for (const MirrorChannel::CatalogEntry& entry : _catalog)
        {
            if (_isHiddenFromAdd(entry.path))
            {
                continue;
            }
            const bool present = entry.tag ? _currentTags.contains(entry.path)
                                           : _currentComps.contains(entry.path);
            if (!present)
            {
                addable.append(entry);
            }
        }
        _componentList->setAddableEntries(addable);
    }

    void EntityComponentBrowser::_onAddComponent(const QString& name)
    {
        if (_mirrorEntity == 0 || name.isEmpty())
        {
            return;
        }
        if (_channel)
        {
            // Mirror mode: queue the structural change to the simulation thread.
            _channel->queueStructural(MirrorChannel::StructuralKind::AddComponent, _mirrorEntity, name);
            _channel->requestResync();
        }
        else if (_world)
        {
            // Direct mode: apply under the world guard (the GUI owns the world here).
            withGuard(_guard, [&] {
                flecs::entity comp = findComponentEntity(*_world, name);
                if (comp.is_valid() && _selectedEntity.is_alive())
                {
                    _selectedEntity.add(comp);
                }
            });
            _componentList->setEntity(_world, _selectedEntity);
        }
    }

    void EntityComponentBrowser::_onRemoveComponentId(qulonglong rawId)
    {
        // Tag/pair rows carry a flecs id — the only correct removal identity for a
        // pair, and unambiguous for tags.
        if (rawId == 0)
        {
            return;
        }
        if (_channel && _mirrorEntity != 0)
        {
            _channel->queueStructuralById(MirrorChannel::StructuralKind::RemoveComponent, _mirrorEntity, rawId);
            _channel->requestResync();
        }
        else if (_world && _selectedEntity.is_alive())
        {
            withGuard(_guard, [&] {
                _selectedEntity.remove(static_cast<flecs::id_t>(rawId));
            });
            _componentList->setEntity(_world, _selectedEntity);
        }
    }

    void EntityComponentBrowser::_onRemoveComponent(const QString& name)
    {
        if (name.isEmpty())
        {
            return;
        }
        if (_channel && _mirrorEntity != 0)
        {
            _channel->queueStructural(MirrorChannel::StructuralKind::RemoveComponent, _mirrorEntity, name);
            _channel->requestResync();
        }
        else if (_world && _selectedEntity.is_alive())
        {
            withGuard(_guard, [&] {
                flecs::entity comp = findComponentEntity(*_world, name, /*bridgedOnly=*/false);
                if (comp.is_valid())
                {
                    _selectedEntity.remove(comp);
                }
            });
            _componentList->setEntity(_world, _selectedEntity);
        }
    }

    void EntityComponentBrowser::_onEntityIdSelected(qulonglong id)
    {
        if (!_channel)
        {
            return; // direct mode uses _onEntitySelected
        }
        _mirrorEntity = id;
        _onSearchChanged(); // search the new entity (and keep the grouped view on it)
        emit entityIdSelected(id); // mode-independent selection notification (GUI thread)
        _refreshPinnedTint();      // the pin set is per entity+component
        // Force a full resend: re-selecting the same entity (e.g. after a filter
        // dropped then restored it) leaves the producer's caches unchanged, so the
        // component list / values would otherwise never repopulate.
        _channel->requestResync();

        // Re-bind the property tree to the component that stays selected across the
        // entity switch. Entities usually share a component set, so the component
        // list doesn't change and won't re-emit a selection — without re-binding
        // here the property panel would go blank on every entity change until the
        // user re-clicks the (already-selected) component. If the new entity lacks
        // it, the component list republish will correct the selection a frame later.
        const QString comp = _componentList->currentComponentName();
        if (!comp.isEmpty())
        {
            _onComponentNameSelected(comp); // rebinds + resync + pushes interest
        }
        else
        {
            _mirrorComponent.clear();
            _propertyEditor->unbind();
            _pushInterest();
        }
    }

    void EntityComponentBrowser::_onComponentNameSelected(const QString& name)
    {
        if (!_channel)
        {
            return; // direct mode uses _onComponentSelected
        }
        _rebindPending = false;
        _mirrorComponent = name;
        if (_groupView)
        {
            // The grouped view shows every component: picking one in the list just
            // brings its group into view.
            _propertyEditor->revealGroup(name);
            _pushInterest();
            return;
        }
        // `name` is the row's selection key: the component's full flecs path, or for
        // a data-carrying pair the "Rel (Target)" identity — in that case the schema
        // binds the TYPE THE PAIR CARRIES (mapped via _pairTypeName).
        const QString typeName = _pairTypeName.value(name, name);
        const rttr::type t = TypeBridge::resolveByName(typeName.toUtf8().constData());
        if (t.is_valid())
        {
            _propertyEditor->bindType(t); // schema only — no instance/world touch
            // Open everything the first time; after that, rows you collapsed stay
            // collapsed across entity switches — and a collapsed row isn't mirrored
            // at all with snapshotOpenFieldsOnly (the default), so it costs the sim
            // nothing.
            _propertyEditor->expandAllExceptCollapsed();
        }
        // bindType reset the property tree to empty values; force a full resend so
        // the values repopulate even when re-selecting the same component (whose
        // values are unchanged, so the producer would otherwise dedup them away).
        _channel->requestResync();
        _pushInterest();
        _refreshPinnedTint(); // the pin set is per entity+component
    }

} // namespace rpe
