#pragma once
#include <reflect/Reflect.h>
// Every REFLECT misuse here must be a reflectgen error (see diag/expect.cmake).
struct REFLECT(component, bogus) A
{
    PROP(slider, max = 10) float a;
    PROP(colour) int b;
    PROP(setter = setB) int c;
private:
    PROP() int d;
};
template <class T> struct REFLECT() Tpl { T v; };
struct REFLECT() PBase { private: PROP() int secret = 0; };
struct REFLECT() PDerived : PBase { REFLECT_FRIEND };
struct REFLECT() Acc
{
    PROP(setter = set) int get() const { return 0; }
    void set(int) {}
    void set(float) {}
    PROP() int notConst() { return 0; }
};
