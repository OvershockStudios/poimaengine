// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poima::development {
enum class Severity { error,warning };
struct Location {
    std::uint32_t line=0;
    std::optional<std::uint32_t> column,end_line,end_column;
};
struct Diagnostic {
    Severity severity=Severity::error;
    std::string code,origin,message,project;
    std::optional<Location> location;
};
struct Report {
    std::vector<Diagnostic> diagnostics;
    // Matching physical lines before exact-record deduplication.
    std::size_t recognized_count=0;
    bool more=false,incomplete=false;
};
// Conservative MSBuild/C# diagnostic subset, never a compile-success verdict.
// Recognizes error/warning + code, optional origin/subcategory, and positive
// (line), (line,column), or (line,column,end_line,end_column) locations.
// A final " [project]" suffix is interpreted as a project annotation only for
// recognized project-file extensions, optionally with ::TargetFramework=...
// Other bracketed message content is preserved.
// Exact normalized records are deduplicated in stdout-then-stderr order.
// Input is capped at 64 KiB per stream and lines at 16 KiB. Truncated leading
// fragments and running unterminated final lines are withheld. Terminal final
// lines may omit a newline. These omissions and malformed diagnostic-looking
// lines set incomplete; additional distinct records beyond limit set more.
// Fields retain output bytes: JSON callers must decode/replace invalid UTF-8.
// limit must be 1..128; invalid limits throw std::invalid_argument.
Report parse_diagnostics(std::string_view stdout_tail,bool stdout_truncated,
                         std::string_view stderr_tail,bool stderr_truncated,
                         bool terminal,std::size_t limit=32);
}
