// reflectgen — turns REFLECT / PROP / NOPROP annotations (reflect/Reflect.h) in
// one header into a .cpp that registers the types with RTTR, the rpe property
// editor and flecs. Uses only the libclang C API, which ships with the official
// LLVM installers (libclang.dll / libclang.lib + clang-c headers on Windows).
//
//   reflectgen <header> -o <out.cpp> [--depfile <out.d>] [--include <spelling>]
//              [--default-fields explicit|all] [--allow-errors]
//              -- <compiler arguments: -I/-D/-std or, with --driver-mode=cl, /I /D /std>
//
// Output is written only when its content changes, so an unchanged header never
// triggers a recompile downstream.

#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{

    // ── small helpers ────────────────────────────────────────────────────────────

    std::string take(CXString s)
    {
        const char* c = clang_getCString(s);
        std::string r = c ? c : "";
        clang_disposeString(s);
        return r;
    }

    struct Loc
    {
        std::string file;
        unsigned line = 0, col = 0;
    };

    Loc locOf(CXCursor c)
    {
        CXFile f = nullptr;
        unsigned line = 0, col = 0, off = 0;
        clang_getSpellingLocation(clang_getCursorLocation(c), &f, &line, &col, &off);
        return { f ? take(clang_getFileName(f)) : std::string("<unknown>"), line, col };
    }

    int g_errors = 0;

    void report(const Loc& l, const char* severity, const std::string& msg)
    {
#ifdef _WIN32 // MSVC format, so Visual Studio's error list can jump to it
        std::fprintf(stderr, "%s(%u,%u): %s RG0001: %s\n", l.file.c_str(), l.line, l.col, severity, msg.c_str());
#else
        std::fprintf(stderr, "%s:%u:%u: %s: %s\n", l.file.c_str(), l.line, l.col, severity, msg.c_str());
#endif
    }
    void error(const Loc& l, const std::string& m)
    {
        ++g_errors;
        report(l, "error", m);
    }
    void warning(const Loc& l, const std::string& m)
    {
        report(l, "warning", m);
    }

    std::string trim(std::string_view s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return std::string(s.substr(b, e - b));
    }

    // ── option lists: `min = 0, max = f(1, 2), slider, label = "a, b"` ───────────

    struct Opt
    {
        std::string key, value; // value empty for bare flags
        bool hasValue = false;
    };

    std::vector<Opt> parseOpts(std::string_view s)
    {
        std::vector<std::string> parts;
        std::string cur;
        int depth = 0;
        char quote = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const char ch = s[i];
            if (quote)
            {
                cur += ch;
                if (ch == '\\' && i + 1 < s.size())
                    cur += s[++i];
                else if (ch == quote)
                    quote = 0;
                continue;
            }
            if (ch == '"' || ch == '\'')
                quote = ch;
            else if (ch == '(' || ch == '[' || ch == '{' || ch == '<')
                ++depth;
            else if ((ch == ')' || ch == ']' || ch == '}' || ch == '>') && depth > 0)
                --depth;
            else if (ch == ',' && depth == 0)
            {
                parts.push_back(cur);
                cur.clear();
                continue;
            }
            cur += ch;
        }
        parts.push_back(cur);

        std::vector<Opt> out;
        for (const std::string& p : parts)
        {
            const std::string t = trim(p);
            if (t.empty())
                continue;
            Opt o;
            const size_t eq = t.find('=');
            const bool isAssign = eq != std::string::npos && eq > 0 && (eq + 1 >= t.size() || t[eq + 1] != '=')
                                  && t.find('"') > eq; // '=' inside a string is not an assignment
            if (isAssign)
            {
                o.key = trim(std::string_view(t).substr(0, eq));
                o.value = trim(std::string_view(t).substr(eq + 1));
                o.hasValue = true;
            }
            else
            {
                o.key = t;
            }
            out.push_back(std::move(o));
        }
        return out;
    }

    // The text after `prefix` of the first [[clang::annotate]] on `c` carrying it.
    std::optional<std::string> annotation(CXCursor c, std::string_view prefix)
    {
        struct Ctx
        {
            std::string_view prefix;
            std::optional<std::string> found;
        } ctx{ prefix, std::nullopt };
        clang_visitChildren(
            c,
            [](CXCursor ch, CXCursor, CXClientData d) {
                auto* x = static_cast<Ctx*>(d);
                if (clang_getCursorKind(ch) == CXCursor_AnnotateAttr)
                {
                    const std::string s = take(clang_getCursorSpelling(ch));
                    if (s.rfind(x->prefix, 0) == 0)
                    {
                        x->found = s.substr(x->prefix.size());
                        return CXChildVisit_Break;
                    }
                }
                return CXChildVisit_Continue;
            },
            &ctx);
        return ctx.found;
    }

    bool hasOpt(const std::vector<Opt>& opts, std::string_view key)
    {
        for (const Opt& o : opts)
            if (o.key == key)
                return true;
        return false;
    }

    // ── collected model ──────────────────────────────────────────────────────────

    struct Scope
    {
        std::string qualified;            // "game::Outer::Light" (no leading ::)
        std::vector<std::string> nsParts; // namespaces only, for `using namespace`
        bool ok = true;
    };

    Scope scopeOf(CXCursor c)
    {
        Scope s;
        std::vector<std::string> path; // outermost first
        for (CXCursor p = c; !clang_Cursor_isNull(p) && clang_getCursorKind(p) != CXCursor_TranslationUnit;
             p = clang_getCursorSemanticParent(p))
        {
            const CXCursorKind k = clang_getCursorKind(p);
            const std::string name = take(clang_getCursorSpelling(p));
            if (k == CXCursor_LinkageSpec)
                continue;
            if (k == CXCursor_Namespace)
            {
                if (clang_Cursor_isAnonymous(p))
                    s.ok = false; // only visible inside the header's own TU
                else if (!clang_Cursor_isInlineNamespace(p))
                {
                    path.insert(path.begin(), name);
                    s.nsParts.insert(s.nsParts.begin(), name);
                }
                continue;
            }
            const bool named = k == CXCursor_StructDecl || k == CXCursor_ClassDecl || k == CXCursor_EnumDecl
                               || k == CXCursor_UnionDecl;
            if (!named || name.empty() || clang_Cursor_isAnonymous(p))
            {
                s.ok = false; // local / unnamed type
                continue;
            }
            path.insert(path.begin(), name);
        }
        for (size_t i = 0; i < path.size(); ++i)
            s.qualified += (i ? "::" : "") + path[i];
        return s;
    }

    struct Field
    {
        std::string name;
        Loc loc;
        std::vector<Opt> hints;
        bool flagsEnum = false;            // type is a REFLECT(flags) enum
        std::optional<std::string> optionalInner; // std::optional<X> → X
        bool flecsMeta = false;            // flecs knows how to describe the type
        std::string metaDep;               // embedded meta struct ("::ns::Vec3"), if any
        std::string typeSpelling;
    };

    struct Record
    {
        Scope scope;
        Loc loc;
        std::string rttrName;  // C++ string literal
        std::string flecsName; // C++ expression (string literal) or "nullptr"
        bool component = false;
        bool meta = false;
        bool isTag = false;
        std::vector<Field> fields;
    };

    struct Enum
    {
        Scope scope;
        Loc loc;
        std::string rttrName;
        std::string flecsName;
        bool flags = false;
        bool component = false;
        std::vector<std::string> values;
    };

    struct Item
    {
        std::optional<Record> rec;
        std::optional<Enum> en;
    };

    // ── analysis ─────────────────────────────────────────────────────────────────

    struct State
    {
        bool defaultAll = false;
        std::vector<Item> items;
    };

    std::string cppString(const std::string& s)
    {
        std::string r = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\')
                r += '\\';
            r += c;
        }
        return r + "\"";
    }

    bool isReflectedFlagsEnum(CXType t)
    {
        if (t.kind != CXType_Enum)
            return false;
        const auto a = annotation(clang_getTypeDeclaration(t), "reflect:");
        return a && hasOpt(parseOpts(*a), "flags");
    }

    bool flecsCanDescribe(CXType t)
    {
        t = clang_getCanonicalType(t);
        switch (t.kind)
        {
        case CXType_Bool:
        case CXType_Char_S:
        case CXType_Char_U:
        case CXType_SChar:
        case CXType_UChar:
        case CXType_Short:
        case CXType_UShort:
        case CXType_Int:
        case CXType_UInt:
        case CXType_Long:
        case CXType_ULong:
        case CXType_LongLong:
        case CXType_ULongLong:
        case CXType_Float:
        case CXType_Double:
        case CXType_Enum:
            return true;
        case CXType_ConstantArray:
            return flecsCanDescribe(clang_getArrayElementType(t));
        case CXType_Record: // another struct reflected with `meta`
        {
            const auto a = annotation(clang_getTypeDeclaration(t), "reflect:");
            return a && hasOpt(parseOpts(*a), "meta");
        }
        default:
            return false;
        }
    }

    const std::set<std::string_view> kFieldKeys = {
        "min", "max", "step", "decimals", "label", "tooltip", "readonly", "editor",
        "slider", "color", "file", "savefile", "dir", "text", "flags", "name",
    };
    const std::set<std::string_view> kEditorNames = { "default", "file", "savefile", "dir", "color", "text", "slider" };

    void validateFieldHints(const Field& f)
    {
        bool slider = false, hasMin = false, hasMax = false;
        for (const Opt& o : f.hints)
        {
            if (!kFieldKeys.count(o.key))
                error(f.loc, "unknown PROP key '" + o.key + "' on field '" + f.name + "'");
            const bool needsValue = o.key == "min" || o.key == "max" || o.key == "step" || o.key == "decimals"
                                    || o.key == "label" || o.key == "tooltip" || o.key == "editor" || o.key == "name";
            if (needsValue && !o.hasValue)
                error(f.loc, "PROP key '" + o.key + "' needs a value (" + o.key + " = ...)");
            if (o.key == "editor" && o.hasValue && !kEditorNames.count(o.value))
                error(f.loc, "editor = '" + o.value + "' is not one of default|file|savefile|dir|color|text|slider");
            slider |= o.key == "slider" || (o.key == "editor" && o.value == "slider");
            hasMin |= o.key == "min";
            hasMax |= o.key == "max";
        }
        if (slider && !(hasMin && hasMax))
            error(f.loc, "slider on field '" + f.name + "' needs both min and max");
    }

    bool hasReflectFriend(CXCursor rec)
    {
        bool found = false;
        clang_visitChildren(
            rec,
            [](CXCursor c, CXCursor, CXClientData d) {
                if (clang_getCursorKind(c) == CXCursor_FriendDecl)
                {
                    // `template <class> friend struct ::reflect::Access;`
                    bool* f = static_cast<bool*>(d);
                    clang_visitChildren(
                        c,
                        [](CXCursor cc, CXCursor, CXClientData dd) {
                            if (take(clang_getCursorSpelling(cc)) == "Access")
                            {
                                *static_cast<bool*>(dd) = true;
                                return CXChildVisit_Break;
                            }
                            return CXChildVisit_Recurse;
                        },
                        f);
                }
                return CXChildVisit_Continue;
            },
            &found);
        return found;
    }

    void handleRecord(State& st, CXCursor c, const std::string& annot)
    {
        Record r;
        r.loc = locOf(c);
        r.scope = scopeOf(c);
        if (!r.scope.ok)
        {
            error(r.loc, "REFLECT type must be nameable from another file (no anonymous namespace / local / unnamed type)");
            return;
        }
        bool all = st.defaultAll;
        r.rttrName = cppString(r.scope.qualified);
        r.flecsName = "nullptr";
        for (const Opt& o : parseOpts(annot))
        {
            if (o.key == "all")
                all = true;
            else if (o.key == "explicit")
                all = false;
            else if (o.key == "fields" && (o.value == "all" || o.value == "explicit"))
                all = o.value == "all";
            else if (o.key == "component")
                r.component = true;
            else if (o.key == "meta")
                r.meta = true;
            else if (o.key == "name" && o.hasValue)
                r.flecsName = o.value;
            else if (o.key == "rttr_name" && o.hasValue)
                r.rttrName = o.value;
            else
                error(r.loc, "unknown REFLECT key '" + o.key + "' (use all|explicit|component|meta|name=|rttr_name=)");
        }

        const bool friendOk = hasReflectFriend(c);
        int fieldCount = 0;
        bool hasBases = false;

        struct Ctx
        {
            Record* r;
            bool all, friendOk;
            int* fieldCount;
            bool* hasBases;
        } ctx{ &r, all, friendOk, &fieldCount, &hasBases };

        clang_visitChildren(
            c,
            [](CXCursor f, CXCursor, CXClientData d) {
                auto* x = static_cast<Ctx*>(d);
                const CXCursorKind k = clang_getCursorKind(f);
                if (k == CXCursor_CXXBaseSpecifier)
                    *x->hasBases = true;
                if (k != CXCursor_FieldDecl)
                    return CXChildVisit_Continue;
                ++*x->fieldCount;

                Field fld;
                fld.name = take(clang_getCursorSpelling(f));
                fld.loc = locOf(f);
                const auto prop = annotation(f, "prop:");
                const bool no = annotation(f, "noprop:").has_value();
                if (prop && no)
                {
                    error(fld.loc, "field '" + fld.name + "' has both PROP and NOPROP");
                    return CXChildVisit_Continue;
                }
                if (!(x->all ? !no : prop.has_value()))
                    return CXChildVisit_Continue;

                const bool isPublic = clang_getCXXAccessSpecifier(f) == CX_CXXPublic;
                if (!isPublic && !x->friendOk)
                {
                    if (prop) // asked for explicitly → tell the user how to fix it
                        error(fld.loc, "field '" + fld.name + "' is not public; add REFLECT_FRIEND to the class");
                    return CXChildVisit_Continue; // `all` mode: private fields stay private
                }
                if (clang_Cursor_isBitField(f))
                {
                    if (prop)
                        error(fld.loc, "bit-field '" + fld.name + "' cannot be reflected (no pointer-to-member)");
                    return CXChildVisit_Continue;
                }

                const CXType t = clang_getCursorType(f);
                const CXType canon = clang_getCanonicalType(t);
                fld.typeSpelling = take(clang_getTypeSpelling(t));
                if (prop)
                    fld.hints = parseOpts(*prop);
                fld.flagsEnum = isReflectedFlagsEnum(canon);
                fld.flecsMeta = flecsCanDescribe(t);
                CXType elem = canon;
                while (elem.kind == CXType_ConstantArray)
                    elem = clang_getCanonicalType(clang_getArrayElementType(elem));
                if (fld.flecsMeta && elem.kind == CXType_Record)
                    fld.metaDep = "::" + scopeOf(clang_getTypeDeclaration(elem)).qualified;
                const std::string cs = take(clang_getTypeSpelling(canon));
                if (cs.rfind("std::optional<", 0) == 0 && clang_Type_getNumTemplateArguments(canon) == 1)
                    fld.optionalInner = take(clang_getTypeSpelling(
                        clang_getCanonicalType(clang_Type_getTemplateArgumentAsType(canon, 0))));
                validateFieldHints(fld);
                x->r->fields.push_back(std::move(fld));
                return CXChildVisit_Continue;
            },
            &ctx);

        if (hasBases)
            warning(r.loc, "base-class fields of '" + r.scope.qualified + "' are not registered; reflect the base separately");
        r.isTag = fieldCount == 0 && !hasBases;
        Item it;
        it.rec = std::move(r);
        st.items.push_back(std::move(it));
    }

    void handleEnum(State& st, CXCursor c, const std::string& annot)
    {
        Enum e;
        e.loc = locOf(c);
        e.scope = scopeOf(c);
        if (!e.scope.ok)
        {
            error(e.loc, "REFLECT enum must be named and reachable from another file");
            return;
        }
        e.rttrName = cppString(e.scope.qualified);
        e.flecsName = "nullptr";
        for (const Opt& o : parseOpts(annot))
        {
            if (o.key == "flags")
                e.flags = true;
            else if (o.key == "component")
                e.component = true;
            else if (o.key == "name" && o.hasValue)
                e.flecsName = o.value;
            else if (o.key == "rttr_name" && o.hasValue)
                e.rttrName = o.value;
            else
                error(e.loc, "unknown REFLECT key '" + o.key + "' on enum (use flags|component|name=|rttr_name=)");
        }
        clang_visitChildren(
            c,
            [](CXCursor v, CXCursor, CXClientData d) {
                if (clang_getCursorKind(v) == CXCursor_EnumConstantDecl && !annotation(v, "noprop:"))
                    static_cast<Enum*>(d)->values.push_back(take(clang_getCursorSpelling(v)));
                return CXChildVisit_Continue;
            },
            &e);
        Item it;
        it.en = std::move(e);
        st.items.push_back(std::move(it));
    }

    CXChildVisitResult visitTop(CXCursor c, CXCursor, CXClientData d)
    {
        auto* st = static_cast<State*>(d);
        if (!clang_Location_isFromMainFile(clang_getCursorLocation(c)))
            return CXChildVisit_Continue;
        switch (clang_getCursorKind(c))
        {
        case CXCursor_Namespace:
        case CXCursor_LinkageSpec:
            return CXChildVisit_Recurse;
        case CXCursor_StructDecl:
        case CXCursor_ClassDecl:
            if (clang_isCursorDefinition(c))
                if (auto a = annotation(c, "reflect:"))
                    handleRecord(*st, c, *a);
            clang_visitChildren(c, visitTop, d); // nested types
            return CXChildVisit_Continue;
        case CXCursor_ClassTemplate:
            if (annotation(c, "reflect:"))
                error(locOf(c), "REFLECT on a class template is not supported; reflect a concrete type instead");
            return CXChildVisit_Continue;
        case CXCursor_EnumDecl:
            if (clang_isCursorDefinition(c))
                if (auto a = annotation(c, "reflect:"))
                    handleEnum(*st, c, *a);
            return CXChildVisit_Continue;
        default:
            return CXChildVisit_Continue;
        }
    }

    // ── code generation ──────────────────────────────────────────────────────────

    std::string editorConst(const std::string& name)
    {
        static const std::pair<const char*, const char*> map[] = {
            { "default", "Default" }, { "file", "FilePath" },  { "savefile", "SaveFile" }, { "dir", "Directory" },
            { "color", "Color" },     { "text", "Multiline" }, { "slider", "Slider" },
        };
        for (const auto& [k, v] : map)
            if (name == k)
                return std::string("::std::string(::rpe::editor::") + v + ")";
        return "::std::string(::rpe::editor::Default)";
    }

    std::vector<std::string> metadataFor(const Field& f)
    {
        std::vector<std::string> md;
        auto add = [&](const char* key, const std::string& value) {
            // std::string key: RTTR matches const char* keys by address, which differs per DLL
            md.push_back("::rttr::metadata(::std::string(::rpe::hint::" + std::string(key) + "), " + value + ")");
        };
        bool flagsAdded = false;
        for (const Opt& o : f.hints)
        {
            if (o.key == "min")
                add("Min", o.value);
            else if (o.key == "max")
                add("Max", o.value);
            else if (o.key == "step")
                add("Step", o.value);
            else if (o.key == "decimals")
                add("Decimals", o.value);
            else if (o.key == "label") // std::string values: RTTR can't to_string() a const char*
                add("Label", "::std::string(" + o.value + ")");
            else if (o.key == "tooltip")
                add("Tooltip", "::std::string(" + o.value + ")");
            else if (o.key == "readonly")
                add("ReadOnly", o.hasValue ? o.value : "true");
            else if (o.key == "editor")
                add("Editor", editorConst(o.value));
            else if (o.key == "slider" || o.key == "color" || o.key == "file" || o.key == "savefile" || o.key == "dir"
                     || o.key == "text")
                add("Editor", editorConst(o.key));
            else if (o.key == "flags")
            {
                add("Flags", o.hasValue ? o.value : "true");
                flagsAdded = true;
            }
        }
        if (f.flagsEnum && !flagsAdded)
            add("Flags", "true");
        return md;
    }

    std::string propName(const Field& f)
    {
        for (const Opt& o : f.hints)
            if (o.key == "name" && o.hasValue)
                return o.value;
        return cppString(f.name);
    }

    void emitUsing(std::ostringstream& o, const Scope& s)
    {
        if (s.nsParts.empty())
            return;
        std::string ns;
        for (const auto& p : s.nsParts)
            ns += "::" + p;
        o << "            using namespace " << ns << "; // hint values may name constants unqualified\n";
    }

    void emitRecord(std::ostringstream& o, const Record& r, int idx)
    {
        const std::string T = "::" + r.scope.qualified;
        o << "// ── " << r.scope.qualified << "  (line " << r.loc.line << ")\n";
        o << "namespace reflect\n{\n    template <>\n    struct Access<" << T << ">\n    {\n";

        // RTTR
        o << "        static void rttr()\n        {\n";
        emitUsing(o, r.scope);
        o << "            ::rttr::registration::class_<" << T << ">(" << r.rttrName << ")";
        for (const Field& f : r.fields)
        {
            o << "\n                .property(" << propName(f) << ", &" << T << "::" << f.name << ")";
            const auto md = metadataFor(f);
            if (!md.empty())
            {
                o << "(";
                for (size_t i = 0; i < md.size(); ++i)
                    o << (i ? ",\n                    " : "\n                    ") << md[i];
                o << ")";
            }
        }
        o << ";\n        }\n";

        // rpe bridges
        std::set<std::string> optionals;
        for (const Field& f : r.fields)
            if (f.optionalInner)
                optionals.insert(*f.optionalInner);
        const bool bridged = r.component && !r.isTag;
        o << "        static void bridge()\n        {\n#if REFLECT_WITH_RPE\n";
        if (bridged)
            o << "            ::rpe::TypeBridge::registerType<" << T << ">();\n";
        for (const auto& inner : optionals)
            o << "            ::rpe::OptionalBridge::registerType<" << inner << ">();\n";
        o << "#endif\n        }\n";
        o << "        static void unbridge()\n        {\n#if REFLECT_WITH_RPE\n";
        if (bridged)
            o << "            ::rpe::TypeBridge::unregisterType<" << T << ">();\n";
        o << "#endif\n        }\n";

        // flecs: components, and value types described with `meta`
        const bool inFlecs = r.component || r.meta;
        std::set<std::string> deps;
        if (r.meta)
            for (const Field& f : r.fields)
                if (!f.metaDep.empty() && f.metaDep != T)
                    deps.insert(f.metaDep);
        if (inFlecs)
        {
            o << "#if REFLECT_WITH_FLECS\n";
            o << "        static void flecs(::flecs::world& w)\n        {\n";
            o << "            auto c = ::reflect::flecs_register<" << T << ">(w, " << r.flecsName << ", "
              << (bridged ? "true" : "false") << ");\n";
            if (r.meta)
            {
                o << "#ifdef FLECS_META\n            if (::reflect::flecs_needs_members(c))\n            {\n";
                for (const Field& f : r.fields)
                {
                    if (f.flecsMeta)
                        o << "                c.member(" << cppString(f.name) << ", &" << T << "::" << f.name << ");\n";
                    else
                        o << "                // skipped " << f.name << ": flecs has no description for '" << f.typeSpelling
                          << "'\n";
                }
                o << "            }\n#endif\n";
            }
            o << "            (void)c;\n        }\n";
            o << "        static void unflecs(::flecs::world& w)\n        {\n";
            o << "            ::reflect::flecs_unregister<" << T << ">(w);\n        }\n";
            if (!deps.empty())
            {
                o << "        static bool ready(::flecs::world& w)\n        {\n            return ";
                bool first = true;
                for (const auto& d : deps)
                {
                    o << (first ? "" : "\n                && ") << "::reflect::flecs_has_members<" << d << ">(w)";
                    first = false;
                }
                o << ";\n        }\n";
            }
            o << "#endif\n";
        }
        o << "    };\n} // namespace reflect\n\n";

        const std::string A = "::reflect::Access<" + T + ">";
        o << "namespace\n{\n    ::reflect::TypeEntry s_entry" << idx << "{ " << cppString(r.scope.qualified) << ", &" << A
          << "::rttr, &" << A << "::bridge, &" << A << "::unbridge,\n#if REFLECT_WITH_FLECS\n        "
          << (inFlecs ? "&" + A + "::flecs" : "nullptr") << ", " << (inFlecs ? "&" + A + "::unflecs" : "nullptr")
          << ", " << (deps.empty() ? "nullptr" : "&" + A + "::ready") << ",\n#endif\n    };\n    ::reflect::detail::AutoLink s_link" << idx << "{ s_entry" << idx << " };\n}\n\n";
    }

    void emitEnum(std::ostringstream& o, const Enum& e, int idx)
    {
        const std::string T = "::" + e.scope.qualified;
        o << "// ── enum " << e.scope.qualified << "  (line " << e.loc.line << ")\n";
        o << "namespace reflect\n{\n    template <>\n    struct Access<" << T << ">\n    {\n";
        o << "        static void rttr()\n        {\n";
        o << "            ::rttr::registration::enumeration<" << T << ">(" << e.rttrName << ")(";
        for (size_t i = 0; i < e.values.size(); ++i)
            o << (i ? "," : "") << "\n                ::rttr::value(" << cppString(e.values[i]) << ", " << T
              << "::" << e.values[i] << ")";
        o << ");\n        }\n";
        o << "        static void bridge()\n        {\n#if REFLECT_WITH_RPE\n";
        if (e.flags)
            o << "            ::rpe::FlagsBridge::registerType<" << T << ">();\n";
        o << "#endif\n        }\n";
        if (e.component)
        {
            o << "#if REFLECT_WITH_FLECS\n        static void flecs(::flecs::world& w)\n        {\n";
            o << "            ::reflect::flecs_register<" << T << ">(w, " << e.flecsName << ", false);\n        }\n";
            o << "        static void unflecs(::flecs::world& w)\n        {\n";
            o << "            ::reflect::flecs_unregister<" << T << ">(w);\n        }\n#endif\n";
        }
        o << "    };\n} // namespace reflect\n\n";

        const std::string A = "::reflect::Access<" + T + ">";
        o << "namespace\n{\n    ::reflect::TypeEntry s_entry" << idx << "{ " << cppString(e.scope.qualified) << ", &" << A
          << "::rttr, &" << A << "::bridge, nullptr,\n#if REFLECT_WITH_FLECS\n        "
          << (e.component ? "&" + A + "::flecs" : "nullptr") << ", " << (e.component ? "&" + A + "::unflecs" : "nullptr")
          << ", nullptr,\n#endif\n    };\n    ::reflect::detail::AutoLink s_link" << idx << "{ s_entry" << idx << " };\n}\n\n";
    }

    std::string generate(const State& st, const std::string& includeSpelling, const std::string& source)
    {
        std::ostringstream o;
        o << "// Generated by reflectgen from " << source << " - DO NOT EDIT.\n";
        o << "#include " << includeSpelling << "\n";
        o << "#include <reflect/Runtime.h>\n\n";
        int idx = 0;
        for (const Item& it : st.items)
        {
            if (it.rec)
                emitRecord(o, *it.rec, idx++);
            else
                emitEnum(o, *it.en, idx++);
        }
        if (st.items.empty())
            o << "// (no REFLECT types in this header)\n";
        return o.str();
    }

    bool writeIfChanged(const fs::path& p, const std::string& content)
    {
        {
            std::ifstream in(p, std::ios::binary);
            if (in)
            {
                std::stringstream ss;
                ss << in.rdbuf();
                if (ss.str() == content)
                    return true;
            }
        }
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << content;
        return static_cast<bool>(out);
    }

    std::string depEscape(std::string s)
    {
        std::string r;
        for (char c : s)
        {
            if (c == ' ' || c == '#')
                r += '\\';
            if (c == '$')
                r += '$';
            r += c;
        }
        return r;
    }

    int usage()
    {
        std::fprintf(stderr,
                     "usage: reflectgen <header> -o <out.cpp> [--depfile <file>] [--include <spelling>]\n"
                     "                  [--default-fields explicit|all] [--allow-errors] -- <compiler args>\n");
        return 2;
    }

} // namespace

