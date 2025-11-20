#ifndef ENUM_UTILS_H
#define ENUM_UTILS_H

#include <ranges>
#include <type_traits>

// Concept to ensure this only works on Enums that have a kCount member
template <typename T>
concept CountableEnum = std::is_enum_v<T> && requires { T::kCount; };

template <CountableEnum E>
constexpr auto get_enum_range() {
    // 1. Create an integer sequence from 0 to kCount
    // 2. Transform (cast) each integer back to the Enum type
    return std::views::iota(0, static_cast<int>(E::kCount)) 
         | std::views::transform([](int i) { return static_cast<E>(i); });
}

#endif