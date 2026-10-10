#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Photoshop ABR 中可直接用于盖印的灰度笔尖。mask: 0=透明，255=完全覆盖。
struct AbrBrushTip
{
    std::string          name;
    int                  width   = 0;
    int                  height  = 0;
    float                spacing = 0.25f; // 相对于笔尖直径的比例
    std::vector<uint8_t> mask;
};

struct AbrLoadResult
{
    int                      version = 0;
    std::vector<AbrBrushTip> brushes;
    std::string              error;
};

// 读取 Photoshop ABR v1/v2/v6/v10 的采样笔尖。计算型笔尖会被安全跳过。
AbrLoadResult loadAbrBrushFile(const std::wstring& path);
