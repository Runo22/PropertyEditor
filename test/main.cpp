// ─────────────────────────────────────────────────────────────────────────────
//  rpe demo — exercises all three features:
//    1. ECS browser   : Entities (filtered to Transform) → Components → Properties
//    2. PropertyEditor : standalone, write-back data editing (incl. arrays/enum/
//                        color/file path)
//    3. VariantEditor  : the separate "edit any rttr::variant struct" feature
// ─────────────────────────────────────────────────────────────────────────────
#include <rpe/rpe.h>

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <rttr/registration.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

// ── Demo types ────────────────────────────────────────────────────────────────
enum class Shape
{
    Circle,
    Square,
    Triangle
};

struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Transform
{
    Vec3 position;
    Vec3 rotation;
    double scale = 1.0;
};

struct Material
{
    QColor tint = QColor(200, 120, 60);
    double roughness = 0.5;
    std::string texturePath = "textures/wood.png";
    Shape shape = Shape::Square;
    std::vector<double> weights = { 1.0, 0.5, 0.25 }; // array inside the view
};

struct Physics
{
    Vec3 velocity;
    double mass = 1.0;
    bool isStatic = false;
    std::string tag = "default";
    std::vector<double> forces = { 0.0, -9.8, 0.0 }; // array inside the view
};

// ── RTTR registration (with editor hints) ─────────────────────────────────────
RTTR_REGISTRATION
{
    using namespace rttr;

    registration::enumeration<Shape>("Shape")(
        value("Circle", Shape::Circle),
        value("Square", Shape::Square),
        value("Triangle", Shape::Triangle));

    registration::class_<Vec3>("Vec3")
        .property("x", &Vec3::x)
        .property("y", &Vec3::y)
        .property("z", &Vec3::z);

    registration::class_<Transform>("Transform")
        .property("position", &Transform::position)
        .property("rotation", &Transform::rotation)
        .property("scale", &Transform::scale)(
            metadata(rpe::hint::Min, 0.0), metadata(rpe::hint::Max, 100.0), metadata(rpe::hint::Step, 0.1), metadata(rpe::hint::Decimals, 3));

    registration::class_<Material>("Material")
        .property("tint", &Material::tint)(
            metadata(rpe::hint::Editor, rpe::editor::Color))
        .property("roughness", &Material::roughness)(
            metadata(rpe::hint::Min, 0.0), metadata(rpe::hint::Max, 1.0), metadata(rpe::hint::Step, 0.05), metadata(rpe::hint::Decimals, 3))
        .property("texturePath", &Material::texturePath)(
            metadata(rpe::hint::Editor, rpe::editor::FilePath),
            metadata(rpe::hint::Label, "Texture"))
        .property("shape", &Material::shape)
        .property("weights", &Material::weights);

    registration::class_<Physics>("Physics")
        .property("velocity", &Physics::velocity)
        .property("mass", &Physics::mass)(metadata(rpe::hint::Min, 0.0))
        .property("isStatic", &Physics::isStatic)
        .property("tag", &Physics::tag)
        .property("forces", &Physics::forces);
}

