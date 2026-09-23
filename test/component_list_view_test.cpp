// Component list, on an entity switch (list rebuild):
//   • the selected component must be SCROLLED INTO VIEW — the rebuild otherwise
//     leaves the scrollbar wherever it was (typically the end of the list), so the
//     selected row sits off-screen;
//   • the selection must land on a row the text filter leaves VISIBLE — the filter
//     is applied after the rebuild, so a pick made before it could select a
//     filtered-out component.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QThread>

#include <cstdio>

#define MK(N)     \
    struct N      \
    {             \
        int v = 0; \
    };
MK(Aaa) MK(Bbb) MK(Ccc) MK(Ddd) MK(Eee) MK(Fff) MK(Ggg) MK(Zzz)

RTTR_REGISTRATION
{
#define REG(N) rttr::registration::class_<N>(#N).property("v", &N::v);
    REG(Aaa) REG(Bbb) REG(Ccc) REG(Ddd) REG(Eee) REG(Fff) REG(Ggg) REG(Zzz)
#undef REG
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Aaa, Bbb, Ccc, Ddd, Eee, Fff, Ggg, Zzz>();

    flecs::world world;
    // Different component SETS so switching actually rebuilds the list.
    world.entity("EntA").set<Aaa>({}).set<Bbb>({}).set<Ccc>({}).set<Ddd>({}).set<Eee>({}).set<Fff>({}).set<Ggg>({});
    world.entity("EntB").set<Aaa>({}).set<Bbb>({}).set<Ccc>({}).set<Ddd>({}).set<Eee>({}).set<Fff>({}).set<Ggg>({}).set<Zzz>({});

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    mirror.setScanIntervalsMs(0, 0);
    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.resize(420, 640);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };
    auto* el = browser.entityList()->findChild<QListWidget*>();
    auto* cl = browser.componentList()->findChild<QListWidget*>();
    auto* filterEdit = browser.componentList()->findChild<QLineEdit*>();
    auto selectEntity = [&](const QString& name) {
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == name)
                el->setCurrentRow(i);
    };
    auto selectComp = [&](const QString& text) {
        for (int i = 0; i < cl->count(); ++i)
            if (cl->item(i)->text() == text)
                cl->setCurrentRow(i);
    };
    // Short list → only a couple of rows fit, so scrolling actually matters.
    cl->setFixedHeight(48);

    pump(25);

    // ── Scroll: select a TOP component, force the view to the end, then switch ──
    selectEntity(QStringLiteral("EntA"));
    pump(20);
    selectComp(QStringLiteral("Aaa"));
    pump(15);
    check("Aaa selected on EntA", cl->currentItem() && cl->currentItem()->text() == QStringLiteral("Aaa"));

    cl->scrollToBottom(); // the state the rebuild used to leave behind
    QCoreApplication::processEvents();
    selectEntity(QStringLiteral("EntB"));
    pump(25);

    QListWidgetItem* cur = cl->currentItem();
    check("selection survives the entity switch (Aaa)",
          cur && cur->text() == QStringLiteral("Aaa"));
    check("the selected component is SCROLLED INTO VIEW after the switch",
          cur && cl->viewport()->rect().intersects(cl->visualItemRect(cur)));

    // ── Filter: the selection must be a row the filter leaves visible ───────────
    filterEdit->setText(QStringLiteral("Zzz")); // only Zzz matches (EntB only)
    QCoreApplication::processEvents();
    pump(15);
    cur = cl->currentItem();
    check("filtering re-homes the selection onto a VISIBLE row",
          cur && !cur->isHidden() && cur->text() == QStringLiteral("Zzz"));
    check("the filtered selection is scrolled into view",
          cur && cl->viewport()->rect().intersects(cl->visualItemRect(cur)));

    // Switching entity while filtered must not select a filtered-out component.
    selectEntity(QStringLiteral("EntA")); // EntA has no Zzz → nothing matches the filter
    pump(25);
    cur = cl->currentItem();
    check("a switch with no filter match selects nothing hidden",
          cur == nullptr || !cur->isHidden());

    filterEdit->clear();
    QCoreApplication::processEvents();
    pump(15);
    cur = cl->currentItem();
    check("clearing the filter leaves a visible selection",
          cur && !cur->isHidden());

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
