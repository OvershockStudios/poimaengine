// SPDX-License-Identifier: Apache-2.0
#include "poima/editor_style.hpp"
#include "poima/editor_fonts.hpp"
#include <imgui.h>
#include <stdexcept>
#include <cstdio>
namespace poima {
void configure_editor_style() {
    auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly=true;io.IniFilename=nullptr;
    ImFontConfig config;config.FontDataOwnedByAtlas=false;config.OversampleH=3;config.OversampleV=2;config.PixelSnapH=false;
    std::snprintf(config.Name,sizeof(config.Name),"Source Sans 3 Regular");
    if(!io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(source_sans_Regular),static_cast<int>(sizeof(source_sans_Regular)),16.f,&config))throw std::runtime_error("Cannot load Source Sans 3 regular.");
    std::snprintf(config.Name,sizeof(config.Name),"Source Sans 3 Semibold");
    if(!io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(source_sans_Semibold),static_cast<int>(sizeof(source_sans_Semibold)),16.f,&config))throw std::runtime_error("Cannot load Source Sans 3 semibold.");
    auto& s=ImGui::GetStyle();ImGui::StyleColorsDark();
    s.WindowPadding={8,7};s.FramePadding={5,2};s.ItemSpacing={6,5};s.ItemInnerSpacing={5,4};s.IndentSpacing=15;
    s.WindowRounding=2;s.ChildRounding=0;s.FrameRounding=2;s.PopupRounding=3;s.ScrollbarRounding=3;s.GrabRounding=1;s.TabRounding=0;
    s.WindowBorderSize=1;s.ChildBorderSize=1;s.FrameBorderSize=0;s.PopupBorderSize=1;s.TabBorderSize=0;
    s.ScrollbarSize=11;s.GrabMinSize=8;s.DockingSeparatorSize=3;s.WindowMinSize={160,100};
    s.AntiAliasedLines=true;s.AntiAliasedLinesUseTex=true;s.AntiAliasedFill=true;
    auto rgba=[](int r,int g,int b,float a=1.f){return ImVec4(static_cast<float>(r)/255.f,static_cast<float>(g)/255.f,static_cast<float>(b)/255.f,a);};auto& c=s.Colors;
    c[ImGuiCol_Text]=rgba(211,211,211);c[ImGuiCol_TextDisabled]=rgba(133,133,133);
    c[ImGuiCol_WindowBg]=rgba(45,45,45);c[ImGuiCol_ChildBg]=rgba(45,45,45);c[ImGuiCol_PopupBg]=rgba(48,48,48);
    c[ImGuiCol_Border]=rgba(26,26,26);c[ImGuiCol_BorderShadow]=rgba(0,0,0,0);
    c[ImGuiCol_FrameBg]=rgba(31,31,31);c[ImGuiCol_FrameBgHovered]=rgba(60,60,60);c[ImGuiCol_FrameBgActive]=rgba(66,66,66);
    c[ImGuiCol_TitleBg]=rgba(38,38,38);c[ImGuiCol_TitleBgActive]=rgba(47,47,47);c[ImGuiCol_TitleBgCollapsed]=rgba(38,38,38);
    c[ImGuiCol_MenuBarBg]=rgba(40,40,40);c[ImGuiCol_ScrollbarBg]=rgba(38,38,38);c[ImGuiCol_ScrollbarGrab]=rgba(79,79,79);c[ImGuiCol_ScrollbarGrabHovered]=rgba(99,99,99);c[ImGuiCol_ScrollbarGrabActive]=rgba(115,115,115);
    c[ImGuiCol_CheckMark]=rgba(192,192,192);c[ImGuiCol_SliderGrab]=rgba(112,112,112);c[ImGuiCol_SliderGrabActive]=rgba(148,148,148);
    c[ImGuiCol_Button]=rgba(58,58,58);c[ImGuiCol_ButtonHovered]=rgba(76,76,76);c[ImGuiCol_ButtonActive]=rgba(37,87,128);
    c[ImGuiCol_Header]=rgba(54,54,54);c[ImGuiCol_HeaderHovered]=rgba(65,65,65);c[ImGuiCol_HeaderActive]=rgba(44,93,135);
    c[ImGuiCol_Separator]=rgba(28,28,28);c[ImGuiCol_SeparatorHovered]=rgba(74,119,154);c[ImGuiCol_SeparatorActive]=rgba(76,146,201);
    c[ImGuiCol_ResizeGrip]=rgba(110,110,110,.15f);c[ImGuiCol_ResizeGripHovered]=rgba(90,130,160,.6f);c[ImGuiCol_ResizeGripActive]=rgba(90,130,160,.9f);
    c[ImGuiCol_Tab]=rgba(38,38,38);c[ImGuiCol_TabHovered]=rgba(63,63,63);c[ImGuiCol_TabSelected]=rgba(51,51,51);c[ImGuiCol_TabSelectedOverline]=rgba(72,133,180);
    c[ImGuiCol_TabDimmed]=rgba(38,38,38);c[ImGuiCol_TabDimmedSelected]=rgba(51,51,51);c[ImGuiCol_TabDimmedSelectedOverline]=rgba(79,79,79);
    c[ImGuiCol_DockingPreview]=rgba(62,125,175,.55f);c[ImGuiCol_DockingEmptyBg]=rgba(34,34,34);
    c[ImGuiCol_TextSelectedBg]=rgba(42,93,139,.75f);c[ImGuiCol_NavHighlight]=rgba(84,141,190);c[ImGuiCol_ModalWindowDimBg]=rgba(0,0,0,.4f);
}
}
