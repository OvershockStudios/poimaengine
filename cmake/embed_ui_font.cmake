# SPDX-License-Identifier: Apache-2.0
file(READ "${INPUT}" font_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," font_values "${font_hex}")
file(WRITE "${OUTPUT}" "// Generated from the pinned Inter font. Redistribution notice: third_party/inter/LICENSE.txt.\n#include <cstdint>\n#include <span>\nnamespace poima {\nstd::span<const std::uint8_t> embedded_ui_font() {\nstatic constexpr std::uint8_t bytes[]={${font_values}};\nreturn bytes;\n}\n}\n")