int main(int argc, char** argv)
{
    std::string header, out, depfile, include;
    bool allowErrors = false;
    State st;
    std::vector<std::string> cargs;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--")
        {
            for (++i; i < argc; ++i)
                cargs.emplace_back(argv[i]);
            break;
        }
        if (a == "-o")
            out = next();
        else if (a == "--depfile")
            depfile = next();
        else if (a == "--include")
            include = next();
        else if (a == "--default-fields")
            st.defaultAll = next() == "all";
        else if (a == "--allow-errors")
            allowErrors = true;
        else if (!a.empty() && a[0] == '-')
            return usage();
        else
            header = a;
    }
    if (header.empty() || out.empty())
        return usage();

    const bool clMode = std::find(cargs.begin(), cargs.end(), "--driver-mode=cl") != cargs.end();
    // Treat the header as C++ and switch the annotations on.
    if (clMode)
    {
        cargs.emplace_back("/TP");
        cargs.emplace_back("/DREFLECT_PARSE");
        // The MSVC STL #errors on Clang versions it was not released with; we only
        // need the declarations, so don't let the installed LLVM's age block parsing.
        cargs.emplace_back("/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH");
    }
    else
    {
        cargs.insert(cargs.begin(), { "-x", "c++" });
        cargs.emplace_back("-DREFLECT_PARSE");
    }
    cargs.emplace_back("-Wno-pragma-once-outside-header");
    std::vector<const char*> argp;
    for (const auto& s : cargs)
        argp.push_back(s.c_str());

    CXIndex index = clang_createIndex(/*excludePCH*/ 0, /*displayDiagnostics*/ 0);
    CXTranslationUnit tu = nullptr;
    const unsigned flags = CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing;
    const CXErrorCode ec = clang_parseTranslationUnit2(index, header.c_str(), argp.data(), static_cast<int>(argp.size()),
                                                       nullptr, 0, flags, &tu);
    if (ec != CXError_Success || !tu)
    {
        std::fprintf(stderr, "reflectgen: failed to parse %s (libclang error %d)\n", header.c_str(), static_cast<int>(ec));
        return 1;
    }

    int parseErrors = 0;
    for (unsigned i = 0, n = clang_getNumDiagnostics(tu); i < n; ++i)
    {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error)
        {
            ++parseErrors;
            std::fprintf(stderr, "%s\n",
                         take(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions())).c_str());
        }
        clang_disposeDiagnostic(d);
    }
    if (parseErrors && !allowErrors)
    {
        std::fprintf(stderr, "reflectgen: %d parse error(s) in %s (pass --allow-errors to generate anyway)\n",
                     parseErrors, header.c_str());
        return 1;
    }

    clang_visitChildren(clang_getTranslationUnitCursor(tu), visitTop, &st);
    if (g_errors)
        return 1;

    const fs::path hdr = fs::absolute(header);
    if (include.empty())
        include = "\"" + hdr.generic_string() + "\"";
    const std::string code = generate(st, include, hdr.filename().generic_string());
    if (!writeIfChanged(out, code))
    {
        std::fprintf(stderr, "reflectgen: cannot write %s\n", out.c_str());
        return 1;
    }

    if (!depfile.empty())
    {
        std::vector<std::string> deps;
        clang_getInclusions(
            tu,
            [](CXFile f, CXSourceLocation*, unsigned, CXClientData d) {
                // libclang may report "/../lib/gcc/..."-style paths; Ninja would
                // normalise those lexically into files that don't exist and rerun
                // us forever. Resolve them against the real filesystem instead.
                std::error_code ec;
                const fs::path p = fs::weakly_canonical(fs::path(take(clang_getFileName(f))), ec);
                if (!ec && fs::exists(p, ec))
                    static_cast<std::vector<std::string>*>(d)->push_back(p.generic_string());
            },
            &deps);
        std::string dep = depEscape(fs::path(out).generic_string()) + ":";
        for (const auto& p : deps)
            dep += " \\\n  " + depEscape(p);
        dep += "\n";
        writeIfChanged(depfile, dep);
    }

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 0;
}
