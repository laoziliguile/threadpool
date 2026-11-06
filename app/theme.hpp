#pragma once
// ============================================================================
//  app/theme.hpp —— 配色与字体
//
//  界面代码最怕"颜色写死在各处"。这里把整套配色集中成一个 Palette，
//  所有绘制代码都只引用 palette().xxx，想换主题只改这一个文件。
// ============================================================================

#include "imgui.h"

namespace app {

struct Palette {
    ImVec4 window_bg;   // 窗口背景
    ImVec4 panel_bg;    // 面板背景
    ImVec4 panel_bg2;   // 次级面板（表头、卡片）
    ImVec4 border;      // 边框
    ImVec4 text;        // 正文
    ImVec4 text_dim;    // 次要文字
    ImVec4 accent;      // 主题色（青蓝）
    ImVec4 ok;          // 成功 / 空闲
    ImVec4 warn;        // 警告 / 忙碌
    ImVec4 err;         // 失败 / 退休
    ImVec4 busy;        // 忙碌的线程块
    ImVec4 idle;        // 空闲的线程块
    ImVec4 retiring;    // 退休中的线程块
};

struct Fonts {
    ImFont* body = nullptr;   // 正文（17px）
    ImFont* title = nullptr;  // 标题/大数字（25px）
};

const Palette& palette();
const Fonts& fonts();

// 当前界面的 DPI 缩放比。布局里写死的像素尺寸都要乘上它，界面才会随系统缩放等比变大。
float ui_scale();

// 设置 ImGui 的全局样式（圆角、间距、各控件配色）。
// scale 是显示器 DPI 缩放比（100% = 1.0，150% = 1.5），所有像素尺寸会乘上它。
void apply_theme(float scale = 1.0f);

// 加载中文字体。必须在 ImGui_ImplDX11_Init 之前调用（后端初始化时会烘焙字形图集）。
// 返回 true 表示成功加载了中文 TrueType 字体；false 表示退回默认字体（中文会显示成方块）。
bool load_fonts(float scale = 1.0f);

// 把 ImVec4 转成绘制列表用的 ImU32
inline ImU32 u32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

}  // namespace app
