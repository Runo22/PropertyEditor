// Renders the EntityComponentBrowser (mirror mode, component-editing enabled) to
// a PNG so the add/remove design can be reviewed. Offscreen; no display needed.
//   usage: rpe_screenshot <out.png>
#include <rpe/core/EditorHints.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QPixmap>
#include <QThread>

#include <vector>

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
    std::vector<int> resistances = { 10, 5, 0 }; // expandable array
};
struct Velocity
{
    double dx = 0, dy = 0;
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Transform>("Transform").property("position", &Transform::position).property("scale", &Transform::scale);
    registration::class_<Health>("Health")
        .property("hp", &Health::hp)(metadata(rpe::hint::Min, 0), metadata(rpe::hint::Max, 100))
        .property("armor", &Health::armor)
        .property("invulnerable", &Health::invulnerable)
        .property("resistances", &Health::resistances);
    registration::class_<Velocity>("Velocity").property("dx", &Velocity::dx).property("dy", &Velocity::dy);
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("browser.png");

    rpe::TypeBridge::registerTypes<Transform, Health, Velocity>();

    flecs::world world;
    world.component<Transform>();
    world.component<Health>();
    world.component<Velocity>();

    auto player = world.entity("Player");
    player.set<Transform>({ { 12.5, 3.0, -4.0 }, 1.0 }).set<Health>({ 87, 40, false }).set<Velocity>({ 1.5, 0.0 });
    world.entity("Enemy").set<Transform>({ { -8.0, 0.0, 2.0 }, 1.2 }).set<Health>({ 50, 10, false });
    world.entity("Camera").set<Transform>({ { 0, 10, 0 }, 1.0 });

    rpe::EcsMirror mirror;
    mirror.attach(&world);

    rpe::EntityComponentBrowser browser;
    browser.setBrowserLayout(rpe::EntityComponentBrowser::Layout::Vertical);
    browser.setMirror(&mirror);

    rpe::EntityComponentBrowser::Settings st = browser.settings();
    st.allowComponentEditing = true;          // show "+" and per-row "×"
    st.requiredComponent = QStringLiteral("Transform");
    st.requiredComponentEnabled = true;
    browser.setSettings(st);

    browser.resize(360, 660);
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
    {
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == QStringLiteral("Player"))
                el->setCurrentRow(i);
    }
    pump(20);
    if (auto* cl = browser.componentList()->findChild<QListWidget*>())
    {
        for (int i = 0; i < cl->count(); ++i)
            if (cl->item(i)->text() == QStringLiteral("Health"))
                cl->setCurrentRow(i); // show Health properties
    }
    pump(30);

    const QPixmap shot = browser.grab();
    const bool ok = shot.save(out, "PNG");
    printf("%s screenshot: %s (%dx%d)\n", ok ? "saved" : "FAILED to save",
           out.toUtf8().constData(), shot.width(), shot.height());

    mirror.detach();
    return ok ? 0 : 1;
}
