// SPDX-License-Identifier: Apache-2.0
#include "development_diagnostics.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace poima::development {
namespace {
constexpr std::size_t input_limit=65536,line_limit=16384;
bool space(char c) { return c==' ' || c=='\t' || c=='\r'; }
std::string_view trim(std::string_view text) {
    while(!text.empty() && space(text.front()))text.remove_prefix(1);
    while(!text.empty() && space(text.back()))text.remove_suffix(1);
    return text;
}
bool alnum(char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9'); }
char lower(char c) { return c>='A' && c<='Z' ? static_cast<char>(c-'A'+'a') : c; }
bool same(std::string_view text,std::string_view expected) {
    return text.size()==expected.size() && std::equal(text.begin(),text.end(),expected.begin(),[](char a,char b){return lower(a)==b;});
}
bool project_annotation(std::string_view value) {
    auto path=value;
    if(const auto metadata=value.find("::");metadata!=std::string_view::npos) {
        const auto suffix=value.substr(metadata+2);
        constexpr std::string_view prefix="TargetFramework=";
        if(!suffix.starts_with(prefix) || suffix.size()==prefix.size())return false;
        const auto framework=suffix.substr(prefix.size());
        if(!std::all_of(framework.begin(),framework.end(),[](char c){return alnum(c) || c=='.' || c=='_' || c=='-';}))return false;
        path=value.substr(0,metadata);
    }
    const auto dot=path.rfind('.');if(dot==std::string_view::npos)return false;
    const auto extension=path.substr(dot);
    for(const auto candidate:{".csproj",".vbproj",".fsproj",".proj",".vcxproj",".vcproj",".shproj",".sqlproj",".wixproj",".esproj"})
        if(same(extension,candidate))return true;
    return false;
}
std::size_t project_suffix(std::string_view message) {
    if(message.empty() || message.back()!=']')return std::string_view::npos;
    std::size_t depth=1;
    // Find the outer annotation, preserving balanced brackets within its path.
    // A last " [" search would incorrectly choose a nested filename bracket.
    for(std::size_t at=message.size()-1;at>0;) {
        const char c=message[--at];
        if(c==']')++depth;
        else if(c=='[' && --depth==0) {
            if(at==0 || !space(message[at-1]))return std::string_view::npos;
            // An unmatched opening bracket before this candidate makes it
            // ambiguous: it may be an inner fragment of an unbalanced path.
            // Keep the complete message rather than inventing a shorter path.
            std::size_t unclosed=0;
            for(const char prefix:message.substr(0,at)) {
                if(prefix=='[')++unclosed;
                else if(prefix==']' && unclosed>0)--unclosed;
            }
            return unclosed==0 ? at : std::string_view::npos;
        }
    }
    return std::string_view::npos;
}
bool location(std::string_view& origin,std::optional<Location>& result) {
    if(origin.empty() || origin.back()!=')')return true;
    const auto open=origin.rfind('(');if(open==std::string_view::npos)return true;
    auto values=trim(origin.substr(open+1,origin.size()-open-2));
    // Parentheses in a path are not automatically source coordinates.
    if(values.empty())return true;
    if(!(values.front()>='0' && values.front()<='9') && values.front()!='-' && values.front()!='+' && values.find(',')==std::string_view::npos)return true;
    std::array<std::uint32_t,4> coordinates{};std::size_t count=0;
    while(!values.empty()) {
        if(count==coordinates.size())return false;
        const auto comma=values.find(',');const auto value=trim(values.substr(0,comma));
        if(value.empty())return false;
        std::uint32_t number=0;
        for(const char c:value) {
            if(c<'0' || c>'9')return false;
            const auto digit=static_cast<std::uint32_t>(c-'0');
            if(number>(std::numeric_limits<std::uint32_t>::max()-digit)/10)return false;
            number=number*10+digit;
        }
        if(number==0)return false;
        coordinates[count++]=number;
        if(comma==std::string_view::npos)break;
        values.remove_prefix(comma+1);if(values.empty())return false;
    }
    if(count!=1 && count!=2 && count!=4)return false;
    Location parsed;parsed.line=coordinates[0];
    if(count>=2)parsed.column=coordinates[1];
    if(count==4) {
        parsed.end_line=coordinates[2];parsed.end_column=coordinates[3];
        if(coordinates[2]<coordinates[0] || (coordinates[2]==coordinates[0] && coordinates[3]<coordinates[1]))return false;
    }
    origin=trim(origin.substr(0,open));if(origin.empty())return false;
    result=parsed;return true;
}
std::optional<Diagnostic> parse_line(std::string_view line,bool& malformed) {
    line=trim(line);std::size_t colon=std::string_view::npos;bool subcategory_valid=true;
    for(std::size_t at=0;at<line.size();) {
        if(line[at]==':') { colon=at++;subcategory_valid=true;continue; }
        if(!alnum(line[at])) {
            if(!space(line[at]) && line[at]!='_' && line[at]!='-' && line[at]!='.')subcategory_valid=false;
            ++at;continue;
        }
        const auto start=at;while(at<line.size() && alnum(line[at]))++at;
        const auto token=line.substr(start,at-start);
        const bool error=same(token,"error"),warning=same(token,"warning");
        if(!error && !warning)continue;
        // A category must be a whitespace-delimited token, not part of a path.
        if((start>0 && !space(line[start-1]) && line[start-1]!=':') || at==line.size() || !space(line[at]))continue;
        const bool eligible=colon!=std::string_view::npos ? subcategory_valid : start==0;
        while(at<line.size() && space(line[at]))++at;
        const auto code_start=at;
        while(at<line.size() && (alnum(line[at]) || line[at]=='_' || line[at]=='-' || line[at]=='.'))++at;
        const auto code=line.substr(code_start,at-code_start);
        while(at<line.size() && space(line[at]))++at;
        if(code.empty() || code.size()>128 || at==line.size() || line[at]!=':') {
            malformed=true;if(eligible)return {};continue;
        }
        if(!eligible) { malformed=true;continue; }
        auto origin=std::string_view{};
        if(colon!=std::string_view::npos)origin=trim(line.substr(0,colon));
        Diagnostic result;result.severity=error?Severity::error:Severity::warning;
        if(!location(origin,result.location)) { malformed=true;return {}; }
        result.origin=origin;result.code=code;
        auto message=trim(line.substr(at+1));
        if(!message.empty() && message.back()==']') {
            const auto project=project_suffix(message);
            if(project!=std::string_view::npos) {
                const auto value=trim(message.substr(project+1,message.size()-project-2));
                if(project_annotation(value)) { result.project=value;message=trim(message.substr(0,project)); }
            }
        }
        result.message=message;malformed=false;return result;
    }
    return {};
}
std::string key(const Diagnostic& value) {
    std::string result=value.severity==Severity::error?"e":"w";
    auto field=[&](const std::string& text){result+=std::to_string(text.size());result+=':';result+=text;};
    field(value.code);field(value.origin);field(value.message);field(value.project);
    if(value.location) {
        result+='L';result+=std::to_string(value.location->line);
        for(const auto coordinate:{value.location->column,value.location->end_line,value.location->end_column}) {
            result+=':';if(coordinate)result+=std::to_string(*coordinate);
        }
    }
    return result;
}
}
Report parse_diagnostics(std::string_view stdout_tail,bool stdout_truncated,
                         std::string_view stderr_tail,bool stderr_truncated,
                         bool terminal,std::size_t limit) {
    if(limit<1 || limit>128)throw std::invalid_argument("Diagnostic result limit must be 1..128.");
    Report report;std::set<std::string> seen;
    auto stream=[&](std::string_view text,bool truncated) {
        if(text.size()>input_limit) { text.remove_prefix(text.size()-input_limit);truncated=true; }
        if(truncated) {
            report.incomplete=true;
            const auto end=text.find('\n');if(end==std::string_view::npos)return;
            text.remove_prefix(end+1);
        }
        while(!text.empty()) {
            const auto end=text.find('\n');
            if(end==std::string_view::npos && !terminal) { report.incomplete=true;return; }
            const auto line=text.substr(0,end);
            if(line.size()>line_limit)report.incomplete=true;
            else {
                bool malformed=false;auto diagnostic=parse_line(line,malformed);
                report.incomplete=report.incomplete || malformed;
                if(diagnostic) {
                    ++report.recognized_count;
                    if(seen.insert(key(*diagnostic)).second) {
                        if(report.diagnostics.size()<limit)report.diagnostics.push_back(std::move(*diagnostic));
                        else report.more=true;
                    }
                }
            }
            if(end==std::string_view::npos)return;
            text.remove_prefix(end+1);
        }
    };
    stream(stdout_tail,stdout_truncated);stream(stderr_tail,stderr_truncated);return report;
}
}
