#pragma once
#include <bolt/detail/Utf8Block.h>

namespace bolt::unicode::detail {
inline constexpr bool has_utf8_blocks = BOLT_HAS_UTF8_BLOCKS != 0;
}
