#ifndef SHASHIN_HELPER_H
#define SHASHIN_HELPER_H

#include <type_traits>
#include <utility>
#include "shashin_types.h"

namespace ShashChess {

// =============================================
// Template Metaprogramming Utilities
// =============================================

template<typename... Ts>
using are_all_shashin = std::conjunction<std::is_same<Ts, ShashinPosition>...>;

// =============================================
// Core Bitmask Operations (Type-Safe)
// =============================================

template<typename... Args>
constexpr bool isInRanges(ShashinPosition value, Args... ranges) {
    static_assert((std::is_same_v<ShashinPosition, Args> && ...), 
                  "All ranges must be ShashinPosition");

    // Verifica se value è COMPLETAMENTE contenuto in almeno uno dei range
    return (( (value & ranges) == ranges ) || ...);
}

// =============================================
// Enum Utilities
// =============================================

template<typename Enum>
constexpr auto to_underlying(Enum e) noexcept -> std::underlying_type_t<Enum> {
    return static_cast<std::underlying_type_t<Enum>>(e);
}

// =============================================
// Bitwise Operator Overloads
// =============================================

inline constexpr ShashinPosition operator|(ShashinPosition lhs, ShashinPosition rhs) noexcept {
    return static_cast<ShashinPosition>(
        to_underlying(lhs) | to_underlying(rhs)
    );
}

inline constexpr ShashinPosition operator&(ShashinPosition lhs, ShashinPosition rhs) noexcept {
    return static_cast<ShashinPosition>(
        to_underlying(lhs) & to_underlying(rhs)
    );
}

inline constexpr ShashinPosition operator~(ShashinPosition v) noexcept {
    return static_cast<ShashinPosition>(~to_underlying(v));
}

inline constexpr ShashinPosition& operator|=(ShashinPosition& lhs, ShashinPosition rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

inline constexpr ShashinPosition& operator&=(ShashinPosition& lhs, ShashinPosition rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

// =============================================
// Advanced Pattern Matching
// =============================================

template<typename Container>
constexpr bool matchesAnyPattern(ShashinPosition value, const Container& patterns) noexcept {
    using PatternT = typename Container::value_type;
    static_assert(std::is_same_v<PatternT, ShashinPosition>, 
                 "Container must hold ShashinPosition values");
    
    for (auto pattern : patterns) {
        if ((value & pattern) == pattern) return true;
    }
    return false;
}

} // namespace ShashChess

#endif // SHASHIN_HELPER_H