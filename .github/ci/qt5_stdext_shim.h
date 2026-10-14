// CI only (force-included with /FI): Qt 5.15.2's qcompilerdetection.h maps
// QT_MAKE_(UN)CHECKED_ARRAY_ITERATOR to stdext::make_(un)checked_array_iterator
// unconditionally on MSVC, and the MSVC 14.5x STL removed those. A plain pointer
// is what they reduce to in a release build.
#pragma once
#ifdef __cplusplus
#include <cstddef>
namespace stdext
{
    template <class T>
    constexpr T* make_checked_array_iterator(T* p, std::size_t, std::size_t = 0)
    {
        return p;
    }
    template <class T>
    constexpr T* make_unchecked_array_iterator(T* p)
    {
        return p;
    }
} // namespace stdext
#endif
