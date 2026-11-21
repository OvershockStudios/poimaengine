# SPDX-License-Identifier: Apache-2.0
file(READ "${INPUT}" wordmark_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," wordmark_values "${wordmark_hex}")
file(WRITE "${OUTPUT}" "// Generated from the Poima wordmark. Font notice: docs/branding/Playball-OFL.txt.\nstatic constexpr unsigned char startup_wordmark_png[]={${wordmark_values}};\n")
