// std::filesystem::path (auto file editor) support, exercised through the real
// PropertyModel write-back path.
#include <rpe/core/RttrBridge.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

struct Asset
{
    fs::path source = "meshes/box.obj";
    int lod = 2;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Asset>("Asset")
        .property("source", &Asset::source)
        .property("lod", &Asset::lod);
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

    Asset a;
    rttr::instance inst(a);

    // ── std::filesystem::path ─────────────────────────────────────────────────
    check("path is inline-editable", rpe::TypeRenderer::isInlineEditable(rttr::type::get<fs::path>()));
    check("path type is detected", rpe::TypeRenderer::isFilePath(rttr::type::get<fs::path>()));
    {
        rttr::variant v = rpe::bridge::getValueByPath(inst, QStringLiteral("source"));
        check("path reads as its string", rpe::TypeRenderer::toDisplayString(v) == QStringLiteral("meshes/box.obj"));
        bool ok = rpe::bridge::setValueByPath(inst, QStringLiteral("source"), rttr::variant(QStringLiteral("tex/wood.png")));
        check("path set from a QString (editor output)", ok && a.source == fs::path("tex/wood.png"));
        bool ok2 = rpe::bridge::setValueByPath(inst, QStringLiteral("source"), rttr::variant(std::string("a/b.c")));
        check("path set from a std::string", ok2 && a.source == fs::path("a/b.c"));
    }

    // ── Through the PropertyModel schema (editability + display) ──────────────
    {
        rpe::PropertyModel model;
        model.bindType(rttr::type::get<Asset>());
        a.source = "x/y.z";
        model.refresh(inst);
        const QStringList leaves = model.allLeafPaths();
        check("model schema has the path leaf", leaves.contains(QStringLiteral("source")));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
