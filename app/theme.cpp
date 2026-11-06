// app/theme.cpp —— 配色实现 + 中文字体加载
#include "app/theme.hpp"

#include <cstdio>

namespace app {

namespace {

Palette g_palette = {
    /*window_bg */ ImVec4(0.055f, 0.062f, 0.078f, 1.00f),
    /*panel_bg  */ ImVec4(0.086f, 0.098f, 0.122f, 1.00f),
    /*panel_bg2 */ ImVec4(0.118f, 0.133f, 0.163f, 1.00f),
    /*border    */ ImVec4(0.180f, 0.204f, 0.243f, 0.90f),
    /*text      */ ImVec4(0.900f, 0.922f, 0.949f, 1.00f),
    /*text_dim  */ ImVec4(0.549f, 0.588f, 0.647f, 1.00f),
    /*accent    */ ImVec4(0.263f, 0.729f, 0.855f, 1.00f),
    /*ok        */ ImVec4(0.365f, 0.804f, 0.510f, 1.00f),
    /*warn      */ ImVec4(0.965f, 0.706f, 0.259f, 1.00f),
    /*err       */ ImVec4(0.937f, 0.365f, 0.376f, 1.00f),
    /*busy      */ ImVec4(0.965f, 0.620f, 0.196f, 1.00f),
    /*idle      */ ImVec4(0.235f, 0.263f, 0.318f, 1.00f),
    /*retiring  */ ImVec4(0.851f, 0.310f, 0.353f, 1.00f),
};

Fonts g_fonts;
float g_scale = 1.0f;

// 界面上用到、但不在"简体中文常用字"范围内的字（由 _scratch/check_glyphs 工具查出来的）。
// 看到界面里哪个字变成了 ?，就往这串里补那个字。
const char* kExtraGlyphs = "▲►▼→兜函屏崩帧戳拟拷晰栅槛渲滞焙粹躯辑迭逻钮阈频馈";

}  // namespace

const Palette& palette() { return g_palette; }
const Fonts& fonts() { return g_fonts; }
float ui_scale() { return g_scale; }

void apply_theme(float scale) {
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.0f) scale = 3.0f;
    g_scale = scale;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    const Palette& p = g_palette;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.text_dim;
    c[ImGuiCol_WindowBg] = p.window_bg;
    c[ImGuiCol_ChildBg] = p.panel_bg;
    c[ImGuiCol_PopupBg] = p.panel_bg2;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg] = p.panel_bg2;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.19f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.24f, 0.30f, 1.0f);

    c[ImGuiCol_TitleBg] = p.panel_bg;
    c[ImGuiCol_TitleBgActive] = p.panel_bg2;
    c[ImGuiCol_TitleBgCollapsed] = p.panel_bg;

    c[ImGuiCol_MenuBarBg] = p.panel_bg;

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.06f, 0.07f, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.27f, 0.33f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.35f, 0.42f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;

    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.42f, 0.83f, 0.94f, 1.0f);

    c[ImGuiCol_Button] = ImVec4(0.16f, 0.19f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.27f, 0.33f, 1.0f);
    c[ImGuiCol_ButtonActive] = p.accent;

    c[ImGuiCol_Header] = ImVec4(0.15f, 0.18f, 0.23f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.25f, 0.31f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.30f, 0.37f, 1.0f);

    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_SeparatorHovered] = p.accent;
    c[ImGuiCol_SeparatorActive] = p.accent;

    c[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.24f, 0.30f, 0.6f);
    c[ImGuiCol_ResizeGripHovered] = p.accent;
    c[ImGuiCol_ResizeGripActive] = p.accent;

    c[ImGuiCol_Tab] = p.panel_bg2;
    c[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.27f, 0.33f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.15f, 0.19f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.022f);

    c[ImGuiCol_PlotLines] = p.accent;
    c[ImGuiCol_PlotLinesHovered] = p.warn;
    c[ImGuiCol_PlotHistogram] = p.accent;
    c[ImGuiCol_PlotHistogramHovered] = p.warn;

    c[ImGuiCol_TextSelectedBg] = ImVec4(0.263f, 0.729f, 0.855f, 0.35f);
    c[ImGuiCol_NavCursor] = p.accent;

    // 一次性把上面所有以像素为单位的尺寸按显示器缩放放大（100% 显示器上等于原样）。
    // 注意：这句只能执行一次，否则尺寸会被反复放大。
    style.ScaleAllSizes(scale);
}

bool load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    if (scale < 0.75f) scale = 0.75f;

    // 按优先级找一个存在的中文字体。msyh.ttc 是 Windows 自带的微软雅黑。
    const char* candidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",    // 微软雅黑
        "C:\\Windows\\Fonts\\msyhl.ttc",   // 微软雅黑 Light（Win10 早期版本）
        "C:\\Windows\\Fonts\\simhei.ttf",  // 黑体
        "C:\\Windows\\Fonts\\simsun.ttc",  // 宋体
    };

    const char* chosen = nullptr;
    for (const char* path : candidates) {
        std::FILE* probe = std::fopen(path, "rb");
        if (probe != nullptr) {
            std::fclose(probe);  // 只是为了检查文件是否存在，真正的读取交给 ImGui
            chosen = path;
            break;
        }
    }

    if (!chosen) {
        g_fonts.body = io.Fonts->AddFontDefault();
        g_fonts.title = g_fonts.body;
        return false;
    }

    // 字形范围 = ImGui 自带的"简体中文常用字"（约 2500 个） + 我们界面里用到的少量生僻字。
    // 为什么不用 GetGlyphRangesChineseFull()（全部约 2 万个汉字）？
    //   那种范围要烘一张 8K 级的字体贴图，显存和启动时间都翻好几倍，
    //   而我们界面上只会出现固定的那些文字，没必要。
    // 如果以后新加的界面文字显示成 ?（问号方块），把那个字补进 kExtraGlyphs 即可。
    static ImVector<ImWchar> ranges;  // 必须是静态的：字体贴图在 load_fonts 返回之后才烘焙
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        builder.AddText(kExtraGlyphs);
        builder.BuildRanges(&ranges);
    }

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.RasterizerMultiply = 1.06f;

    g_fonts.body = io.Fonts->AddFontFromFileTTF(chosen, 17.0f * scale, &cfg, ranges.Data);
    g_fonts.title = io.Fonts->AddFontFromFileTTF(chosen, 25.0f * scale, &cfg, ranges.Data);
    if (!g_fonts.body) g_fonts.body = io.Fonts->AddFontDefault();
    if (!g_fonts.title) g_fonts.title = g_fonts.body;
    io.FontDefault = g_fonts.body;
    return true;
}

}  // namespace app
