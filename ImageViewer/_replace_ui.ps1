$file = "G:\ProjectC++\VulkanTutorial_\ImageViewer\main.cpp"
$content = [System.IO.File]::ReadAllText($file)

# 找 buildImGuiUI 函数
$pattern = "(?s)  void buildImGuiUI\(\) \{.*?^\s*\}\s*\r?\n(?=\s*// ---)"
$m = [regex]::Match($content, $pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)

if (-not $m.Success) {
    Write-Host "FAILED!" -ForegroundColor Red
    exit 1
}
Write-Host "Matched from pos $($m.Index) length $($m.Length)" -ForegroundColor Green

$newFunc = @'
  void buildImGuiUI() {
    int winW, winH;
    renderer.extent(winW, winH);
    float cy = mainAreaCenterY();

    // 鼠标任何移动都刷新
    double mx, my;
    mouseFb(mx, my);
    static double lastMx = -1, lastMy = -1;
    if (std::abs(mx - lastMx) > 0.5 || std::abs(my - lastMy) > 0.5) {
      lastMx = mx; lastMy = my;
      lastInputTime = glfwGetTime();
    }

    if (!uiVisible) return; // Immersive 模式

    auto noDeco = [] {
      return ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
             ImGuiWindowFlags_NoSavedSettings |
             ImGuiWindowFlags_NoBringToFrontOnFocus;
    };

    // === 主图圆角底板 + 阴影 ===
    if (mainTex != 0) {
      float qw = imgW * scale, qh = imgH * scale;
      float qx = winW * 0.5f + panX - qw * 0.5f;
      float qy = cy + panY - qh * 0.5f;
      float cr = std::min(18.0f, qw * 0.03f);

      ImDrawList *dl = ImGui::GetForegroundDrawList();
      for (int i = 8; i >= 1; i--) {
        float s = 24.0f * (float)i / 8.0f;
        float a = 0.018f;
        dl->AddRectFilled(
            ImVec2(qx - s, qy - s),
            ImVec2(qx + qw + s, qy + qh + s),
            IM_COL32(0, 0, 0, (ImUCHAR)(a * 255)), cr + s);
      }
      dl->AddRectFilled(ImVec2(qx, qy), ImVec2(qx + qw, qy + qh),
                        IM_COL32(28, 29, 31, 255), cr);
    }

    // === 顶部工具栏 ===
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)winW, TOOLBAR_HEIGHT), ImGuiCond_Always);
    ImGui::Begin("##toolbar", nullptr, noDeco());

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));
    if (ImGui::Button(u8"打开")) {
      std::wstring f = openFileDialog(nativeWindow);
      if (!f.empty()) openPath(fs::path(f));
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"📂")) {
      std::wstring f = openFolderDialog(nativeWindow);
      if (!f.empty()) openPath(fs::path(f));
    }
    ImGui::PopStyleVar();
    ImGui::SameLine(); ImGui::Spacing(); ImGui::SameLine();

    if (current >= 0 && current < (int)images.size()) {
      std::string fname = images[current].path.filename().u8string();
      ImGui::TextDisabled("(%d/%d) %s", current + 1, (int)images.size(),
                          fname.c_str());
    } else {
      ImGui::TextDisabled(u8"拖入图片 / 文件夹，或点 ⌘O");
    }

    char zoomStr[24];
    snprintf(zoomStr, sizeof(zoomStr), "%d%%", (int)std::lround(scale * 100));
    float rightW = 8.0f;
    const ImGuiStyle &st = ImGui::GetStyle();
    const char *labels[] = {"◀", "▶", "-", zoomStr, "+", u8"适应", "1:1"};
    for (const char *lb : labels)
      rightW += ImGui::CalcTextSize(lb).x + st.FramePadding.x * 2 + st.ItemSpacing.x;
    rightW += 24.0f + ImGui::CalcTextSize(u8"✨").x + st.FramePadding.x * 2 + st.ItemSpacing.x;
    ImGui::SameLine((float)winW - rightW);

    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) selectImage(current - 1);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) selectImage(current + 1);
    ImGui::SameLine(); ImGui::Separator(); ImGui::SameLine();

    if (ImGui::Button("-")) zoomAt(winW * 0.5f, cy, 1.0f / 1.2f);
    ImGui::SameLine();
    if (ImGui::Button(zoomStr)) fitToWindow();
    ImGui::SameLine();
    if (ImGui::Button("+")) zoomAt(winW * 0.5f, cy, 1.2f);
    ImGui::SameLine();
    if (ImGui::Button(u8"适应")) fitToWindow();
    ImGui::SameLine();
    if (ImGui::Button("1:1") && mainTex) { scale = 1.0f; panX = panY = 0; }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,
                          sharpenEnabled ? ImVec4(0.039f, 0.518f, 1.000f, 1.0f)
                                         : st.Colors[ImGuiCol_Button]);
    if (ImGui::Button(u8"✨")) sharpenEnabled = !sharpenEnabled;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"放大增强（双三次）— 空格键切换");

    ImGui::End();

    if (images.empty()) return;

    // === 底部缩略图条 ===
    if (showThumbsBar) {
      ImGui::SetNextWindowPos(ImVec2(0, (float)winH - THUMB_PANEL_H), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2((float)winW, THUMB_PANEL_H), ImGuiCond_Always);
      ImGui::Begin("##thumbs", nullptr, noDeco() | ImGuiWindowFlags_HorizontalScrollbar);

      for (int i = 0; i < (int)images.size(); i++) {
        ImageItem &item = images[i];
        ImGui::PushID(i);
        bool selected = (i == current);

        if (selected) {
          ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.039f, 0.518f, 1.000f, 1.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.5f);
          ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        } else {
          ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
          ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.255f, 0.259f, 0.275f, 1.0f));
        }

        bool clicked = false;
        if (item.imguiTex != 0) {
          float s = std::min(THUMB_IMG_H / item.thumbH, 1.0f);
          clicked = ImGui::ImageButton("##thumb", item.imguiTex,
                                      ImVec2(item.thumbW * s, item.thumbH * s));
        } else {
          clicked = ImGui::Button(item.thumbFailed ? "x" : "...", ImVec2(96, THUMB_IMG_H));
        }
        if (clicked) selectImage(i);

        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        if (selected && scrollThumbsToCurrent) ImGui::SetScrollHereX(0.5f);
        ImGui::SameLine();
        ImGui::PopID();
      }
      scrollThumbsToCurrent = false;
      ImGui::End();
    }

    // === 右下角信息面板 ===
    if (showInfoPanel && current >= 0 && current < (int)images.size()) {
      const float panelW = 230.0f, panelH = 150.0f;
      float panelX = (float)winW - panelW - 20.0f;
      float panelY = (showThumbsBar ? (float)winH - THUMB_PANEL_H : (float)winH)
                   - panelH - 20.0f;

      ImGui::SetNextWindowPos(ImVec2(panelX, panelY), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
      ImGui::Begin("##info", nullptr, noDeco() | ImGuiWindowFlags_NoBackground);
      ImGui::PopStyleVar();

      ImDrawList *dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(ImGui::GetWindowPos(),
                        ImVec2(ImGui::GetWindowPos().x + panelW,
                               ImGui::GetWindowPos().y + panelH),
                        IM_COL32(28, 29, 31, 220), 10.0f);

      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.97f, 1.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));

      std::string fname = images[current].path.filename().u8string();
      if (fname.size() > 22) fname = fname.substr(0, 19) + "...";
      ImGui::Text("%s", fname.c_str());
      ImGui::Separator();
      ImGui::Text(u8"%d × %d px", imgW, imgH);
      ImGui::Text(u8"缩放 %.0f%%", scale * 100.0f);
      std::error_code ec;
      uintmax_t fsize = fs::file_size(images[current].path, ec);
      const char *unit = "B";
      float f = (float)fsize;
      if (fsize > 1024)      { f /= 1024;        unit = "KB"; }
      if (fsize > 1024*1024) { f /= 1024;        unit = "MB"; }
      ImGui::Text(u8"%.1f %s", f, unit);
      std::string ext = images[current].path.extension().u8string();
      for (auto &c : ext) c = tolower((unsigned char)c);
      ImGui::Text(u8"%s", ext.c_str());

      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      ImGui::End();
    }
  }
'@

$newContent = $content.Substring(0, $m.Index) + $newFunc + $content.Substring($m.Index + $m.Length)
[System.IO.File]::WriteAllText($file, $newContent, [System.Text.UTF8Encoding]::new($false))
Write-Host "SUCCESS - replaced buildImGuiUI" -ForegroundColor Green
