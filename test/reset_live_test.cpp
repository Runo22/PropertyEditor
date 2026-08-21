// Reset / un-freeze must return a node to the CURRENT live value — not a stale one.
// In mirror mode a node is frozen while an inline editor is open (beginLocalEdit) or
// via the context menu's "Local edit (freeze live value)". While frozen, live updates
// were dropped, and the mirror dedups — so after reset the node used to show a stale
// value (and a stale cached display). Both must now reflect the latest live value.
#include <rpe/core/TypeRenderer.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QToolButton>

#include <cstdio>

struct Comp
{
    int hp = 0;
    int mp = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Comp>("Comp").property("hp", &Comp::hp).property("mp", &Comp::mp);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QString displayOf(rpe::PropertyModel& m, const QString& path)
{
    for (int r = 0; r < m.rowCount(); ++r)
    {
        const QModelIndex idx = m.index(r, 0);
        if (idx.data(rpe::PropertyPathRole).toString() == path)
            return m.index(r, 1).data(Qt::DisplayRole).toString();
    }
    return QStringLiteral("<not found>");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::PropertyModel model;
    model.bindType(rttr::type::get<Comp>());
    Comp c { 10, 20 };
    rttr::instance inst(c);
    model.refresh(inst);
    check("initial hp shows 10", displayOf(model, QStringLiteral("hp")) == QStringLiteral("10"));

    // ── Freeze hp, then the sim moves it to 50 (injected while frozen) ──────────
    model.beginLocalEdit(QStringLiteral("hp"));
    check("frozen hp still shows 10", displayOf(model, QStringLiteral("hp")) == QStringLiteral("10"));

    model.setPropertyValue(QStringLiteral("hp"), rttr::variant(50));
    QCoreApplication::processEvents(); // apply the queued injection
    check("frozen hp does NOT show the injected value (still 10)",
          displayOf(model, QStringLiteral("hp")) == QStringLiteral("10"));

    // Reset → must snap to the CURRENT live value (50), not the stale 10.
    model.resetNode(QStringLiteral("hp"));
    check("reset shows the current live value (50), not the stale one",
          displayOf(model, QStringLiteral("hp")) == QStringLiteral("50"));
    check("no local edits remain", !model.hasAnyLocalEdit());

    // ── After reset the node tracks live again ─────────────────────────────────
    model.setPropertyValue(QStringLiteral("hp"), rttr::variant(51));
    QCoreApplication::processEvents();
    check("un-frozen hp resumes live tracking (51)",
          displayOf(model, QStringLiteral("hp")) == QStringLiteral("51"));

    // ── Freeze with NO injection → reset returns the unchanged live value ───────
    model.beginLocalEdit(QStringLiteral("mp"));
    model.resetNode(QStringLiteral("mp"));
    check("reset with no live change returns the live value (20)",
          displayOf(model, QStringLiteral("mp")) == QStringLiteral("20"));

    // ── resetAll releases several frozen nodes to their live values ────────────
    model.beginLocalEdit(QStringLiteral("hp"));
    model.beginLocalEdit(QStringLiteral("mp"));
    model.setPropertyValue(QStringLiteral("hp"), rttr::variant(7));
    model.setPropertyValue(QStringLiteral("mp"), rttr::variant(8));
    QCoreApplication::processEvents();
    model.resetAll();
    check("resetAll snaps hp to live (7)", displayOf(model, QStringLiteral("hp")) == QStringLiteral("7"));
    check("resetAll snaps mp to live (8)", displayOf(model, QStringLiteral("mp")) == QStringLiteral("8"));
    check("resetAll leaves no local edits", !model.hasAnyLocalEdit());

    // ── The Reset button is enabled ONLY while something is frozen ─────────────
    {
        rpe::PropertyEditor editor;
        editor.bindType(rttr::type::get<Comp>());
        Comp c2 { 3, 4 };
        rttr::instance inst2(c2);
        editor.refresh(inst2);

        QToolButton* resetBtn = nullptr;
        for (auto* b : editor.findChildren<QToolButton*>())
            if (b->text() == QStringLiteral("Reset"))
                resetBtn = b;
        check("Reset button found", resetBtn != nullptr);

        auto* m = editor.findChild<rpe::PropertyModel*>();
        check("Reset disabled when nothing is frozen", resetBtn && !resetBtn->isEnabled());
        m->beginLocalEdit(QStringLiteral("hp"));
        check("Reset enabled once a value is frozen", resetBtn && resetBtn->isEnabled());
        m->resetAll();
        check("Reset disabled again after releasing the freeze", resetBtn && !resetBtn->isEnabled());
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
