// SPDX-License-Identifier: Apache-2.0
#include "development_diagnostics.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace dev=poima::development;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
dev::Report parse(const std::string& output,bool terminal=true,std::size_t limit=32) {
    return dev::parse_diagnostics(output,false,{},false,terminal,limit);
}
void fields_and_summary() {
    const std::string warning="/work/Diagnostic Path (A)/Broken.cs(1,10): warning CS1030: #warning: 'Fixture warning' [/work/Diagnostic Path (A)/Broken.csproj]";
    const std::string error="/work/Diagnostic Path (A)/Broken.cs(2,57): error CS1525: Invalid expression term ';' [/work/Diagnostic Path (A)/Broken.csproj]";
    const auto report=dev::parse_diagnostics("Restoring...\n"+warning+"\n"+error+"\nBuild FAILED.\n"+warning+"\n",false,error+"\n",false,true);
    check(report.recognized_count==4 && report.diagnostics.size()==2 && !report.more && !report.incomplete,"Summary diagnostics were not exactly deduplicated.");
    const auto& first=report.diagnostics[0];
    check(first.severity==dev::Severity::warning && first.code=="CS1030" && first.origin=="/work/Diagnostic Path (A)/Broken.cs","Linux origin/category/code changed.");
    check(first.location && first.location->line==1u && first.location->column==10u && !first.location->end_line && !first.location->end_column,"Point location changed.");
    check(first.message=="#warning: 'Fixture warning'" && first.project=="/work/Diagnostic Path (A)/Broken.csproj","Message/project fields changed.");
    const auto windows=parse("C:\\Source warning tree\\Files (A)\\\xCE\x94.cs(4,5,6,7): ERROR CS1002: \xE7\xBC\xBA\xE5\xB0\x91 ';' [C:\\Projects (A)\\Game.csproj]\r\n");
    check(windows.diagnostics.size()==1 && !windows.incomplete,"Windows path/uppercase/UTF-8 diagnostic was not recognized.");
    const auto& win=windows.diagnostics[0];
    check(win.origin=="C:\\Source warning tree\\Files (A)\\\xCE\x94.cs" && win.location->line==4u && win.location->column==5u && win.location->end_line==6u && win.location->end_column==7u,"Windows span or origin changed.");
    check(win.message=="\xE7\xBC\xBA\xE5\xB0\x91 ';'" && win.project=="C:\\Projects (A)\\Game.csproj","Unicode/message/project changed.");
    const auto line=parse("file.cs(9): warning CS0168: Unused\n");
    check(line.diagnostics.size()==1 && line.diagnostics[0].location->line==9u && !line.diagnostics[0].location->column,"Line-only location invented a column.");
    const auto tool=parse("CSC : error CS5001: Missing entry point\ncl : Command line Warning D4024 : Unknown file\nerror CS0006: Metadata file not found.\n");
    check(tool.diagnostics.size()==3 && tool.diagnostics[0].origin=="CSC" && !tool.diagnostics[0].location && tool.diagnostics[1].origin=="cl" && tool.diagnostics[1].code=="D4024" && tool.diagnostics[2].origin.empty(),"Tool, subcategory or blank origin failed.");
    const auto parenthesized=parse("/work/folder (notes)/File (copy).cs: warning CS001: Message [literal content]\n");
    check(parenthesized.diagnostics.size()==1 && !parenthesized.diagnostics[0].location,"Path parentheses became coordinates.");
    check(parenthesized.diagnostics[0].message=="Message [literal content]" && parenthesized.diagnostics[0].project.empty(),"Bracketed message content was stripped as a project.");
    const auto attribute=parse("f.cs(1,2): error CS1: Invalid attribute [Foo]\n");
    check(attribute.diagnostics.size()==1 && attribute.diagnostics[0].message=="Invalid attribute [Foo]" && attribute.diagnostics[0].project.empty(),"Attribute brackets became project metadata.");
    const auto multi=parse("f.cs(1,2): warning CS1: Message [C:\\Game.CSPROJ::TargetFramework=net10.0]\n");
    check(multi.diagnostics.size()==1 && multi.diagnostics[0].message=="Message" && multi.diagnostics[0].project=="C:\\Game.CSPROJ::TargetFramework=net10.0","Multitarget annotation was lost.");
    const auto bracketed=parse("f.cs(1,2): error CS1: Bad [/work/Game [special].csproj]\n");
    check(bracketed.diagnostics.size()==1 && bracketed.diagnostics[0].message=="Bad" && bracketed.diagnostics[0].project=="/work/Game [special].csproj","Nested project brackets corrupted message or path.");
    const auto nested=parse("f.cs(1,2): error CS1: Bad [Foo] [C:\\Folder [one [two]]\\Game.csproj::TargetFramework=net10.0]\n");
    check(nested.diagnostics.size()==1 && nested.diagnostics[0].message=="Bad [Foo]" && nested.diagnostics[0].project=="C:\\Folder [one [two]]\\Game.csproj::TargetFramework=net10.0","Nested Windows project brackets or message brackets were stripped.");
    const auto ambiguous=parse("f.cs(1,2): error CS1: Invalid attribute [Foo [Bar]]\n");
    check(ambiguous.diagnostics.size()==1 && ambiguous.diagnostics[0].message=="Invalid attribute [Foo [Bar]]" && ambiguous.diagnostics[0].project.empty(),"Nested message brackets became a project.");
    const auto unbalanced=parse("f.cs(1,2): error CS1: Bad [/work/Game [special.csproj]\n");
    check(unbalanced.diagnostics.size()==1 && unbalanced.diagnostics[0].message=="Bad [/work/Game [special.csproj]" && unbalanced.diagnostics[0].project.empty(),"Unbalanced project-looking text was silently corrupted.");
}
void omission_and_limits() {
    const std::string one="file.cs(1,2): error CS1002: Missing semicolon";
    check(parse(one,false).diagnostics.empty() && parse(one,false).incomplete,"Running partial line was published.");
    check(parse(one,true).diagnostics.size()==1 && !parse(one,true).incomplete,"Terminal unterminated diagnostic was lost.");
    auto clipped=dev::parse_diagnostics(one+"\n"+one+"\n",true,{},false,true);
    check(clipped.incomplete && clipped.recognized_count==1 && clipped.diagnostics.size()==1,"Truncated prefix was interpreted as a full line.");
    clipped=dev::parse_diagnostics(one,true,{},false,true);check(clipped.incomplete && clipped.diagnostics.empty(),"Entire truncated fragment was published.");
    const auto duplicates=parse(one+"\n"+one+"\n",true,1);
    check(duplicates.recognized_count==2 && duplicates.diagnostics.size()==1 && !duplicates.more,"Duplicate filled result limit was called overflow.");
    const auto overflow=parse(one+"\nother.cs(1,2): error CS1002: Missing semicolon\nother.cs(1,2): error CS1002: Missing semicolon\n",true,1);
    check(overflow.recognized_count==3 && overflow.diagnostics.size()==1 && overflow.more,"Distinct result overflow was not explicit.");
    const auto oversized=parse(std::string(16385,'x')+"\n"+one+"\n");
    check(oversized.incomplete && oversized.diagnostics.size()==1,"Oversized line was parsed or poisoned following line.");
    const auto input=parse(std::string(70000,'x')+"\n"+one+"\n");
    check(input.incomplete && input.diagnostics.size()==1,"Input cap failed to retain bounded tail diagnostics.");
    for(const auto limit:{std::size_t(0),std::size_t(129)}) {
        bool rejected=false;try { parse(one,true,limit); }catch(const std::invalid_argument&) { rejected=true; }
        check(rejected,"Unbounded/empty result limit accepted.");
    }
    // Message/project/location remain part of the key, never code alone.
    const auto separate=parse("a.cs(1,1): error CS1: First [A.csproj]\na.cs(1,1): error CS1: First [B.csproj]\na.cs(2,1): error CS1: First [A.csproj]\na.cs(1,1): error CS1: Second [A.csproj]\n");
    check(separate.diagnostics.size()==4,"Distinct same-code diagnostics collapsed.");
}
void malformed_and_plain_logs() {
    for(const auto* output:{"file.cs(0,2): error CS1: Bad\n","file.cs(1,x): error CS1: Bad\n","file.cs(1,2,3): error CS1: Bad\n",
        "file.cs(4294967296,2): error CS1: Bad\n","file.cs(2,4,1,7): error CS1: Bad\n","file.cs(1,): error CS1: Bad\n",
        "file.cs(1,2): error : Missing code\n","file.cs(1,2): warning CS1 Missing colon\n",
        "file.cs(1,2): error invalid code message: warning CS2: Not a second diagnostic\n"}) {
        const auto report=parse(output);check(report.diagnostics.empty() && report.incomplete,"Malformed compiler-like line produced a diagnostic or complete report.");
    }
    const auto logs=parse("Determining projects to restore...\nBuild FAILED.\n    1 Warning(s)\n    1 Error(s)\nTime Elapsed 00:00:13.45\n");
    check(logs.diagnostics.empty() && logs.recognized_count==0 && !logs.more,"Plain logs became fake diagnostics.");
    const std::uint8_t invalid_bits=0xff;
    char invalid_byte=0;
    std::memcpy(&invalid_byte,&invalid_bits,1);
    const auto bytes=parse(std::string("f.cs(1,1): error CS1: bad byte ")+invalid_byte+"\n");
    check(bytes.diagnostics.size()==1 && bytes.diagnostics[0].message.back()==invalid_byte,"Raw diagnostic bytes were silently rewritten.");
    const auto path=parse("/work/error cases/warning files/a.cs(1,2): error CS1: Bad\n");
    check(path.diagnostics.size()==1 && path.diagnostics[0].origin=="/work/error cases/warning files/a.cs" && !path.incomplete,"Severity words in paths confused parsing.");
}
void sdk_log(const char* path) {
    std::ifstream file(path,std::ios::binary);check(bool(file),"Cannot read private SDK fixture.");
    const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    const auto report=parse(text);
    check(report.recognized_count==4 && report.diagnostics.size()==2 && !report.more && !report.incomplete,"Real failing SDK log differed from expected diagnostic pair.");
    check(report.diagnostics[0].code=="CS1030" && report.diagnostics[0].location->line==1u && report.diagnostics[0].location->column==10u && report.diagnostics[1].code=="CS1525" && report.diagnostics[1].location->line==2u && report.diagnostics[1].location->column==57u,"Real SDK field extraction failed.");
    check(!report.diagnostics[0].origin.empty() && !report.diagnostics[0].project.empty(),"Real SDK paths were lost.");
}
}
int main(int argc,char** argv) {
    try {
        fields_and_summary();omission_and_limits();malformed_and_plain_logs();
        if(argc==3 && std::string(argv[1])=="--verify-sdk")sdk_log(argv[2]);
        else check(argc==1,"Use no arguments or --verify-sdk <private stdout log>.");
        std::cout<<"MSBuild/C# diagnostic subset: fields, spans, literal paths, UTF-8 bytes, duplicates, omissions, caps and malformed lines passed.\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
