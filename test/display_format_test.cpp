// Float display formatting: fixed-point (never scientific notation), trailing
// zeros trimmed, tiny values collapse to "0", configurable decimals (default 3).
#include <rpe/core/TypeRenderer.h>

#include <cstdio>

static int g_fails = 0;
static void check(const char* name, const QString& got, const char* want)
{
    const bool ok = got == QLatin1String(want);
    printf("[%s] %-28s got=\"%s\" want=\"%s\"\n", ok ? "PASS" : "FAIL", name,
           got.toUtf8().constData(), want);
    if (!ok)
        ++g_fails;
}

int main()
{
    using rpe::TypeRenderer;
    auto s = [](double v) { return TypeRenderer::toDisplayString(rttr::variant(v)); };

    check("tiny -> 0 (not 2.5e-05)", s(2.5e-05), "0");
    check("negative tiny -> 0", s(-2.5e-05), "0");
    check("e-3 boundary rounds", s(1e-3), "0.001");
    check("below half-ulp rounds to 0", s(0.0004), "0");
    check("plain half", s(0.5), "0.5");
    check("trailing zeros trimmed", s(1.50), "1.5");
    check("3-decimal truncation", s(1.23456), "1.235");
    check("integer-valued double", s(42.0), "42");
    check("negative value", s(-3.25), "-3.25");
    check("zero", s(0.0), "0");
    check("float type too", TypeRenderer::toDisplayString(rttr::variant(0.0001f)), "0");

    // Configurable decimals.
    TypeRenderer::setFloatDecimals(5);
    check("5 decimals shows 2.5e-05", s(2.5e-05), "0.00003");
    TypeRenderer::setFloatDecimals(3);

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
