#include <rttr_property_editor/rttr_property_editor.h>

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <rttr/registration>
#include <rttr/type>

#include <flecs.h>

#include <cmath>
#include <string>
#include <vector>

// ── Demo types ────────────────────────────────────────────────────────────────

enum class Color { Red, Green, Blue };

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct TransformComponent {
    Vec3   position;
    Vec3   rotation;
    double scale = 1.0;
};

struct PhysicsComponent {
    Vec3              velocity;
    double            mass    = 1.0;
    bool              isStatic= false;
    std::string       tag     = "default";
    std::vector<double> forces;
};

// ── RTTR registration ─────────────────────────────────────────────────────────

RTTR_REGISTRATION
{
    using namespace rttr;

    registration::enumeration<Color>("Color")
    (
        value("Red",   Color::Red),
        value("Green", Color::Green),
        value("Blue",  Color::Blue)
    );

    registration::class_<Vec3>("Vec3")
        .property("x", &Vec3::x)
        .property("y", &Vec3::y)
        .property("z", &Vec3::z)
    ;

    registration::class_<TransformComponent>("TransformComponent")
        .property("position", &TransformComponent::position)
        .property("rotation", &TransformComponent::rotation)
        .property("scale",    &TransformComponent::scale)
    ;

    registration::class_<PhysicsComponent>("PhysicsComponent")
        .property("velocity", &PhysicsComponent::velocity)
        .property("mass",     &PhysicsComponent::mass)
        .property("isStatic", &PhysicsComponent::isStatic)
        .property("tag",      &PhysicsComponent::tag)
        .property("forces",   &PhysicsComponent::forces)
    ;
}

// ── Tab 1: Standalone PropertyEditor demo ─────────────────────────────────────

static QWidget* makePropertyEditorDemo(QWidget* parent)
{
    auto* container = new QWidget(parent);
    auto* layout    = new QVBoxLayout(container);

    auto* label = new QLabel(
        QStringLiteral("Live 50Hz PhysicsComponent — double-click a value to edit, "
                        "right-click for override/reset."), container);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto* editor = new rpe::PropertyEditor(container);
    editor->bindType(rttr::type::get<PhysicsComponent>());
    editor->setMode(rpe::DisplayMode::EditLive);
    layout->addWidget(editor, 1);

    // Static live object — persists for the life of the app
    static PhysicsComponent live;
    live.forces = {0.0, 0.0, 0.0};

    auto* timer = new QTimer(container);
    timer->setInterval(20); // 50 Hz
    QObject::connect(timer, &QTimer::timeout, [editor]() {
        static double t = 0.0;
        t += 0.02;
        live.velocity.x = std::sin(t) * 10.0;
        live.velocity.y = std::cos(t) * 5.0;
        live.velocity.z = std::sin(t * 0.5) * 2.0;
        live.mass       = 1.0 + std::abs(std::sin(t * 0.3));
        live.isStatic   = static_cast<int>(t) % 10 < 5;
        live.forces[0]  = std::sin(t * 2.0);
        live.forces[1]  = std::cos(t * 3.0);
        live.forces[2]  = std::sin(t * 0.7);
        editor->refresh(rttr::instance(live));
    });
    timer->start();

    QObject::connect(editor, &rpe::PropertyEditor::propertyEdited,
                     [](const QString& path, const rttr::variant&) {
        qDebug("propertyEdited: %s", qPrintable(path));
    });

    return container;
}

// ── Tab 2: EntityComponentBrowser demo ───────────────────────────────────────

static QWidget* makeEcsBrowserDemo(QWidget* parent)
{
    static flecs::world world;

    // Create three entities with varying components
    static auto player   = world.entity("Player");
    static auto enemy    = world.entity("Enemy");
    static auto obstacle = world.entity("Obstacle");

    player.set<TransformComponent>({ {1.0, 2.0, 0.0},  {0.0, 0.0,  0.0}, 1.0 });
    player.set<PhysicsComponent>  ({ {0.5, 0.0, 0.0}, 70.0, false, "player", {0.0, -9.8} });

    enemy.set<TransformComponent>({ {-5.0, 0.0, 0.0}, {0.0, 45.0, 0.0}, 1.5 });
    enemy.set<PhysicsComponent>  ({ {-1.0, 0.0, 0.0}, 90.0, false, "enemy", {0.0, -9.8} });

    obstacle.set<TransformComponent>({ {0.0, 0.0, 5.0}, {0.0, 0.0, 0.0}, 3.0 });
    // obstacle has no PhysicsComponent intentionally

    auto* browser = new rpe::EntityComponentBrowser(parent);
    browser->setWorld(&world);
    browser->setLiveUpdateIntervalMs(20); // 50 Hz

    // Animate entities so live values change visibly
    auto* animTimer = new QTimer(parent);
    animTimer->setInterval(20);
    QObject::connect(animTimer, &QTimer::timeout, []() {
        static double t = 0.0;
        t += 0.02;

        if (auto* tc = player.get_mut<TransformComponent>()) {
            tc->position.x = std::sin(t) * 5.0;
            tc->position.y = std::cos(t) * 2.0;
        }
        if (auto* pc = enemy.get_mut<PhysicsComponent>()) {
            pc->velocity.x = std::cos(t) * 3.0;
            pc->velocity.y = std::sin(t * 0.7);
        }
    });
    animTimer->start();

    return browser;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("RTTR Property Editor Test"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("RTTR Property Editor — Test"));
    window.resize(1000, 650);

    auto* tabs = new QTabWidget(&window);
    tabs->addTab(makePropertyEditorDemo(tabs), QStringLiteral("PropertyEditor Demo"));
    tabs->addTab(makeEcsBrowserDemo(tabs),      QStringLiteral("ECS Browser Demo"));

    window.setCentralWidget(tabs);
    window.show();

    return app.exec();
}
