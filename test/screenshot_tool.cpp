// Renders the EntityComponentBrowser (mirror mode, component-editing enabled) to
// PNGs so the add/remove design can be reviewed. Offscreen; no display needed.
//   usage: rpe_screenshot <out-prefix>   → <prefix>_normal.png, _confirm.png, _add.png
#include <rpe/core/EditorHints.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <filesystem>
#include <QCoreApplication>
#include <QListWidget>
#include <QMouseEvent>
#include <QPixmap>
#include <QThread>
#include <QToolButton>

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
    std::filesystem::path deathSound = "sfx/die.wav";
};
namespace physics
{
    struct RigidBody
    {
        double mass = 1.0;
    };
    struct Collider
    {
        double radius = 0.5;
    };
} // namespace physics
namespace render
{
    struct Mesh
    {
        int lod = 0;
    };
    struct Material
    {
        double roughness = 0.5;
    };
} // namespace render

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
    registration::class_<physics::Collider>("physics::Collider").property("radius", &physics::Collider::radius);
    registration::class_<render::Mesh>("render::Mesh").property("lod", &render::Mesh::lod);
    registration::class_<render::Material>("render::Material").property("roughness", &render::Material::roughness);
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString prefix = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("browser");

    rpe::TypeBridge::registerTypes<Transform, Health, physics::RigidBody, physics::Collider, render::Mesh, render::Material>();

    flecs::world world;
    world.component<Transform>();
    world.component<Health>();
    world.component<physics::RigidBody>();
    world.component<physics::Collider>();
    world.component<render::Mesh>();
    world.component<render::Material>();

    world.entity("Player").set<Transform>({ { 12.5, 3.0, -4.0 }, 1.0 }).set<Health>({ 87, 40, false, { 10, 5, 0 } });
    world.entity("Enemy").set<Transform>({ { -8, 0, 2 }, 1.2 }).set<Health>({ 50, 10, false, {} });

    rpe::EcsMirror mirror;
    mirror.attach(&world);

    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    rpe::EntityComponentBrowser::Settings st = browser.settings();
    st.layout = rpe::EntityComponentBrowser::Layout::Vertical;
    st.allowComponentEditing = true;
    st.requiredComponent = QStringLiteral("Transform");
    st.requiredComponentEnabled = true;
    browser.setSettings(st);
    browser.resize(360, 680);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };

    pump(25);
    if (auto* el = browser.entityList()->findChild<QListWidget*>())
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == QStringLiteral("Player"))
                el->setCurrentRow(i);
    pump(20);
    QListWidget* cl = browser.componentList()->findChild<QListWidget*>();
    if (cl)
        for (int i = 0; i < cl->count(); ++i)
            if (cl->item(i)->text() == QStringLiteral("Health"))
                cl->setCurrentRow(i);
    pump(30);

    auto saveShot = [](QWidget* w, const QString& path) {
        const QPixmap pm = w->grab();
        printf("%s %s (%dx%d)\n", pm.save(path, "PNG") ? "saved" : "FAILED", path.toUtf8().constData(), pm.width(), pm.height());
    };

    // 1) Normal state.
    saveShot(&browser, prefix + "_normal.png");

    // 2) Delete-confirm: synthesise a click on the trash glyph of the "Transform"
    //    row to arm it (the delegate consumes the release and shows the confirm).
    if (cl)
    {
        for (int i = 0; i < cl->count(); ++i)
        {
            if (cl->item(i)->text() != QStringLiteral("Transform"))
                continue;
            const QRect r = cl->visualItemRect(cl->item(i));
            const QPoint pos(r.right() - r.height() / 2, r.center().y()); // trash glyph
            for (QEvent::Type t : { QEvent::MouseButtonPress, QEvent::MouseButtonRelease })
            {
                QMouseEvent me(t, pos, cl->viewport()->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(cl->viewport(), &me);
            }
        }
    }
    pump(3);
    saveShot(&browser, prefix + "_confirm.png");

    // 3) Add popup open (grouped by namespace + filter).
    if (auto* addBtn = browser.componentList()->findChild<QToolButton*>())
    {
        addBtn->click();
        pump(3);
        if (QWidget* popup = QApplication::activePopupWidget())
            saveShot(popup, prefix + "_add.png");
        if (QWidget* popup = QApplication::activePopupWidget())
            popup->close();
    }

    mirror.detach();
    return 0;
}
