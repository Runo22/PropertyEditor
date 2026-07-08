// std::optional<T> support (safe read + set), exercised through the real
// PropertyModel / RttrBridge write-back path.
#include <rpe/core/OptionalSupport.h> // must be visible where optional types are registered
#include <rpe/core/RttrBridge.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>
#include <optional>

struct Asset
{
    std::optional<int> lodBias;             // empty
    std::optional<double> fadeStart = 12.5; // engaged
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Asset>("Asset")
        .property("lodBias", &Asset::lodBias)
        .property("fadeStart", &Asset::fadeStart);
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

    RPE_REGISTER_OPTIONAL(int);
    RPE_REGISTER_OPTIONAL(double);

    Asset a;
    rttr::instance inst(a);

    // ── read (safe: empty shows "(none)", engaged shows inner) ────────────────
    {
        rttr::variant e = rpe::bridge::getValueByPath(inst, QStringLiteral("lodBias"));
        check("empty optional reads as \"(none)\"", rpe::TypeRenderer::toDisplayString(e) == QStringLiteral("(none)"));
        rttr::variant f = rpe::bridge::getValueByPath(inst, QStringLiteral("fadeStart"));
        check("engaged optional shows inner value (12.5)", rpe::TypeRenderer::toDisplayString(f) == QStringLiteral("12.5"));
    }

    // ── set (engage from inner value, update, clear to nullopt) ───────────────
    {
        bool ok = rpe::bridge::setValueByPath(inst, QStringLiteral("lodBias"), rttr::variant(3));
        check("setting empty optional engages it (int 3)", ok && a.lodBias.has_value() && a.lodBias.value() == 3);
        bool ok2 = rpe::bridge::setValueByPath(inst, QStringLiteral("fadeStart"), rttr::variant(2.0));
        check("setting engaged optional updates it (2.0)", ok2 && a.fadeStart.has_value() && a.fadeStart.value() == 2.0);
        bool ok3 = rpe::bridge::setValueByPath(inst, QStringLiteral("lodBias"),
                                               rpe::OptionalBridge::disengage(rttr::type::get<std::optional<int>>()));
        check("optional can be cleared to nullopt", ok3 && !a.lodBias.has_value());
    }

    // ── Through the PropertyModel schema (editability + display) ──────────────
    {
        rpe::PropertyModel model;
        model.bindType(rttr::type::get<Asset>());
        a.lodBias = std::optional<int>{}; // empty again
        a.fadeStart = 5.0;
        model.refresh(inst);
        const QStringList leaves = model.allLeafPaths();
        check("model schema has both optionals as leaves",
              leaves.contains(QStringLiteral("lodBias")) && leaves.contains(QStringLiteral("fadeStart")));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
