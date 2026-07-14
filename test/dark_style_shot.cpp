// Renders the inspector under rpe::darkStyleSheet() to PNGs for review:
//   <prefix>_browser.png   — entity list + component list + property tree (dark)
//   <prefix>_add.png       — the add-component popup (dark)
//   <prefix>_pathedit.png  — a path cell's editor open (dark)
//   <prefix>_menu.png      — the file/folder browse menu (dark)
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/DarkStyle.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAbstractItemModel>
#include <QApplication>
#include <QHeaderView>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QThread>
#include <QToolButton>
#include <QTreeView>

#include <filesystem>

namespace fs = std::filesystem;

struct Vec3
{
    double x = 0, y = 0, z = 0;
};
struct Transform
{
    Vec3 position;
    double scale = 1.0;
};
struct Health
{
    int hp = 100;
    int armor = 25;
    bool invulnerable = false;
    std::vector<int> resistances = { 10, 5, 0 };
    fs::path deathSound = "sfx/die.wav";
};
namespace physics
{
    struct RigidBody
    {
        double mass = 1.0;
    };
} // namespace physics

// Standalone editor subject (part 2): paths + arrays.
struct Material
{
    fs::path albedoTexture = "textures/rock_albedo.png";
    fs::path contentRoot = "assets/materials";
    std::vector<int> mipLevels = { 2048, 1024, 512, 256 };
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Transform>("Transform").property("position", &Transform::position).property("scale", &Transform::scale);
    registration::class_<Health>("Health")
        .property("hp", &Health::hp)
        .property("armor", &Health::armor)
        .property("invulnerable", &Health::invulnerable)
        .property("resistances", &Health::resistances)
        .property("deathSound", &Health::deathSound);
    registration::class_<physics::RigidBody>("physics::RigidBody").property("mass", &physics::RigidBody::mass);
    registration::class_<Material>("Material")
        .property("albedoTexture", &Material::albedoTexture)
        .property("contentRoot", &Material::contentRoot)
        .property("mipLevels", &Material::mipLevels);
}

static void saveShot(QWidget* w, const QString& path)
{
    const QPixmap pm = w->grab();
    printf("%s %s (%dx%d)\n", pm.save(path, "PNG") ? "saved" : "FAILED", path.toUtf8().constData(), pm.width(), pm.height());
}

static QModelIndex findByPath(QAbstractItemModel* m, const QString& name, const QModelIndex& parent = {})
{
    for (int r = 0; r < m->rowCount(parent); ++r)
    {
        const QModelIndex idx = m->index(r, 0, parent);
        if (idx.data(rpe::PropertyPathRole).toString() == name)
            return idx;
        if (const QModelIndex hit = findByPath(m, name, idx); hit.isValid())
            return hit;
    }
    return {};
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString prefix = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("dark");

    // Apply the dark theme app-wide so popups/menus are styled too.
    app.setStyleSheet(rpe::darkStyleSheet());

    rpe::TypeBridge::registerTypes<Transform, Health, physics::RigidBody, Material>();

    // ── Part 1: the full browser (entities, components, property tree) ────────
    flecs::world world;
    world.component<Transform>();
    world.component<Health>();
    world.component<physics::RigidBody>();
    auto player = world.entity("Player").set<Transform>({ { 12.5, 3.0, -4.0 }, 1.0 }).set<Health>({ 87, 40, false, { 10, 5, 0 } }).set<physics::RigidBody>({ 2.0 });
    auto enemy = world.entity("Enemy").set<Transform>({ { -8, 0, 2 }, 1.2 }).set<Health>({ 50, 10, false, { 3, 1 } });

    rpe::EcsMirror mirror;
    mirror.attach(&world);

    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    rpe::EntityComponentBrowser::Settings st = browser.settings();
    st.layout = rpe::EntityComponentBrowser::Layout::Vertical;
    st.allowComponentEditing = true;
    browser.setSettings(st);
    browser.resize(360, 720);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(6);
        }
    };

    pump(25);
    QListWidget* cl = browser.componentList()->findChild<QListWidget*>();
    if (cl)
        for (int i = 0; i < cl->count(); ++i)
            if (cl->item(i)->text() == QStringLiteral("Health"))
                cl->setCurrentRow(i);
    pump(25);
    // Expand the array so its element rows show in the tree.
    if (auto* tv = browser.propertyEditor()->findChild<QTreeView*>())
        tv->expandAll();
    pump(6);
    saveShot(&browser, prefix + QStringLiteral("_browser.png"));

    // Pinned-properties watch list: pin values from BOTH entities, then capture the
    // widget and the browser with its tinted (teal) pinned row.
    rpe::PinnedPropertiesWidget pinWidget;
    browser.setPinnedPropertiesWidget(&pinWidget);
    pinWidget.resize(360, 170);
    pinWidget.show();
    pinWidget.pin(static_cast<qulonglong>(player.id()), QStringLiteral("Player"), QStringLiteral("Health"), QStringLiteral("hp"));
    pinWidget.pin(static_cast<qulonglong>(player.id()), QStringLiteral("Player"), QStringLiteral("Health"), QStringLiteral("resistances.[0]"));
    pinWidget.pin(static_cast<qulonglong>(enemy.id()), QStringLiteral("Enemy"), QStringLiteral("Health"), QStringLiteral("armor"));
    pump(10);
    pinWidget.pollNow();
    QCoreApplication::processEvents();
    saveShot(&pinWidget, prefix + QStringLiteral("_pins.png"));
    saveShot(&browser, prefix + QStringLiteral("_browser_pinned.png"));

    // Add-component popup.
    if (auto* addBtn = browser.componentList()->findChild<QToolButton*>())
    {
        addBtn->click();
        pump(3);
        if (QWidget* popup = QApplication::activePopupWidget())
            saveShot(popup, prefix + QStringLiteral("_add.png"));
        if (QWidget* popup = QApplication::activePopupWidget())
            popup->close();
    }
    mirror.detach();

    // ── Part 2: a standalone editor to show a path editor + file/folder menu ──
    Material mat;
    rpe::PropertyEditor editor;
    editor.setToolbarVisible(false);
    editor.editObject(mat);
    editor.expandAll();
    editor.resize(430, 300);
    if (auto* tv = editor.view())
        tv->header()->resizeSection(0, 200);
    editor.show();
    QCoreApplication::processEvents();

    if (QTreeView* view = editor.view())
    {
        if (const QModelIndex idx = findByPath(view->model(), QStringLiteral("albedoTexture")); idx.isValid())
        {
            const QModelIndex valueIdx = idx.siblingAtColumn(1);
            view->openPersistentEditor(valueIdx);
            QCoreApplication::processEvents();
            saveShot(&editor, prefix + QStringLiteral("_pathedit.png"));

            if (auto* btn = view->findChild<QToolButton*>())
            {
                if (QMenu* menu = btn->menu())
                {
                    menu->popup(QPoint(0, 0));
                    QCoreApplication::processEvents();
                    saveShot(menu, prefix + QStringLiteral("_menu.png"));
                    menu->close();
                }
            }
            view->closePersistentEditor(valueIdx);
        }
    }

    return 0;
}