// ── Tab 1: ECS browser (only when the flecs layer is built) ───────────────────
// Mirror mode: the world runs on a real simulation THREAD with NO lock in its
// loop. EcsMirror registers a per-frame system that snapshots watched values and
// applies edits on the sim thread; the GUI only reads those snapshots.
#if defined(RPE_WITH_FLECS)
static QWidget* makeEcsTab(QWidget* parent)
{
    static flecs::world world;
    static rpe::EcsMirror mirror;
    static std::atomic<bool> simRunning { true };

    // Register the bridges (void* -> typed instance + value clone). In a plugin
    // build this lives next to each plugin's RTTR registration.
    rpe::TypeBridge::registerTypes<Transform, Physics, Material>();

    static auto player = world.entity("Player");
    static auto enemy = world.entity("Enemy");
    static auto noXform = world.entity("AudioBus"); // no Transform → filtered out

    player.set<Transform>({ { 1, 2, 0 }, {}, 1.0 });
    player.set<Physics>({ { 0.5, 0, 0 }, 70.0, false, "player", { 0, -9.8, 0 } });
    player.set<Material>({});

    enemy.set<Transform>({ { -5, 0, 0 }, { 0, 45, 0 }, 1.5 });
    enemy.set<Physics>({ { -1, 0, 0 }, 90.0, false, "enemy", { 0, -9.8, 0 } });

    noXform.set<Physics>({});

    mirror.attach(&world);                                    // registers the system
    mirror.setRequiredComponent(QStringLiteral("Transform")); // entity-list filter

    auto* browser = new rpe::EntityComponentBrowser(parent);
    // Unreal-style vertical sidebar: entities / components / properties stacked.
    browser->setBrowserLayout(rpe::EntityComponentBrowser::Layout::Vertical);
    browser->setMirror(&mirror); // instead of setWorld — GUI never touches world

    // Simulation thread: NO mutex around the loop.
    static std::thread simThread([] {
        double t = 0.0;
        while (simRunning.load(std::memory_order_relaxed))
        {
            if (auto* tc = player.try_get_mut<Transform>())
            {
                t += 0.02;
                tc->position.x = std::sin(t) * 5.0;
                tc->position.y = std::cos(t) * 2.0;
            }
            if (auto* pc = enemy.try_get_mut<Physics>())
            {
                pc->velocity.x = std::cos(t) * 3.0;
                pc->mass = 90.0 + std::sin(t);
            }
            world.progress(0.016f); // mirror's system runs here, on this thread
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] {
        simRunning.store(false, std::memory_order_relaxed);
        if (simThread.joinable())
        {
            simThread.join();
        }
        mirror.detach();
    });

    return browser;
}
#endif // RPE_WITH_FLECS

// ── Tab 2: standalone write-back editor ────────────────────────────────────────
static QWidget* makePropertyEditorTab(QWidget* parent)
{
    auto* container = new QWidget(parent);
    auto* layout = new QVBoxLayout(container);

    auto* label = new QLabel(
        QObject::tr("Write-back editor for a Material (edits modify the object; "
                    "arrays, enum, color and file-path editors included)."),
        container);
    label->setWordWrap(true);
    layout->addWidget(label);

    static Material material;
    auto* editor = new rpe::PropertyEditor(container);
    editor->editObject(material); // bind + WriteBack + provider in one call
    editor->expandAll();
    layout->addWidget(editor, 1);
    return container;
}

// ── Tab 3: VariantEditor (separate feature) ────────────────────────────────────
static QWidget* makeVariantTab(QWidget* parent)
{
    auto* container = new QWidget(parent);
    auto* layout = new QVBoxLayout(container);

    auto* label = new QLabel(QObject::tr("Independent feature: edit an arbitrary "
                                         "rttr::variant struct."),
                             container);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto* ve = new rpe::VariantEditor(container);
    ve->setVariant(rttr::variant(Transform { { 3, 4, 5 }, { 10, 20, 30 }, 2.0 }));
    layout->addWidget(ve, 1);

    auto* echo = new QLabel(container);
    layout->addWidget(echo);
    QObject::connect(ve, &rpe::VariantEditor::valueChanged, [echo](const QString& path, const rttr::variant&) {
        echo->setText(QObject::tr("Edited: %1").arg(path));
    });
    return container;
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("rpe demo"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("RTTR Property Editor — Demo"));
    window.resize(1080, 680);

    auto* tabs = new QTabWidget(&window);
#if defined(RPE_WITH_FLECS)
    tabs->addTab(makeEcsTab(tabs), QStringLiteral("ECS Browser"));
#endif
    tabs->addTab(makePropertyEditorTab(tabs), QStringLiteral("Property Editor"));
    tabs->addTab(makeVariantTab(tabs), QStringLiteral("Variant Editor"));
    window.setCentralWidget(tabs);
    window.show();
    return app.exec();
}
