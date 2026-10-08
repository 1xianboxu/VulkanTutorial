// Vulkan 图片浏览器
// - 拖拽图片文件/文件夹到窗口打开（也支持命令行参数与工具栏对话框）
// - 支持 psd / jpg / png / bmp / tga / gif / hdr 等 stb_image 可解码的格式
// - UI 使用 Dear ImGui：顶部工具栏 + 底部缩略图栏（可点击选中、横向滚动）
// - 滚轮缩放（以光标为中心）；左键拖拽平移；双击切换实际大小/适应窗口
// - 主图带完整 mipmap 链 + 各向异性过滤：缩小不闪、放大平滑

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // 防止 windows.h 的 min/max 宏破坏 std::min/std::max
#endif
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "renderer/VulkanImageRenderer.h"
#include "renderer/AbrBrushLoader.h"

#include "backends/imgui_impl_vulkan.h" // AddTexture / RemoveTexture（缩略图交给 ImGui 画）
#include "imgui.h"

#include "renderer/PlatformDialogs.h"
#include "resource.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
const uint32_t WIDTH = 1280;
const uint32_t HEIGHT = 800;

const float THUMB_PANEL_H = 128.0f; // 底部缩略图面板高度（ImGui 窗口）
const float THUMB_IMG_H = 96.0f;    // 缩略图按钮的图片高度
const float FLOAT_TB_H = 44.0f;     // 底部悬浮胶囊工具栏高度
const float FLOAT_TB_GAP = 28.0f;   // 胶囊工具栏距 thumbs 的空隙（悬浮占位）
const float HQ_MIN_SCALE = 2.0f;     // 轻度放大时硬件线性采样更自然
const int THUMB_BOX_W = 160;        // 缩略图最大宽（解码目标）
const int THUMB_BOX_H = 96;         // 缩略图最大高

const std::set<std::string> imageExtensions = {".psd", ".jpg", ".jpeg", ".png",
                                               ".bmp", ".tga", ".gif",  ".hdr",
                                               ".pic", ".ppm", ".pgm"};

bool isImageFile(const fs::path &p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return imageExtensions.count(ext) > 0;
}

// 简单 box 滤波降采样（缩略图 / 超 GPU 纹理上限的大图）
void downscaleBox(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw,
                  int dh) {
  float xscale = (float)sw / dw;
  float yscale = (float)sh / dh;
  for (int y = 0; y < dh; y++) {
    for (int x = 0; x < dw; x++) {
      int x0 = (int)(x * xscale), x1 = (int)((x + 1) * xscale);
      int y0 = (int)(y * yscale), y1 = (int)((y + 1) * yscale);
      if (x1 <= x0)
        x1 = x0 + 1;
      if (y1 <= y0)
        y1 = y0 + 1;
      if (x1 > sw)
        x1 = sw;
      if (y1 > sh)
        y1 = sh;

      uint64_t rp = 0, gp = 0, bp = 0, a = 0;
      uint32_t cnt = 0;
      for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
          const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
          rp += (uint32_t)p[0] * p[3];
          gp += (uint32_t)p[1] * p[3];
          bp += (uint32_t)p[2] * p[3];
          a += p[3];
          cnt++;
        }
      }
      uint8_t *d = dst + ((size_t)y * dw + x) * 4;
      // 在预乘 Alpha 空间平均，避免透明像素中隐藏的 RGB 污染缩略图边缘。
      d[0] = a ? (uint8_t)(rp / a) : 0;
      d[1] = a ? (uint8_t)(gp / a) : 0;
      d[2] = a ? (uint8_t)(bp / a) : 0;
      d[3] = (uint8_t)(a / cnt);
    }
  }
}
} // namespace

class ImageViewerApp {
public:
  void run(const std::string &startPath) {
    try {
      initWindow();
      renderer.init(window);
      initImGui();
      loadPersistentBrushLibrary();
      if (!startPath.empty())
        openPath(startPath);
      mainLoop();
    } catch (...) {
      shutdown(); // 异常路径也要 join 解码线程，否则析构时 std::terminate
      throw;
    }
    shutdown();
  }

  ~ImageViewerApp() { shutdown(); }

private:
  struct ImageItem {
    fs::path path;
    TextureHandle thumb = 0; // 渲染器句柄
    ImTextureID imguiTex =
        0; // 同一张纹理注册给 ImGui 的 ID（ImU64，实为 VkDescriptorSet）
    int thumbW = 0, thumbH = 0;
    bool thumbQueued = false;
    bool thumbFailed = false;
  };

  struct DecodeTask {
    int index;
    bool full;         // true=全尺寸主图，false=缩略图
    uint64_t gen;      // 代数：打开新文件夹时 +1，丢弃过期结果
    fs::path filepath; // 入队时拷贝路径，避免 worker 与主线程竞争 images 数组
  };

  struct DecodeResult {
    int index;
    bool full;
    bool ok;
    int w = 0, h = 0;
    std::vector<uint8_t> pixels;
    uint64_t gen;
  };

  struct DrawPoint {
    float x = 0.0f;
    float y = 0.0f;
  };

  enum class DrawTool { Pen, Eraser, Clear };

  struct DrawStroke {
    std::vector<DrawPoint> points;
    float color[4] = {0.094f, 0.565f, 0.965f, 1.0f};
    float radius = 3.0f; // 原图像素单位，缩放后仍与图片保持一致
    float hardness = 0.85f;
    float spacing = 0.12f; // 直径的比例
    DrawTool tool = DrawTool::Pen;
    std::shared_ptr<const AbrBrushTip> brushTip;
  };

  struct ImportedBrushTip {
    std::shared_ptr<AbrBrushTip> tip;
    TextureHandle previewTexture = 0;
    ImTextureID previewImGui = 0;
  };

  GLFWwindow *window = nullptr;
  VulkanImageRenderer renderer;
  void *nativeWindow = nullptr; // HWND（打开对话框的 owner）

  std::vector<ImageItem> images;
  int current = -1;
  uint64_t generation = 0;

  // 主图
  TextureHandle mainTex = 0;
  int imgW = 0, imgH = 0;
  int uploadedIndex = -1;
  bool loadingCurrent = false;
  bool currentDecodeFailed = false;

  // 视图变换（主图区域）
  float scale = 1.0f; // 绝对缩放（1.0 = 图片像素:屏幕像素 1:1）
  float panX = 0.0f;  // 相对主区域中心的平移（像素）
  float panY = 0.0f;

  // UI 状态：Mac 风格浮层工具栏可隐藏 / 信息面板可切换 / 全屏
  bool uiVisible = true;
  bool fullscreen = false;
  bool showInfoPanel = true;
  double lastInputTime = 0.0;
  static constexpr double UI_AUTO_HIDE_SEC = 3.0;

  // 缩略图自动滚动 + 放大增强（计算着色器双三次）
  bool scrollThumbsToCurrent = false;
  bool sharpenEnabled = true;
  bool showThumbsBar = true;
  float pendingThumbScroll = 0.0f;   // 滚轮 → delta（应用时乘系数）
  float thumbDragScrollDelta = 0.0f; // 拖拽 → delta（1:1，不乘系数）
  // 缩略图条鼠标拖拽状态
  bool thumbDragging = false;
  double thumbDragStartX = 0;

  // 旋转（UV 变换）
  int rotationDeg = 0;

  // 输入状态 + 自动隐藏
  bool dragging = false;
  double lastMouseX = 0, lastMouseY = 0;
  double lastClickTime = 0.0;
  double cursorX = WIDTH * 0.5, cursorY = HEIGHT * 0.5;

  // 绘制/标注模式。CPU RGBA 工作画布从原图复制，画笔和橡皮擦都直接
  // 修改这份副本；橡皮擦降低原图/笔触的 Alpha，透明处显示棋盘格。
  bool editMode = false;
  bool editDirty = false;
  bool drawingStroke = false;
  int editIndex = -1;
  DrawTool drawTool = DrawTool::Pen;
  float brushColor[4] = {0.094f, 0.565f, 0.965f, 1.0f};
  float brushDiameter = 8.0f;
  float brushHardness = 0.85f;
  float brushSpacing = 0.12f;
  std::vector<ImportedBrushTip> importedBrushTips;
  std::shared_ptr<AbrBrushTip> activeBrushTip;
  bool brushUiHovered = false; // 上一帧鼠标是否位于画笔面板或笔尖列表
  std::vector<DrawStroke> drawStrokes;
  size_t drawHistoryCursor = 0; // [0,cursor) 是当前已应用命令
  std::vector<uint8_t> editLayerPixels;
  TextureHandle editLayerTex = 0;
  bool editLayerDirty = false;
  int dirtyMinX = 0, dirtyMinY = 0, dirtyMaxX = -1, dirtyMaxY = -1;
  fs::path lastEditSavePath;
  int pendingSelectIndex = -1;
  bool openUnsavedPopup = false;
  std::string editNotice;
  double editNoticeUntil = 0.0;

  // 后台解码线程
  std::thread worker;
  std::mutex taskMutex;
  std::condition_variable taskCv;
  std::deque<DecodeTask> tasks;
  std::atomic<bool> quit{false};
  std::mutex resultMutex;
  std::vector<DecodeResult> results;

  // 全尺寸解码缓存（预取 ±1）
  std::map<int, DecodeResult> fullCache;
  std::set<int> fullQueued;

  // -----------------------------------------------------------------
  // 窗口与回调
  // -----------------------------------------------------------------

  void initWindow() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(
        WIDTH, HEIGHT, "Vulkan 图片浏览器 — 拖入图片或文件夹，或点左上角打开",
        nullptr, nullptr);
    glfwSetWindowUserPointer(window, this);

#ifdef _WIN32
    nativeWindow = glfwGetWin32Window(window);
    HWND hwnd = static_cast<HWND>(nativeWindow);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HICON bigIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    HICON smallIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (bigIcon)
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    if (smallIcon)
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                   reinterpret_cast<LPARAM>(smallIcon));
#endif

    glfwSetKeyCallback(window,
                       [](GLFWwindow *w, int key, int, int action, int mods) {
                         app(w)->onKey(key, action, mods);
                       });
    glfwSetScrollCallback(window, [](GLFWwindow *w, double, double yoff) {
      app(w)->onScroll(yoff);
    });
    glfwSetMouseButtonCallback(window,
                               [](GLFWwindow *w, int button, int action, int) {
                                 app(w)->onMouseButton(button, action);
                               });
    glfwSetCursorPosCallback(window, [](GLFWwindow *w, double x, double y) {
      app(w)->onCursorPos(x, y);
    });
    glfwSetDropCallback(
        window, [](GLFWwindow *w, int count, const char **paths) {
          if (count > 0)
            app(w)->openPath(fs::u8path(paths[0])); // GLFW 拖放路径为 UTF-8
        });
    glfwSetWindowCloseCallback(window, [](GLFWwindow *w) {
      ImageViewerApp *viewer = app(w);
      if (viewer->editDirty) {
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        viewer->pendingSelectIndex = -2; // -2 表示关闭程序
        viewer->openUnsavedPopup = true;
        viewer->uiVisible = true;
      }
    });

    worker = std::thread([this] { decodeLoop(); });
  }

  void initImGui() {
    renderer.initImGui();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // 中文字体：微软雅黑
    ImFont *font = io.Fonts->AddFontFromFileTTF(
        "C:/Windows/Fonts/msyh.ttc", 18.0f, nullptr,
        io.Fonts->GetGlyphRangesChineseFull());
    if (font == nullptr)
      io.Fonts->AddFontDefault();

    ImGui_ImplVulkan_CreateFontsTexture();

    // === 2345 看图王浅色主题 ===
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.AntiAliasedFill = true;
    style.AntiAliasedLines = true;
    style.CurveTessellationTol = 0.25f;

    // 2345 看图王配色（浅灰背景 + 白底浮层 + 蓝色强调）
    ImVec4 cBG = ImVec4(0.945f, 0.945f, 0.945f, 1.00f);      // #F2F2F2 主背景
    ImVec4 cPanel = ImVec4(1.000f, 1.000f, 1.000f, 0.96f);   // 白色胶囊工具栏
    ImVec4 cPanel2 = ImVec4(0.960f, 0.960f, 0.960f, 1.00f);  // 浅灰次级
    ImVec4 cBorder = ImVec4(0.820f, 0.820f, 0.820f, 1.00f);  // #D1D1D1
    ImVec4 cText = ImVec4(0.180f, 0.180f, 0.180f, 1.00f);    // #2E2E2E 深色文字
    ImVec4 cTextDim = ImVec4(0.500f, 0.500f, 0.500f, 1.00f); // #808080 次要
    ImVec4 cAccent = ImVec4(0.094f, 0.565f, 0.965f, 1.00f);  // #1890FF 蓝
    ImVec4 cAccentH = ImVec4(0.149f, 0.612f, 0.976f, 1.00f); // 悬停态
    ImVec4 cHoverBg = ImVec4(0.900f, 0.900f, 0.900f, 1.00f); // 按钮悬停
    ImVec4 cWhite = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);

    style.Colors[ImGuiCol_Text] = cText;
    style.Colors[ImGuiCol_TextDisabled] = cTextDim;
    style.Colors[ImGuiCol_WindowBg] = cBG;
    style.Colors[ImGuiCol_ChildBg] = cWhite;
    style.Colors[ImGuiCol_PopupBg] = cWhite;
    style.Colors[ImGuiCol_Border] = cBorder;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_FrameBg] = cPanel2;
    style.Colors[ImGuiCol_FrameBgHovered] = cHoverBg;
    style.Colors[ImGuiCol_FrameBgActive] = cAccent;
    style.Colors[ImGuiCol_TitleBg] = cPanel;
    style.Colors[ImGuiCol_TitleBgActive] = cPanel2;
    style.Colors[ImGuiCol_TitleBgCollapsed] = cPanel2;
    style.Colors[ImGuiCol_MenuBarBg] = cPanel2;
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ScrollbarGrab] = cBorder;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = cTextDim;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = cAccent;
    style.Colors[ImGuiCol_CheckMark] = cAccent;
    style.Colors[ImGuiCol_SliderGrab] = cAccent;
    style.Colors[ImGuiCol_SliderGrabActive] = cAccentH;
    style.Colors[ImGuiCol_Button] = cWhite;
    style.Colors[ImGuiCol_ButtonHovered] = cHoverBg;
    style.Colors[ImGuiCol_ButtonActive] = cBorder;
    style.Colors[ImGuiCol_Header] = cAccent;
    style.Colors[ImGuiCol_HeaderHovered] = cAccentH;
    style.Colors[ImGuiCol_HeaderActive] = cAccent;
    style.Colors[ImGuiCol_Separator] = cBorder;
    style.Colors[ImGuiCol_SeparatorHovered] = cAccent;
    style.Colors[ImGuiCol_SeparatorActive] = cAccent;
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_PlotLines] = cAccent;
    style.Colors[ImGuiCol_PlotLinesHovered] = cAccentH;
    style.Colors[ImGuiCol_PlotHistogram] = cAccent;
    style.Colors[ImGuiCol_PlotHistogramHovered] = cAccentH;
    style.Colors[ImGuiCol_TableHeaderBg] = cPanel2;
    style.Colors[ImGuiCol_TableBorderStrong] = cBorder;
    style.Colors[ImGuiCol_TableBorderLight] = cBorder;
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_TableRowBgAlt] = cPanel2;
    style.Colors[ImGuiCol_TextSelectedBg] = cAccent;
    style.Colors[ImGuiCol_DragDropTarget] = cAccent;
    style.Colors[ImGuiCol_NavHighlight] = cAccent;
    style.Colors[ImGuiCol_NavWindowingHighlight] = cAccent;
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.6f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
  }

  static ImageViewerApp *app(GLFWwindow *w) {
    return reinterpret_cast<ImageViewerApp *>(glfwGetWindowUserPointer(w));
  }

  // -----------------------------------------------------------------
  // 文件夹扫描 / 解码线程
  // -----------------------------------------------------------------

  void openFromDialog(bool folder) {
    std::wstring selected = folder ? openFolderDialog(nativeWindow)
                                   : openFileDialog(nativeWindow);
    if (!selected.empty())
      openPath(fs::path(selected));
  }

  void openPath(const fs::path &p) {
    if (editDirty) {
      setEditNotice(u8"当前标注尚未保存，请先保存或取消修改");
      return;
    }
    resetEditSession();
    std::error_code ec;

    fs::path folder;
    fs::path selectFile;
    if (fs::is_directory(p, ec)) {
      folder = p;
    } else if (fs::is_regular_file(p, ec) && isImageFile(p)) {
      folder = p.parent_path();
      selectFile = p.filename();
    } else {
      setTitle("不支持的文件: " + p.filename().u8string());
      return;
    }

    // 停止旧任务
    {
      std::lock_guard<std::mutex> lock(taskMutex);
      tasks.clear();
      generation++;
    }
    {
      std::lock_guard<std::mutex> lock(resultMutex);
      results.clear();
    }
    fullCache.clear();
    fullQueued.clear();

    // 释放旧纹理前等 GPU 空闲：ImGui 的 descriptor 是立即释放的，
    // 可能仍被在飞帧引用（渲染器的纹理销毁本身是延迟的，但这里一并等掉最稳妥）
    renderer.waitIdle();

    // 释放旧纹理
    for (auto &item : images) {
      if (item.imguiTex != 0)
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)item.imguiTex);
      if (item.thumb != 0)
        renderer.destroyTexture(item.thumb);
    }
    images.clear();
    if (mainTex != 0) {
      renderer.destroyTexture(mainTex);
      mainTex = 0;
    }
    uploadedIndex = -1;
    current = -1;
    rotationDeg = 0;
    panX = panY = 0.0f;

    // 扫描
    for (const auto &entry : fs::directory_iterator(folder, ec)) {
      if (entry.is_regular_file(ec) && isImageFile(entry.path()))
        images.push_back({entry.path()});
    }
    std::sort(
        images.begin(), images.end(),
        [](const ImageItem &a, const ImageItem &b) { return a.path < b.path; });

    if (images.empty()) {
      setTitle("文件夹中没有图片 — 拖入图片或文件夹");
      return;
    }

    // 选中目标
    current = 0;
    if (!selectFile.empty()) {
      for (size_t i = 0; i < images.size(); i++) {
        if (images[i].path.filename() == selectFile) {
          current = (int)i;
          break;
        }
      }
    }

    // 排队：prefetchAround 会把当前图放在队首，缩略图随后
    prefetchAround(current);
    for (size_t i = 0; i < images.size(); i++)
      queueThumbDecode((int)i);

    loadingCurrent = true;
    scrollThumbsToCurrent = true;
  }

  void queueThumbDecode(int index) {
    if (images[index].thumbQueued || images[index].thumb != 0)
      return;
    images[index].thumbQueued = true;
    std::lock_guard<std::mutex> lock(taskMutex);
    tasks.push_back({index, false, generation, images[index].path});
    taskCv.notify_one();
  }

  void queueFullDecode(int index) {
    if (fullQueued.count(index) || fullCache.count(index))
      return;
    fullQueued.insert(index);
    std::lock_guard<std::mutex> lock(taskMutex);
    tasks.push_front(
        {index, true, generation, images[index].path}); // 全尺寸图优先
    taskCv.notify_one();
  }

  void prefetchAround(int index) {
    // 先排邻居再排当前，使当前排在队首
    if (index + 1 < (int)images.size())
      queueFullDecode(index + 1);
    if (index - 1 >= 0)
      queueFullDecode(index - 1);
    queueFullDecode(index);
  }

  void decodeLoop() {
    while (!quit) {
      DecodeTask task;
      {
        std::unique_lock<std::mutex> lock(taskMutex);
        taskCv.wait(lock, [this] { return quit || !tasks.empty(); });
        if (quit)
          return;
        task = tasks.front();
        tasks.pop_front();
      }

      DecodeResult res;
      res.index = task.index;
      res.full = task.full;
      res.gen = task.gen;

      // 用 ifstream 读入内存再解码：stbi_load 在 Windows 走 ANSI fopen，
      // 中文路径会失败；MSVC 的 ifstream 支持 wchar 路径
      const fs::path &file = task.filepath;
      int w = 0, h = 0, channels = 0;
      stbi_uc *pixels = nullptr;
      {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (in) {
          std::streamsize size = in.tellg();
          in.seekg(0);
          std::vector<uint8_t> bytes((size_t)size);
          if (in.read((char *)bytes.data(), size))
            pixels = stbi_load_from_memory(bytes.data(), (int)size, &w, &h,
                                           &channels, STBI_rgb_alpha);
        }
      }

      if (!pixels) {
        res.ok = false;
        std::cerr << "decode failed: " << file.u8string() << " ("
                  << stbi_failure_reason() << ")" << std::endl;
      } else {
        res.ok = true;
        if (task.full) {
          // 超过 GPU 纹理上限的大图先降采样
          int maxTex = renderer.maxTextureSize();
          if (w > maxTex || h > maxTex) {
            float s = (float)maxTex / std::max(w, h);
            int dw = std::max(1, (int)(w * s));
            int dh = std::max(1, (int)(h * s));
            res.pixels.resize((size_t)dw * dh * 4);
            downscaleBox(pixels, w, h, res.pixels.data(), dw, dh);
            res.w = dw;
            res.h = dh;
            std::cout << "downscaled to GPU limit: " << file << std::endl;
          } else {
            res.w = w;
            res.h = h;
            res.pixels.assign(pixels, pixels + (size_t)w * h * 4);
          }
        } else {
          // 缩略图：等比缩到 THUMB_BOX 内
          float s = std::min((float)THUMB_BOX_W / w, (float)THUMB_BOX_H / h);
          s = std::min(s, 1.0f);
          int dw = std::max(1, (int)(w * s));
          int dh = std::max(1, (int)(h * s));
          res.pixels.resize((size_t)dw * dh * 4);
          downscaleBox(pixels, w, h, res.pixels.data(), dw, dh);
          res.w = dw;
          res.h = dh;
        }
        stbi_image_free(pixels);
      }

      {
        std::lock_guard<std::mutex> lock(resultMutex);
        results.push_back(std::move(res));
      }
    }
  }

  void consumeResults() {
    std::vector<DecodeResult> batch;
    {
      std::lock_guard<std::mutex> lock(resultMutex);
      batch.swap(results);
    }

    for (auto &res : batch) {
      if (res.gen != generation || res.index < 0 ||
          res.index >= (int)images.size())
        continue; // 过期结果

      ImageItem &item = images[res.index];

      if (!res.full) {
        if (res.ok) {
          item.thumb =
              renderer.createTexture(res.pixels.data(), res.w, res.h, false);
          item.thumbW = res.w;
          item.thumbH = res.h;

          // 注册给 ImGui（缩略图由 ImGui 直接绘制）
          VkImageView view;
          VkSampler sampler;
          if (renderer.getTextureInfo(item.thumb, view, sampler))
            item.imguiTex = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {
          item.thumbFailed = true;
        }
      } else {
        fullQueued.erase(res.index);
        if (res.ok) {
          fullCache[res.index] = std::move(res);
          if (res.index == current)
            uploadCurrentFromCache();
        } else if (res.index == current) {
          loadingCurrent = false;
          currentDecodeFailed = true;
        }
      }
    }

    // 缓存只保留当前附近的图
    for (auto it = fullCache.begin(); it != fullCache.end();) {
      if (std::abs(it->first - current) > 2)
        it = fullCache.erase(it);
      else
        ++it;
    }
  }

  void uploadCurrentFromCache() {
    auto it = fullCache.find(current);
    if (it == fullCache.end())
      return;

    const DecodeResult &res = it->second;
    TextureHandle newTex =
        renderer.createTexture(res.pixels.data(), res.w, res.h, true);
    if (mainTex != 0)
      renderer.destroyTexture(mainTex);
    mainTex = newTex;
    imgW = res.w;
    imgH = res.h;
    uploadedIndex = current;
    loadingCurrent = false;

    fitToWindow();
  }

  // -----------------------------------------------------------------
  // 视图变换
  // -----------------------------------------------------------------

  // 底部悬浮胶囊工具栏占位（悬浮在主图之上，主图要缩进去避免被盖）
  float bottomToolbarOccupied() const {
    if (!uiVisible || images.empty())
      return 0.0f;
    return FLOAT_TB_H + FLOAT_TB_GAP;
  }

  float mainAreaTop() const {
    return uiVisible && showInfoPanel ? 66.0f : 0.0f;
  }
  float mainAreaBottom() const {
    int w, h;
    renderer.extent(w, h);
    float bottom = (float)h;
    if (uiVisible && showThumbsBar && !images.empty())
      bottom -= THUMB_PANEL_H;
    bottom -= bottomToolbarOccupied();
    return bottom;
  }
  float mainAreaCenterY() const {
    return (mainAreaTop() + mainAreaBottom()) * 0.5f;
  }

  bool isQuarterTurn() const { return rotationDeg % 180 != 0; }
  float displayedImageWidth() const {
    return (float)(isQuarterTurn() ? imgH : imgW);
  }
  float displayedImageHeight() const {
    return (float)(isQuarterTurn() ? imgW : imgH);
  }

  float fitScale() const {
    if (imgW <= 0 || imgH <= 0)
      return 1.0f;
    int winW, winH;
    renderer.extent(winW, winH);
    float mainH = mainAreaBottom() - mainAreaTop();
    return std::min((float)winW / displayedImageWidth(),
                    mainH / displayedImageHeight());
  }

  // 以 (fx, fy)（帧缓冲像素）为焦点缩放：保持焦点下的图像点不动
  void zoomAt(float fx, float fy, float factor) {
    if (mainTex == 0)
      return;

    int winW, winH;
    renderer.extent(winW, winH);
    float cy0 = mainAreaCenterY();
    float cx = winW * 0.5f + panX;
    float cy = cy0 + panY;

    float u = (fx - cx) / scale; // 图像空间坐标（以图中心为原点）
    float v = (fy - cy) / scale;

    float newScale = scale * factor;
    newScale = std::clamp(newScale, std::min(fitScale(), 0.02f), 64.0f);

    panX = fx - u * newScale - winW * 0.5f;
    panY = fy - v * newScale - cy0;
    scale = newScale;
  }

  void fitToWindow() {
    scale = fitScale();
    panX = panY = 0;
  }

  void rotateBy(int degrees) {
    rotationDeg = (rotationDeg + degrees) % 360;
    if (rotationDeg < 0)
      rotationDeg += 360;
    // 90° 旋转会交换显示宽高，重新适应窗口可避免突然裁切到画布外。
    fitToWindow();
  }

  void resetEditSession() {
    editMode = false;
    editDirty = false;
    drawingStroke = false;
    editIndex = -1;
    drawStrokes.clear();
    drawHistoryCursor = 0;
    editLayerPixels.clear();
    if (editLayerTex != 0) {
      renderer.destroyTexture(editLayerTex);
      editLayerTex = 0;
    }
    editLayerDirty = false;
    dirtyMaxX = dirtyMaxY = -1;
    lastEditSavePath.clear();
  }

  void selectImageNow(int index) {
    if (images.empty())
      return;
    index = std::clamp(index, 0, (int)images.size() - 1);
    if (index == current)
      return;

    resetEditSession();
    current = index;
    currentDecodeFailed = false;
    prefetchAround(current);
    scrollThumbsToCurrent = true;

    if (fullCache.count(current))
      uploadCurrentFromCache();
    else if (uploadedIndex != current)
      loadingCurrent = true; // 等待解码，期间保留旧图
  }

  void selectImage(int index) {
    if (images.empty())
      return;
    index = std::clamp(index, 0, (int)images.size() - 1);
    if (index == current)
      return;
    if (editDirty) {
      pendingSelectIndex = index;
      openUnsavedPopup = true;
      return;
    }
    selectImageNow(index);
  }

  void setEditNotice(const std::string &message) {
    editNotice = message;
    editNoticeUntil = glfwGetTime() + 3.0;
    uiVisible = true;
  }

  void clearImportedBrushTips() {
    if (importedBrushTips.empty()) {
      activeBrushTip.reset();
      return;
    }
    renderer.waitIdle();
    for (ImportedBrushTip &item : importedBrushTips) {
      if (item.previewImGui != 0)
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)item.previewImGui);
      if (item.previewTexture != 0)
        renderer.destroyTexture(item.previewTexture);
    }
    importedBrushTips.clear();
    activeBrushTip.reset();
  }

  ImportedBrushTip makeImportedBrushTip(AbrBrushTip tip) {
    ImportedBrushTip item;
    item.tip = std::make_shared<AbrBrushTip>(std::move(tip));

    constexpr int previewSize = 64;
    constexpr float inset = 6.0f;
    std::vector<uint8_t> rgba(previewSize * previewSize * 4, 255);
    const AbrBrushTip &source = *item.tip;
    const float scale = std::min((previewSize - inset * 2.0f) / source.width,
                                 (previewSize - inset * 2.0f) / source.height);
    const float drawW = source.width * scale;
    const float drawH = source.height * scale;
    const float left = (previewSize - drawW) * 0.5f;
    const float top = (previewSize - drawH) * 0.5f;
    for (int y = 0; y < previewSize; ++y) {
      for (int x = 0; x < previewSize; ++x) {
        uint8_t *pixel = rgba.data() + ((size_t)y * previewSize + x) * 4;
        const uint8_t background = ((x / 8 + y / 8) & 1) ? 238 : 250;
        float alpha = 0.0f;
        if (x + 0.5f >= left && x + 0.5f < left + drawW &&
            y + 0.5f >= top && y + 0.5f < top + drawH) {
          const float u = (x + 0.5f - left) / drawW * source.width - 0.5f;
          const float v = (y + 0.5f - top) / drawH * source.height - 0.5f;
          const int sx = std::clamp((int)std::lround(u), 0, source.width - 1);
          const int sy = std::clamp((int)std::lround(v), 0, source.height - 1);
          alpha = source.mask[(size_t)sy * source.width + sx] / 255.0f;
        }
        const uint8_t value = (uint8_t)std::lround(
            background * (1.0f - alpha) + 32.0f * alpha);
        pixel[0] = pixel[1] = pixel[2] = value;
        pixel[3] = 255;
      }
    }

    try {
      item.previewTexture =
          renderer.createTexture(rgba.data(), previewSize, previewSize, false);
      VkImageView view;
      VkSampler sampler;
      if (renderer.getTextureInfo(item.previewTexture, view, sampler))
        item.previewImGui = (ImTextureID)ImGui_ImplVulkan_AddTexture(
            sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } catch (...) {
      // 极大画笔包即使预览纹理资源不足，仍保留笔尖供绘制使用。
      if (item.previewTexture != 0)
        renderer.destroyTexture(item.previewTexture);
      item.previewTexture = 0;
      item.previewImGui = 0;
    }
    return item;
  }

  fs::path brushLibraryDirectory() const {
#ifdef _WIN32
    wchar_t localAppData[32768]{};
    const DWORD capacity =
        (DWORD)(sizeof(localAppData) / sizeof(localAppData[0]));
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, capacity);
    if (length > 0 && length < capacity)
      return fs::path(localAppData) / L"ImageViewer" / L"Brushes";
#endif
    std::error_code ec;
    return fs::temp_directory_path(ec) / L"ImageViewer" / L"Brushes";
  }

  bool persistAbrToLibrary(const fs::path &source, fs::path &stored,
                           std::string &error) {
    std::error_code ec;
    const fs::path library = brushLibraryDirectory();
    fs::create_directories(library, ec);
    if (ec) {
      error = u8"无法创建本地画笔库";
      return false;
    }
    stored = library / source.filename();
    bool sameFile = false;
    if (fs::exists(stored, ec) && !ec)
      sameFile = fs::equivalent(source, stored, ec) && !ec;
    ec.clear();
    if (!sameFile)
      fs::copy_file(source, stored, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      error = u8"无法将 ABR 保存到本地画笔库";
      return false;
    }
    // 最近导入的包排在画笔库前面，并作为下次启动的默认笔尖。
    fs::last_write_time(stored, fs::file_time_type::clock::now(), ec);
    return true;
  }

  void loadPersistentBrushLibrary(const fs::path &preferredPack = {}) {
    const fs::path library = brushLibraryDirectory();
    std::error_code ec;
    fs::create_directories(library, ec);
    if (ec)
      return;

    std::vector<fs::path> packs;
    for (fs::directory_iterator it(library, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (!it->is_regular_file(ec))
        continue;
      std::string extension = it->path().extension().u8string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      if (extension == ".abr")
        packs.push_back(it->path());
    }
    std::sort(packs.begin(), packs.end(), [](const fs::path &a,
                                             const fs::path &b) {
      std::error_code ea, eb;
      const auto ta = fs::last_write_time(a, ea);
      const auto tb = fs::last_write_time(b, eb);
      if (!ea && !eb && ta != tb)
        return ta > tb;
      return a.filename().wstring() < b.filename().wstring();
    });

    clearImportedBrushTips();
    std::shared_ptr<AbrBrushTip> preferredTip;
    for (const fs::path &pack : packs) {
      AbrLoadResult loaded = loadAbrBrushFile(pack.wstring());
      if (!loaded.error.empty())
        continue; // 单个损坏包不能阻止其余画笔库启动。
      const bool preferred = !preferredPack.empty() &&
                             pack.filename() == preferredPack.filename();
      for (AbrBrushTip &tip : loaded.brushes) {
        importedBrushTips.push_back(makeImportedBrushTip(std::move(tip)));
        if (preferred && !preferredTip)
          preferredTip = importedBrushTips.back().tip;
      }
    }

    if (preferredTip)
      activeBrushTip = preferredTip;
    else if (!importedBrushTips.empty())
      activeBrushTip = importedBrushTips.front().tip;
    if (activeBrushTip) {
      brushDiameter = std::clamp(
          (float)std::max(activeBrushTip->width, activeBrushTip->height),
          1.0f, 256.0f);
      brushSpacing = activeBrushTip->spacing;
    }
  }

  void beginEditMode() {
    if (mainTex == 0 || current < 0 || !fullCache.count(current)) {
      setEditNotice(u8"图片尚未载入完成");
      return;
    }
    if (editIndex != current) {
      resetEditSession();
      editIndex = current;
    }
    if (editLayerTex == 0) {
      const DecodeResult &base = fullCache.at(current);
      if (base.w != imgW || base.h != imgH ||
          base.pixels.size() != (size_t)imgW * imgH * 4) {
        setEditNotice(u8"原图像素尺寸异常，无法进入绘制模式");
        return;
      }
      editLayerPixels = base.pixels;
      editLayerTex =
          renderer.createTexture(editLayerPixels.data(), imgW, imgH, false);
      dirtyMaxX = dirtyMaxY = -1;
      editLayerDirty = false;
    }
    editMode = true;
    uiVisible = true;
    setEditNotice(u8"已进入绘制模式 · 在图片上拖动鼠标开始标注");
  }

  void undoDraw() {
    if (drawHistoryCursor == 0)
      return;
    --drawHistoryCursor;
    rebuildEditLayer();
    editDirty = true;
  }

  void redoDraw() {
    if (drawHistoryCursor >= drawStrokes.size())
      return;
    ++drawHistoryCursor;
    rebuildEditLayer();
    editDirty = true;
  }

  bool screenToImage(float fx, float fy, DrawPoint &out) const {
    if (imgW <= 0 || imgH <= 0 || scale <= 0.0f)
      return false;
    int winW, winH;
    renderer.extent(winW, winH);
    const float dx = (fx - (winW * 0.5f + panX)) / scale;
    const float dy = (fy - (mainAreaCenterY() + panY)) / scale;
    const float r = rotationDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(r), sn = std::sin(r);
    out.x = dx * cs - dy * sn + imgW * 0.5f;
    out.y = dx * sn + dy * cs + imgH * 0.5f;
    return out.x >= 0.0f && out.y >= 0.0f && out.x < imgW && out.y < imgH;
  }

  void markLayerDirty(int x0, int y0, int x1, int y1) {
    x0 = std::clamp(x0, 0, std::max(0, imgW - 1));
    y0 = std::clamp(y0, 0, std::max(0, imgH - 1));
    x1 = std::clamp(x1, 0, std::max(0, imgW - 1));
    y1 = std::clamp(y1, 0, std::max(0, imgH - 1));
    if (x0 > x1 || y0 > y1)
      return;
    if (!editLayerDirty) {
      dirtyMinX = x0;
      dirtyMinY = y0;
      dirtyMaxX = x1;
      dirtyMaxY = y1;
      editLayerDirty = true;
    } else {
      dirtyMinX = std::min(dirtyMinX, x0);
      dirtyMinY = std::min(dirtyMinY, y0);
      dirtyMaxX = std::max(dirtyMaxX, x1);
      dirtyMaxY = std::max(dirtyMaxY, y1);
    }
  }

  void paintLayerPixel(int x, int y, const DrawStroke &stroke,
                       float coverage) {
    if (x < 0 || y < 0 || x >= imgW || y >= imgH || coverage <= 0.0f)
      return;
    uint8_t *d = editLayerPixels.data() + ((size_t)y * imgW + x) * 4;
    const float amount =
        std::clamp(stroke.color[3] * coverage, 0.0f, 1.0f);
    if (stroke.tool == DrawTool::Eraser) {
      const float newAlpha = (d[3] / 255.0f) * (1.0f - amount);
      d[3] = (uint8_t)std::lround(newAlpha * 255.0f);
      if (d[3] == 0)
        d[0] = d[1] = d[2] = 0;
      return;
    }

    const float da = d[3] / 255.0f;
    const float oa = amount + da * (1.0f - amount);
    if (oa <= 1e-6f) {
      d[0] = d[1] = d[2] = d[3] = 0;
      return;
    }
    for (int c = 0; c < 3; ++c) {
      const float dc = d[c] / 255.0f;
      const float out =
          (stroke.color[c] * amount + dc * da * (1.0f - amount)) / oa;
      d[c] = (uint8_t)std::lround(std::clamp(out, 0.0f, 1.0f) * 255.0f);
    }
    d[3] = (uint8_t)std::lround(std::clamp(oa, 0.0f, 1.0f) * 255.0f);
  }

  void stampLayerBrush(const DrawPoint &p, const DrawStroke &stroke) {
    const float radius = std::max(0.5f, stroke.radius);
    if (stroke.brushTip && stroke.brushTip->width > 0 &&
        stroke.brushTip->height > 0 && !stroke.brushTip->mask.empty()) {
      const AbrBrushTip &tip = *stroke.brushTip;
      const float diameter = radius * 2.0f;
      const float maskScale =
          diameter / std::max(1.0f, float(std::max(tip.width, tip.height)));
      const float stampW = std::max(1.0f, tip.width * maskScale);
      const float stampH = std::max(1.0f, tip.height * maskScale);
      const float left = p.x - stampW * 0.5f;
      const float top = p.y - stampH * 0.5f;
      const int x0 = (int)std::floor(left - 1.0f);
      const int x1 = (int)std::ceil(left + stampW + 1.0f);
      const int y0 = (int)std::floor(top - 1.0f);
      const int y1 = (int)std::ceil(top + stampH + 1.0f);

      auto sampleMask = [&](float u, float v) {
        u = std::clamp(u, 0.0f, float(tip.width - 1));
        v = std::clamp(v, 0.0f, float(tip.height - 1));
        const int ax = (int)std::floor(u), ay = (int)std::floor(v);
        const int bx = std::min(ax + 1, tip.width - 1);
        const int by = std::min(ay + 1, tip.height - 1);
        const float tx = u - ax, ty = v - ay;
        const float a = tip.mask[(size_t)ay * tip.width + ax] / 255.0f;
        const float b = tip.mask[(size_t)ay * tip.width + bx] / 255.0f;
        const float c = tip.mask[(size_t)by * tip.width + ax] / 255.0f;
        const float d = tip.mask[(size_t)by * tip.width + bx] / 255.0f;
        return (a + (b - a) * tx) * (1.0f - ty) +
               (c + (d - c) * tx) * ty;
      };

      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const float nx = (x + 0.5f - left) / stampW;
          const float ny = (y + 0.5f - top) / stampH;
          if (nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f)
            continue;
          const float u = nx * tip.width - 0.5f;
          const float v = ny * tip.height - 0.5f;
          paintLayerPixel(x, y, stroke, sampleMask(u, v));
        }
      }
      markLayerDirty(x0, y0, x1, y1);
      return;
    }

    const float inner = radius * std::clamp(stroke.hardness, 0.0f, 1.0f);
    const int x0 = (int)std::floor(p.x - radius - 1.0f);
    const int x1 = (int)std::ceil(p.x + radius + 1.0f);
    const int y0 = (int)std::floor(p.y - radius - 1.0f);
    const int y1 = (int)std::ceil(p.y + radius + 1.0f);
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const float dx = x + 0.5f - p.x;
        const float dy = y + 0.5f - p.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        float coverage = 0.0f;
        if (distance <= inner)
          coverage = 1.0f;
        else
          coverage = std::clamp(
              (radius + 0.5f - distance) /
                  std::max(0.5f, radius - inner + 0.5f),
              0.0f, 1.0f);
        paintLayerPixel(x, y, stroke, coverage);
      }
    }
    markLayerDirty(x0, y0, x1, y1);
  }

  void rasterizeStrokeSegment(const DrawStroke &stroke, const DrawPoint &a,
                              const DrawPoint &b, bool includeStart) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    const float step = std::max(
        0.35f, stroke.radius * 2.0f * std::clamp(stroke.spacing, 0.03f, 1.0f));
    const int steps = std::max(1, (int)std::ceil(length / step));
    const int first = includeStart ? 0 : 1;
    for (int i = first; i <= steps; ++i) {
      const float t = (float)i / steps;
      stampLayerBrush({a.x + dx * t, a.y + dy * t}, stroke);
    }
  }

  void rasterizeStroke(const DrawStroke &stroke) {
    if (stroke.tool == DrawTool::Clear) {
      const auto base = fullCache.find(editIndex);
      if (base != fullCache.end() &&
          base->second.pixels.size() == editLayerPixels.size())
        editLayerPixels = base->second.pixels;
      else
        std::fill(editLayerPixels.begin(), editLayerPixels.end(), 0);
      markLayerDirty(0, 0, imgW - 1, imgH - 1);
      return;
    }
    if (stroke.points.empty())
      return;
    stampLayerBrush(stroke.points.front(), stroke);
    for (size_t i = 1; i < stroke.points.size(); ++i)
      rasterizeStrokeSegment(stroke, stroke.points[i - 1], stroke.points[i],
                             false);
  }

  void rebuildEditLayer() {
    if (editLayerPixels.empty())
      return;
    const auto base = fullCache.find(editIndex);
    if (base != fullCache.end() &&
        base->second.pixels.size() == editLayerPixels.size())
      editLayerPixels = base->second.pixels;
    else
      std::fill(editLayerPixels.begin(), editLayerPixels.end(), 0);
    markLayerDirty(0, 0, imgW - 1, imgH - 1);
    const size_t count = std::min(drawHistoryCursor, drawStrokes.size());
    for (size_t i = 0; i < count; ++i)
      rasterizeStroke(drawStrokes[i]);
  }

  void flushEditLayerTexture() {
    if (!editLayerDirty || editLayerTex == 0 || dirtyMaxX < dirtyMinX ||
        dirtyMaxY < dirtyMinY)
      return;
    renderer.updateTextureRegion(editLayerTex, dirtyMinX, dirtyMinY,
                                 dirtyMaxX - dirtyMinX + 1,
                                 dirtyMaxY - dirtyMinY + 1,
                                 editLayerPixels.data(), imgW * 4);
    editLayerDirty = false;
    dirtyMaxX = dirtyMaxY = -1;
  }

  void appendDrawCommand(DrawStroke stroke) {
    if (drawHistoryCursor < drawStrokes.size())
      drawStrokes.erase(drawStrokes.begin() + (ptrdiff_t)drawHistoryCursor,
                        drawStrokes.end());
    drawStrokes.push_back(std::move(stroke));
    drawHistoryCursor = drawStrokes.size();
  }

  void beginDrawInput(float fx, float fy) {
    DrawPoint p;
    if (!screenToImage(fx, fy, p))
      return;
    drawingStroke = true;
    editDirty = true;
    DrawStroke stroke;
    stroke.points.push_back(p);
    for (int c = 0; c < 4; ++c)
      stroke.color[c] = brushColor[c];
    stroke.radius = brushDiameter * 0.5f;
    stroke.hardness = brushHardness;
    stroke.spacing = brushSpacing;
    stroke.tool = drawTool;
    stroke.brushTip = activeBrushTip;
    appendDrawCommand(std::move(stroke));
    rasterizeStroke(drawStrokes[drawHistoryCursor - 1]);
  }

  void continueDrawInput(float fx, float fy) {
    if (!drawingStroke)
      return;
    DrawPoint p;
    if (!screenToImage(fx, fy, p))
      return;
    if (drawHistoryCursor == 0 || drawHistoryCursor > drawStrokes.size())
      return;
    DrawStroke &stroke = drawStrokes[drawHistoryCursor - 1];
    const DrawPoint &last = stroke.points.back();
    const float dx = p.x - last.x, dy = p.y - last.y;
    const float minStep = std::max(0.25f, stroke.radius * stroke.spacing * 0.5f);
    if (dx * dx + dy * dy >= minStep * minStep) {
      rasterizeStrokeSegment(stroke, last, p, false);
      stroke.points.push_back(p);
    }
  }

  bool saveEditedCopy(bool chooseNewPath) {
    if (editIndex != current || !fullCache.count(current)) {
      setEditNotice(u8"无法取得当前图片像素");
      return false;
    }
    fs::path output = lastEditSavePath;
    if (chooseNewPath || output.empty()) {
      fs::path source = images[current].path;
      fs::path suggested =
          source.parent_path() / (source.stem().wstring() + L"_标注.png");
      std::wstring selected = savePngDialog(nativeWindow, suggested.wstring());
      if (selected.empty())
        return false;
      output = fs::path(selected);
    }

    const DecodeResult &base = fullCache.at(current);
    if (editLayerPixels.size() != base.pixels.size()) {
      setEditNotice(u8"编辑画布尺寸异常，无法保存");
      return false;
    }
    if (!saveRgbaPng(output.wstring(), editLayerPixels.data(), base.w,
                     base.h)) {
      setEditNotice(u8"保存失败，请检查目标路径是否可写");
      return false;
    }
    lastEditSavePath = output;
    editDirty = false;
    setEditNotice(u8"已保存 PNG 副本：" + output.filename().u8string());
    return true;
  }

  void exportBrushPreset() {
    fs::path suggested = images.empty()
                             ? fs::path(L"我的画笔.ivbrush")
                             : images[current].path.parent_path() /
                                   fs::path(L"我的画笔.ivbrush");
    std::wstring selected =
        saveBrushPresetDialog(nativeWindow, suggested.wstring());
    if (selected.empty())
      return;
    std::ofstream out(fs::path(selected), std::ios::binary);
    if (!out) {
      setEditNotice(u8"画笔预设导出失败");
      return;
    }
    out << "ImageViewerBrush=1\n";
    out << "diameter=" << brushDiameter << '\n';
    out << "hardness=" << brushHardness << '\n';
    out << "spacing=" << brushSpacing << '\n';
    out << "color=" << brushColor[0] << ',' << brushColor[1] << ','
        << brushColor[2] << ',' << brushColor[3] << '\n';
    out.close();
    setEditNotice(u8"已导出画笔：" + fs::path(selected).filename().u8string());
  }

  void importBrushPreset() {
    std::wstring selected = openBrushPresetDialog(nativeWindow);
    if (selected.empty())
      return;
    fs::path selectedPath(selected);
    std::string extension = selectedPath.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".abr") {
      AbrLoadResult loaded = loadAbrBrushFile(selected);
      if (!loaded.error.empty()) {
        setEditNotice(u8"ABR 导入失败：" + loaded.error);
        return;
      }
      const size_t importedCount = loaded.brushes.size();
      fs::path stored;
      std::string persistError;
      if (!persistAbrToLibrary(selectedPath, stored, persistError)) {
        clearImportedBrushTips();
        importedBrushTips.reserve(importedCount);
        for (AbrBrushTip &tip : loaded.brushes)
          importedBrushTips.push_back(makeImportedBrushTip(std::move(tip)));
        activeBrushTip = importedBrushTips.front().tip;
        brushDiameter = std::clamp(
            (float)std::max(activeBrushTip->width, activeBrushTip->height),
            1.0f, 256.0f);
        brushSpacing = activeBrushTip->spacing;
        setEditNotice(u8"ABR 已用于本次运行，但" + persistError);
      } else {
        loadPersistentBrushLibrary(stored);
        setEditNotice(u8"已导入并保存到画笔库：" +
                      std::to_string(importedCount) + u8" 个笔尖");
      }
      drawTool = DrawTool::Pen;
      return;
    }
    std::ifstream in(fs::path(selected), std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line != "ImageViewerBrush=1") {
      setEditNotice(u8"无法识别该画笔预设");
      return;
    }
    float diameter = brushDiameter;
    float hardness = brushHardness;
    float spacing = brushSpacing;
    float color[4] = {brushColor[0], brushColor[1], brushColor[2],
                      brushColor[3]};
    while (std::getline(in, line)) {
      try {
        if (line.rfind("diameter=", 0) == 0)
          diameter = std::stof(line.substr(9));
        else if (line.rfind("hardness=", 0) == 0)
          hardness = std::stof(line.substr(9));
        else if (line.rfind("spacing=", 0) == 0)
          spacing = std::stof(line.substr(8));
        else if (line.rfind("color=", 0) == 0)
          sscanf_s(line.c_str() + 6, "%f,%f,%f,%f", &color[0], &color[1],
                   &color[2], &color[3]);
      } catch (...) {
        setEditNotice(u8"画笔预设内容损坏");
        return;
      }
    }
    brushDiameter = std::clamp(diameter, 1.0f, 256.0f);
    brushHardness = std::clamp(hardness, 0.0f, 1.0f);
    brushSpacing = std::clamp(spacing, 0.03f, 1.0f);
    for (int c = 0; c < 4; ++c)
      brushColor[c] = std::clamp(color[c], 0.0f, 1.0f);
    activeBrushTip.reset();
    drawTool = DrawTool::Pen;
    setEditNotice(u8"已导入画笔：" + fs::path(selected).filename().u8string());
  }

  // -----------------------------------------------------------------
  // 输入处理（ImGui 通过回调链收到事件；这里按 io.WantCapture* 过滤）
  // -----------------------------------------------------------------

  // 鼠标坐标（窗口系）-> 帧缓冲像素
  void mouseFb(double &x, double &y) {
    int winW, winH, fbW, fbH;
    glfwGetWindowSize(window, &winW, &winH);
    renderer.extent(fbW, fbH);
    x = cursorX * fbW / std::max(1, winW);
    y = cursorY * fbH / std::max(1, winH);
  }

  void onKey(int key, int action, int mods) {
    if (action == GLFW_PRESS)
      lastInputTime = glfwGetTime();

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
      if (editMode) {
        editMode = false;
        drawingStroke = false;
        setEditNotice(editDirty ? u8"已退出绘制；标注尚未保存"
                                : u8"已退出绘制模式");
      } else if (fullscreen) {
        glfwSetWindowMonitor(window, nullptr, 100, 100, WIDTH, HEIGHT, 0);
        fullscreen = false;
      } else {
        if (editDirty) {
          pendingSelectIndex = -2;
          openUnsavedPopup = true;
        } else {
          glfwSetWindowShouldClose(window, true);
        }
      }
      return;
    }

    // 绘制模式快捷键：Ctrl+S 保存，Ctrl+Z/Y 撤销/重做。
    if (action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL) &&
        editIndex == current) {
      if (key == GLFW_KEY_Z) {
        if (mods & GLFW_MOD_SHIFT)
          redoDraw();
        else
          undoDraw();
        return;
      }
      if (key == GLFW_KEY_Y) {
        redoDraw();
        return;
      }
      if (key == GLFW_KEY_S) {
        saveEditedCopy(false);
        return;
      }
    }

    // Ctrl+O / Ctrl+Shift+O
    if (action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL) &&
        key == GLFW_KEY_O) {
      openFromDialog((mods & GLFW_MOD_SHIFT) != 0);
      return;
    }

    // 所有按键都刷新 UI 可见状态
    if (action == GLFW_PRESS)
      uiVisible = true;

    if (ImGui::GetIO().WantCaptureKeyboard)
      return;
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
      return;

    switch (key) {
    case GLFW_KEY_E:
      if (editMode) {
        drawTool = DrawTool::Eraser;
      } else {
        beginEditMode();
      }
      break;
    case GLFW_KEY_B:
      if (editMode)
        drawTool = DrawTool::Pen;
      break;
    case GLFW_KEY_LEFT:
    case GLFW_KEY_A:
      selectImage(current - 1);
      break;
    case GLFW_KEY_RIGHT:
    case GLFW_KEY_D:
      selectImage(current + 1);
      break;
    case GLFW_KEY_HOME:
      selectImage(0);
      break;
    case GLFW_KEY_END:
      selectImage((int)images.size() - 1);
      break;
    case GLFW_KEY_F:
      fitToWindow();
      break;
    case GLFW_KEY_1:
      if (mainTex) {
        scale = 1.0f;
        panX = panY = 0;
      }
      break;
    case GLFW_KEY_0:
      fitToWindow();
      break;
    case GLFW_KEY_EQUAL: // 主键盘 +
    case GLFW_KEY_KP_ADD:
      zoomAt(WIDTH * 0.5f, HEIGHT * 0.5f, 1.2f);
      break;
    case GLFW_KEY_MINUS: // 主键盘 -
    case GLFW_KEY_KP_SUBTRACT:
      zoomAt(WIDTH * 0.5f, HEIGHT * 0.5f, 1.0f / 1.2f);
      break;
    case GLFW_KEY_I: // 信息面板开关
      showInfoPanel = !showInfoPanel;
      break;
    case GLFW_KEY_T: // 缩略图条开关
      showThumbsBar = !showThumbsBar;
      break;
    case GLFW_KEY_SPACE: // 放大增强开关
      sharpenEnabled = !sharpenEnabled;
      break;
    case GLFW_KEY_H: // 隐藏所有 UI（Immersive 模式）
      uiVisible = !uiVisible;
      break;
    case GLFW_KEY_F11: // 全屏切换
      fullscreen = !fullscreen;
      if (fullscreen) {
        int mx, my;
        glfwGetMonitorPos(glfwGetPrimaryMonitor(), &mx, &my);
        const GLFWvidmode *vm = glfwGetVideoMode(glfwGetPrimaryMonitor());
        glfwSetWindowMonitor(window, glfwGetPrimaryMonitor(), mx, my, vm->width,
                             vm->height, vm->refreshRate);
      } else {
        glfwSetWindowMonitor(window, nullptr, 100, 100, WIDTH, HEIGHT, 0);
      }
      break;
    }
  }

  bool inFloatingToolbarArea(double my, int winH) const {
    // 底部悬浮胶囊区域（胶囊本身 + thumbs 条）
    float thumbsTop =
        showThumbsBar ? ((float)winH - THUMB_PANEL_H) : (float)winH;
    float tbTop = thumbsTop - FLOAT_TB_GAP - FLOAT_TB_H;
    return my >= tbTop;
  }

  void onScroll(double yoff) {
    double mx, my;
    mouseFb(mx, my);
    int winW, winH;
    renderer.extent(winW, winH);
    if (inFloatingToolbarArea(my, winH)) {
      // thumbs 区域 → 滚缩略图；胶囊区域让 ImGui 处理按钮
      if (showThumbsBar && my >= (float)winH - THUMB_PANEL_H) {
        pendingThumbScroll += (float)yoff;
      }
      return;
    }
    // 下拉列表、滑块等 ImGui 控件已接收滚轮时，禁止继续传给画布缩放。
    if (brushUiHovered || ImGui::GetIO().WantCaptureMouse)
      return;
    zoomAt((float)mx, (float)my, std::pow(1.15f, (float)yoff));
  }

  void onMouseButton(int button, int action) {
    if (button != GLFW_MOUSE_BUTTON_LEFT)
      return;

    double mx, my;
    mouseFb(mx, my);

    if (editMode) {
      if (ImGui::GetIO().WantCaptureMouse)
        return;
      if (action == GLFW_PRESS)
        beginDrawInput((float)mx, (float)my);
      else if (action == GLFW_RELEASE)
        drawingStroke = false;
      return;
    }

    int winW, winH;
    renderer.extent(winW, winH);

    if (inFloatingToolbarArea(my, winH)) {
      // thumbs 区域：拖拽缩略图
      if (showThumbsBar && my >= (float)winH - THUMB_PANEL_H) {
        if (action == GLFW_PRESS && !thumbDragging) {
          thumbDragging = true;
          thumbDragStartX = mx;
        } else if (action == GLFW_RELEASE) {
          thumbDragging = false;
        }
      }
      // 胶囊区域：交给 ImGui 处理按钮点击
      if (ImGui::GetIO().WantCaptureMouse)
        dragging = false;
      return;
    }

    // 主图区域：拖动 / 缩放
    if (action == GLFW_PRESS) {
      dragging = true;
      lastMouseX = mx;
      lastMouseY = my;
      double now = glfwGetTime();
      if (now - lastClickTime < 0.35) {
        // 双击在“实际像素”和“适应窗口”之间切换，比单向重置更符合浏览器习惯。
        if (std::abs(scale - 1.0f) < 0.01f)
          fitToWindow();
        else {
          scale = 1.0f;
          panX = panY = 0.0f;
        }
        dragging = false;
      }
      lastClickTime = now;
    } else if (action == GLFW_RELEASE) {
      dragging = false;
    }
  }

  void onCursorPos(double x, double y) {
    cursorX = x;
    cursorY = y;

    double mx, my;
    mouseFb(mx, my);

    if (editMode && drawingStroke) {
      continueDrawInput((float)mx, (float)my);
      return;
    }

    // thumbs 拖拽：相对增量 → 累加到 pendingThumbScroll
    // （鼠标向右拖 → 内容向右 → ScrollX 增大）
    if (thumbDragging) {
      // 鼠标左拖 → 内容跟着左走 → ScrollX 减小（方向相反）
      thumbDragScrollDelta += (float)(thumbDragStartX - mx);
      thumbDragStartX = mx;
      return;
    }

    if (dragging && mainTex != 0) {
      panX += (float)(mx - lastMouseX);
      panY += (float)(my - lastMouseY);
      lastMouseX = mx;
      lastMouseY = my;
    }
  }

  void drawEditOverlay() {
    if (editIndex != current || mainTex == 0)
      return;

    int winW, winH;
    renderer.extent(winW, winH);
    const ImVec2 uiSize = ImGui::GetIO().DisplaySize;
    const float uiScaleX = uiSize.x / std::max(1, winW);
    const float uiScaleY = uiSize.y / std::max(1, winH);
    const float uiScale = (uiScaleX + uiScaleY) * 0.5f;
    const float displayW = displayedImageWidth() * scale;
    const float displayH = displayedImageHeight() * scale;
    const float left = winW * 0.5f + panX - displayW * 0.5f;
    const float top = mainAreaCenterY() + panY - displayH * 0.5f;
    ImVec2 clipMin(std::max(0.0f, left) * uiScaleX,
                   std::max(mainAreaTop(), top) * uiScaleY);
    ImVec2 clipMax(std::min((float)winW, left + displayW) * uiScaleX,
                   std::min(mainAreaBottom(), top + displayH) * uiScaleY);
    if (clipMin.x >= clipMax.x || clipMin.y >= clipMax.y)
      return;

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(clipMin, clipMax, true);
    if (editMode) {
      double mx, my;
      mouseFb(mx, my);
      DrawPoint imagePoint;
      if (screenToImage((float)mx, (float)my, imagePoint)) {
        const ImU32 cursorColor = drawTool == DrawTool::Eraser
                                      ? IM_COL32(55, 55, 55, 230)
                                      : ImGui::ColorConvertFloat4ToU32(ImVec4(
                                            brushColor[0], brushColor[1],
                                            brushColor[2], 0.95f));
        dl->AddCircle(ImVec2((float)mx * uiScaleX, (float)my * uiScaleY),
                      std::max(3.0f,
                               brushDiameter * 0.5f * scale * uiScale),
                      cursorColor, 32, 1.5f);
      }
      dl->AddRect(clipMin, clipMax, IM_COL32(24, 144, 255, 190), 2.0f, 0,
                  1.5f);
    }
    dl->PopClipRect();
  }

  // -----------------------------------------------------------------
  // ImGui 界面（工具栏 + 缩略图面板）
  // -----------------------------------------------------------------

  void buildImGuiUI() {
    brushUiHovered = false;
    int fbW, fbH;
    renderer.extent(fbW, fbH);
    const ImVec2 uiSize = ImGui::GetIO().DisplaySize;
    const int winW = std::max(1, (int)uiSize.x);
    const int winH = std::max(1, (int)uiSize.y);
    float cy = mainAreaCenterY() * winH / std::max(1, fbH);

    double mx = cursorX, my = cursorY;
    static double lastMx = -1, lastMy = -1;
    if (std::abs(mx - lastMx) > 0.5 || std::abs(my - lastMy) > 0.5) {
      lastMx = mx;
      lastMy = my;
      lastInputTime = glfwGetTime();
    }
    if (!uiVisible ||
        (fullscreen && !editMode &&
         glfwGetTime() - lastInputTime > UI_AUTO_HIDE_SEC))
      return;

    auto noDeco = [] {
      return ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
             ImGuiWindowFlags_NoSavedSettings |
             ImGuiWindowFlags_NoBringToFrontOnFocus;
    };

    // === 空白页：提供明确的第一步，而不是显示一排不可用的工具按钮 ===
    if (images.empty()) {
      const ImVec2 cardSize(440.0f, 220.0f);
      ImGui::SetNextWindowPos(ImVec2(winW * 0.5f, winH * 0.46f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(cardSize, ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.96f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
      ImGui::Begin("##welcome", nullptr, noDeco());
      ImGui::SetWindowFontScale(1.25f);
      ImGui::TextUnformatted(u8"图片浏览器");
      ImGui::SetWindowFontScale(1.0f);
      ImGui::Spacing();
      ImGui::TextDisabled(u8"拖入图片或文件夹，也可以从这里开始");
      ImGui::Dummy(ImVec2(0.0f, 24.0f));
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(0.094f, 0.565f, 0.965f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.149f, 0.612f, 0.976f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
      if (ImGui::Button(u8"打开图片", ImVec2(176.0f, 42.0f)))
        openFromDialog(false);
      ImGui::SameLine(0.0f, 14.0f);
      if (ImGui::Button(u8"打开文件夹", ImVec2(176.0f, 42.0f)))
        openFromDialog(true);
      ImGui::PopStyleColor(3);
      ImGui::Dummy(ImVec2(0.0f, 10.0f));
      ImGui::TextDisabled(u8"快捷键：Ctrl+O 打开图片 · Ctrl+Shift+O 打开文件夹");
      ImGui::End();
      ImGui::PopStyleVar(2);
      return;
    }

    // === 绘制模式工具面板 ===
    if (editMode && editIndex == current) {
      const float panelW = 340.0f;
      ImGui::SetNextWindowPos(ImVec2((float)winW - panelW - 16.0f, 74.0f),
                              ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(panelW, 502.0f), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.97f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
      ImGui::Begin("##drawpanel", nullptr, noDeco());
      ImGui::SetWindowFontScale(1.12f);
      ImGui::TextUnformatted(editDirty ? u8"绘制模式  ·  未保存"
                                       : u8"绘制模式");
      ImGui::SetWindowFontScale(1.0f);
      ImGui::TextDisabled(u8"B 画笔 · E 橡皮 · Esc 退出 · Ctrl+S 保存");
      ImGui::Spacing();

      auto toolButton = [&](const char *label, DrawTool tool) {
        const bool active = drawTool == tool;
        if (active)
          ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.094f, 0.565f, 0.965f, 0.95f));
        if (active)
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        bool pressed = ImGui::Button(label, ImVec2(116.0f, 34.0f));
        if (active)
          ImGui::PopStyleColor(2);
        if (pressed)
          drawTool = tool;
      };
      toolButton(u8"画笔", DrawTool::Pen);
      ImGui::SameLine();
      toolButton(u8"橡皮擦", DrawTool::Eraser);
      if (drawTool == DrawTool::Eraser && ImGui::IsItemHovered())
        ImGui::SetTooltip(
            u8"像 Photoshop 橡皮擦一样擦成透明；原图与笔画都会被擦除");

      ImGui::TextDisabled(u8"颜色");
      ImGui::SameLine(70.0f);
      ImGui::SetNextItemWidth(170.0f);
      ImGui::ColorEdit4("##brushcolor", brushColor,
                        ImGuiColorEditFlags_NoInputs |
                            ImGuiColorEditFlags_AlphaBar);
      ImGui::TextDisabled(u8"粗细");
      ImGui::SameLine(70.0f);
      ImGui::SetNextItemWidth(170.0f);
      ImGui::SliderFloat("##brushsize", &brushDiameter, 1.0f, 256.0f,
                         "%.0f px");
      ImGui::TextDisabled(u8"硬度");
      ImGui::SameLine(70.0f);
      ImGui::SetNextItemWidth(170.0f);
      float hardnessPercent = brushHardness * 100.0f;
      ImGui::BeginDisabled(activeBrushTip != nullptr);
      if (ImGui::SliderFloat("##brushhardness", &hardnessPercent, 0.0f,
                             100.0f, "%.0f%%"))
        brushHardness = hardnessPercent / 100.0f;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(activeBrushTip
                              ? u8"ABR 笔尖自带灰度与软硬边缘"
                              : u8"0% 柔边 · 100% 硬边");
      ImGui::TextDisabled(u8"间距");
      ImGui::SameLine(70.0f);
      ImGui::SetNextItemWidth(170.0f);
      ImGui::SliderFloat("##brushspacing", &brushSpacing, 0.03f, 1.0f,
                         "%.2f×");

      ImGui::TextDisabled(u8"笔尖");
      ImGui::SameLine(70.0f);
      const ImportedBrushTip *activeItem = nullptr;
      for (const ImportedBrushTip &item : importedBrushTips) {
        if (item.tip == activeBrushTip) {
          activeItem = &item;
          break;
        }
      }
      if (activeItem && activeItem->previewImGui != 0) {
        ImGui::Image(activeItem->previewImGui, ImVec2(40.0f, 40.0f));
      } else {
        ImGui::Button(u8"●##round_tip_preview", ImVec2(40.0f, 40.0f));
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(194.0f);
      ImGui::SetNextWindowSizeConstraints(ImVec2(390.0f, 150.0f),
                                          ImVec2(460.0f, 390.0f));
      const char *tipPreview =
          activeBrushTip ? activeBrushTip->name.c_str() : u8"圆形笔尖";
      if (ImGui::BeginCombo("##brushtip", tipPreview)) {
        const bool roundSelected = activeBrushTip == nullptr;
        if (ImGui::Selectable(u8"●  圆形笔尖", roundSelected,
                              ImGuiSelectableFlags_None,
                              ImVec2(0.0f, 42.0f)))
          activeBrushTip.reset();
        if (roundSelected)
          ImGui::SetItemDefaultFocus();
        ImGui::Separator();
        for (size_t tipIndex = 0; tipIndex < importedBrushTips.size();
             ++tipIndex) {
          const ImportedBrushTip &item = importedBrushTips[tipIndex];
          const auto &tip = item.tip;
          const bool selected = activeBrushTip == tip;
          ImGui::PushID((int)tipIndex);
          bool choose = false;
          if (item.previewImGui != 0)
            choose = ImGui::ImageButton("##shape", item.previewImGui,
                                        ImVec2(48.0f, 48.0f));
          else
            choose = ImGui::Button("?##shape", ImVec2(48.0f, 48.0f));
          const bool previewHovered = ImGui::IsItemHovered();
          ImGui::SameLine();
          std::string label = tip->name + "\n" +
                              std::to_string(tip->width) + " × " +
                              std::to_string(tip->height) + " px##abr_tip_" +
                              std::to_string(tipIndex);
          if (ImGui::Selectable(label.c_str(), selected,
                                ImGuiSelectableFlags_None,
                                ImVec2(300.0f, 48.0f)))
            choose = true;
          if (previewHovered || ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%d × %d px", tip->name.c_str(), tip->width,
                              tip->height);
          if (choose) {
            activeBrushTip = tip;
            brushDiameter = std::clamp(
                (float)std::max(tip->width, tip->height), 1.0f, 256.0f);
            brushSpacing = tip->spacing;
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
          ImGui::PopID();
        }
        brushUiHovered =
            brushUiHovered || ImGui::IsWindowHovered(
                                  ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::EndCombo();
      }

      ImGui::BeginDisabled(drawHistoryCursor == 0);
      if (ImGui::Button(u8"撤销", ImVec2(70, 32)))
        undoDraw();
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(drawHistoryCursor >= drawStrokes.size());
      if (ImGui::Button(u8"重做", ImVec2(70, 32)))
        redoDraw();
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(drawHistoryCursor == 0);
      if (ImGui::Button(u8"还原", ImVec2(70, 32))) {
        DrawStroke clearCommand;
        clearCommand.tool = DrawTool::Clear;
        appendDrawCommand(std::move(clearCommand));
        rasterizeStroke(drawStrokes[drawHistoryCursor - 1]);
        editDirty = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"恢复进入绘制时的原图（可撤销）");
      ImGui::EndDisabled();

      if (ImGui::Button(u8"导入画笔", ImVec2(116.0f, 32.0f)))
        importBrushPreset();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            u8"支持 Photoshop ABR v1/v2/v6/v10；ABR 会自动保存到本地画笔库");
      ImGui::SameLine();
      if (ImGui::Button(u8"导出画笔", ImVec2(116.0f, 32.0f)))
        exportBrushPreset();

      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(0.094f, 0.565f, 0.965f, 0.95f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.149f, 0.612f, 0.976f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
      if (ImGui::Button(u8"保存 PNG", ImVec2(116.0f, 36.0f)))
        saveEditedCopy(false);
      ImGui::SameLine();
      if (ImGui::Button(u8"另存为", ImVec2(116.0f, 36.0f)))
        saveEditedCopy(true);
      ImGui::PopStyleColor(3);
      if (ImGui::Button(u8"完成绘制", ImVec2(238.0f, 34.0f))) {
        editMode = false;
        drawingStroke = false;
      }
      brushUiHovered =
          brushUiHovered || ImGui::IsWindowHovered(
                                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
      ImGui::End();
      ImGui::PopStyleVar(2);
    }

    // === 左右导航箭头（鼠标近才淡入）===
    if (images.size() > 1 && mainTex != 0) {
      float edgeThresh = 60.0f;
      float fade = 0.0f;
      if ((float)mx < edgeThresh)
        fade = (edgeThresh - (float)mx) / edgeThresh;
      if ((float)mx > (float)winW - edgeThresh)
        fade = ((float)mx - ((float)winW - edgeThresh)) / edgeThresh;
      fade = std::min(1.0f, fade);

      if (fade > 0.1f && current > 0) {
        float btnR = 28.0f;
        float cx = btnR + 16.0f;
        float cy2 = cy;
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        dl->AddCircleFilled(ImVec2(cx, cy2), btnR,
                            IM_COL32(0, 0, 0, (unsigned char)(fade * 170)),
                            32);
        dl->AddCircleFilled(
            ImVec2(cx, cy2), btnR - 1,
            IM_COL32(255, 255, 255, (unsigned char)(fade * 210)), 32);
        float t = 11.0f;
        dl->AddTriangleFilled(
            ImVec2(cx - t * 0.5f, cy2), ImVec2(cx + t * 0.4f, cy2 - t),
            ImVec2(cx + t * 0.4f, cy2 + t),
            IM_COL32(50, 50, 50, (unsigned char)(fade * 230)));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          float dx = (float)mx - cx, dy = (float)my - cy2;
          if (dx * dx + dy * dy < btnR * btnR)
            selectImage(current - 1);
        }
      }
      if (fade > 0.1f && current < (int)images.size() - 1) {
        float btnR = 28.0f;
        float cx = (float)winW - btnR - 16.0f;
        float cy2 = cy;
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        dl->AddCircleFilled(ImVec2(cx, cy2), btnR,
                            IM_COL32(0, 0, 0, (unsigned char)(fade * 170)),
                            32);
        dl->AddCircleFilled(
            ImVec2(cx, cy2), btnR - 1,
            IM_COL32(255, 255, 255, (unsigned char)(fade * 210)), 32);
        float t = 11.0f;
        dl->AddTriangleFilled(
            ImVec2(cx + t * 0.5f, cy2), ImVec2(cx - t * 0.4f, cy2 - t),
            ImVec2(cx - t * 0.4f, cy2 + t),
            IM_COL32(50, 50, 50, (unsigned char)(fade * 230)));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          float dx = (float)mx - cx, dy = (float)my - cy2;
          if (dx * dx + dy * dy < btnR * btnR)
            selectImage(current + 1);
        }
      }
    }

    // === 底部悬浮胶囊工具栏 ===
    {
      char zoomStr[24];
      snprintf(zoomStr, sizeof(zoomStr), "%d%%", (int)std::lround(scale * 100));
      const ImGuiStyle &st = ImGui::GetStyle();
      float tbH = FLOAT_TB_H;

      // 分两组：导航（▶◀）| 缩放（- % + F 1:1）| 旋转 ↺ | 放大 ✨
      // 宽度先估，之后算
      // 这里手动设一个合理的最小 + 自适应
      int pad = 16;      // 左右留白
      float sepW = 8.0f; // 分隔线宽度
      struct Btn {
        const char *t;
      };
      Btn group1[] = {{u8"◀"}, {u8"▶"}};
      Btn group2[] = {{"-"}, {zoomStr}, {"+"}, {u8"适应"}, {"1:1"}};
      Btn group3[] = {{u8"旋转"}};
      Btn group4[] = {{u8"高清"}};

      auto grpW = [&](Btn *g, int n) -> float {
        float w = 0;
        for (int i = 0; i < n; i++)
          w += ImGui::CalcTextSize(g[i].t).x + st.FramePadding.x * 2 +
               st.ItemSpacing.x;
        return w;
      };
      float tbW = pad + grpW(group1, 2) + sepW + grpW(group2, 5) + sepW +
                  grpW(group3, 1) + sepW + grpW(group4, 1) + pad;

      float tbX = ((float)winW - tbW) * 0.5f;
      float tbY = (float)winH - (showThumbsBar ? THUMB_PANEL_H : 0) - tbH -
                  FLOAT_TB_GAP;

      // 胶囊底板（画在背景层）
      {
        float pad2 = 5.0f;
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        for (int i = 6; i >= 1; i--) {
          float s = 8.0f * (float)i / 6.0f;
          dl->AddRectFilled(ImVec2(tbX - s, tbY - s + pad2),
                            ImVec2(tbX + tbW + s, tbY + tbH + s - pad2),
                            IM_COL32(0, 0, 0, (unsigned char)(15)), 22.0f + s);
        }
        dl->AddRectFilled(ImVec2(tbX + pad2, tbY + pad2),
                          ImVec2(tbX + tbW - pad2, tbY + tbH - pad2),
                          IM_COL32(255, 255, 255, 240), 22.0f);
        dl->AddRect(ImVec2(tbX + pad2, tbY + pad2),
                    ImVec2(tbX + tbW - pad2, tbY + tbH - pad2),
                    IM_COL32(215, 215, 215, 255), 22.0f, 0, 1.0f);
      }

      ImGui::SetNextWindowPos(ImVec2(tbX, tbY), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(tbW, tbH), ImGuiCond_Always);
      ImGui::Begin("##toolbar", nullptr, noDeco());

      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 7));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 22.0f);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.90f, 0.90f, 0.90f, 0.9f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                            ImVec4(0.82f, 0.82f, 0.82f, 0.9f));

      // Group 1: ◀ ▶
      if (current > 0 && ImGui::ArrowButton("##pv", ImGuiDir_Left))
        selectImage(current - 1);
      ImGui::SameLine();
      if (current < (int)images.size() - 1 &&
          ImGui::ArrowButton("##nx", ImGuiDir_Right))
        selectImage(current + 1);

      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();

      // Group 2: - % + F 1:1
      if (ImGui::Button("-"))
        zoomAt(fbW * 0.5f, mainAreaCenterY(), 1.0f / 1.2f);
      ImGui::SameLine();
      if (ImGui::Button(zoomStr))
        fitToWindow();
      ImGui::SameLine();
      if (ImGui::Button("+"))
        zoomAt(fbW * 0.5f, mainAreaCenterY(), 1.2f);
      ImGui::SameLine();
      if (ImGui::Button(u8"适应"))
        fitToWindow();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"适应窗口 (F)");
      ImGui::SameLine();
      if (ImGui::Button("1:1") && mainTex) {
        scale = 1.0f;
        panX = panY = 0;
      }

      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();

      // Group 3: 旋转（左键快速 +90°，弹出菜单选任意角度）
      if (ImGui::Button(u8"旋转"))
        rotateBy(90);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##rotmenu");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"左键顺时针旋转；右键选择角度");
      if (ImGui::BeginPopup("##rotmenu")) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
        if (ImGui::MenuItem(u8"90°  顺时针旋转")) {
          rotateBy(90);
        }
        if (ImGui::MenuItem(u8"180°  半圈翻转")) {
          rotateBy(180);
        }
        if (ImGui::MenuItem(u8"270°  逆时针旋转")) {
          rotateBy(270);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(u8"重置为 0°")) {
          rotationDeg = 0;
          fitToWindow();
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
      }

      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();

      // Group 4: 高清采样（只在 200% 以上生效，轻度放大保持自然）
      ImGui::PushStyleColor(ImGuiCol_Button,
                            sharpenEnabled
                                ? ImVec4(0.094f, 0.565f, 0.965f, 0.85f)
                                : st.Colors[ImGuiCol_Button]);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            sharpenEnabled
                                ? ImVec4(0.149f, 0.612f, 0.976f, 0.9f)
                                : st.Colors[ImGuiCol_ButtonHovered]);
      if (ImGui::Button(u8"高清"))
        sharpenEnabled = !sharpenEnabled;
      ImGui::PopStyleColor(2);
      if (ImGui::IsItemHovered()) {
        if (scale >= HQ_MIN_SCALE)
          ImGui::SetTooltip(u8"边缘保持锐利放大 — 空格切换");
        else
          ImGui::SetTooltip(u8"已开启；放大到 200% 后自动生效");
      }

      ImGui::PopStyleColor(3);
      ImGui::PopStyleVar(2);
      ImGui::End();
    }

    // === 顶部信息条 + 随手可用的打开入口 ===
    if (showInfoPanel) {
      const float infoW = std::min((float)winW - 32.0f, 820.0f);
      ImGui::SetNextWindowPos(ImVec2(16, 12), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(infoW, 46), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.92f);
      ImGui::Begin("##infobar", nullptr, noDeco());
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
      if (ImGui::SmallButton(u8"打开"))
        openFromDialog(false);
      ImGui::SameLine();
      if (ImGui::SmallButton(u8"文件夹"))
        openFromDialog(true);
      ImGui::SameLine();
      if (editMode)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.094f, 0.565f, 0.965f, 0.95f));
      if (ImGui::SmallButton(editMode ? u8"完成" : u8"绘制")) {
        if (editMode) {
          editMode = false;
          drawingStroke = false;
        } else {
          beginEditMode();
        }
      }
      if (editMode)
        ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"进入绘制模式 (E)");
      ImGui::SameLine();
      ImGui::TextDisabled("|");
      ImGui::SameLine();
      if (current >= 0 && current < (int)images.size()) {
        std::string fname = images[current].path.filename().u8string();
        ImGui::Text("%d / %d   %s%s   %d x %d   %.0f%%", current + 1,
                    (int)images.size(), fname.c_str(), editDirty ? " *" : "",
                    imgW, imgH, scale * 100.0f);
      }
      ImGui::PopStyleColor();
      ImGui::End();
    }

    // 异步切图时保留上一张，额外给出轻量状态提示，避免误以为点击无效。
    if (loadingCurrent || currentDecodeFailed) {
      ImGui::SetNextWindowPos(ImVec2(winW * 0.5f, showInfoPanel ? 70.0f : 18.0f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.94f);
      ImGui::Begin("##loadstatus", nullptr,
                   noDeco() | ImGuiWindowFlags_AlwaysAutoResize);
      if (currentDecodeFailed)
        ImGui::TextColored(ImVec4(0.86f, 0.20f, 0.18f, 1.0f),
                           u8"图片读取失败");
      else
        ImGui::TextDisabled(u8"正在载入图片...");
      ImGui::End();
    }

    if (!editNotice.empty() && glfwGetTime() < editNoticeUntil) {
      ImGui::SetNextWindowPos(ImVec2(winW * 0.5f, showInfoPanel ? 70.0f : 18.0f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.96f);
      ImGui::Begin("##editnotice", nullptr,
                   noDeco() | ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::TextUnformatted(editNotice.c_str());
      ImGui::End();
    }

    if (openUnsavedPopup) {
      ImGui::OpenPopup(u8"未保存的标注");
      openUnsavedPopup = false;
    }
    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(u8"未保存的标注", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      const bool closing = pendingSelectIndex == -2;
      ImGui::TextUnformatted(
          closing ? u8"当前图片有尚未保存的绘制内容。\n关闭程序前要如何处理？"
                  : u8"当前图片有尚未保存的绘制内容。\n切换图片前要如何处理？");
      ImGui::Dummy(ImVec2(0, 10));
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(0.094f, 0.565f, 0.965f, 0.95f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
      if (ImGui::Button(closing ? u8"保存并关闭" : u8"保存并切换",
                        ImVec2(130, 36))) {
        if (saveEditedCopy(false)) {
          const int target = pendingSelectIndex;
          pendingSelectIndex = -1;
          ImGui::CloseCurrentPopup();
          if (target == -2)
            glfwSetWindowShouldClose(window, true);
          else
            selectImageNow(target);
        }
      }
      ImGui::PopStyleColor(2);
      ImGui::SameLine();
      if (ImGui::Button(u8"不保存", ImVec2(110, 36))) {
        const int target = pendingSelectIndex;
        pendingSelectIndex = -1;
        resetEditSession();
        ImGui::CloseCurrentPopup();
        if (target == -2)
          glfwSetWindowShouldClose(window, true);
        else
          selectImageNow(target);
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"取消", ImVec2(90, 36))) {
        pendingSelectIndex = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    // === 底部缩略图条 ===
    if (showThumbsBar) {
      ImGui::SetNextWindowPos(ImVec2(0, (float)winH - THUMB_PANEL_H),
                              ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2((float)winW, THUMB_PANEL_H),
                               ImGuiCond_Always);
      ImGui::Begin("##thumbs", nullptr,
                   noDeco() | ImGuiWindowFlags_HorizontalScrollbar);
      ImGui::GetWindowDrawList()->AddRectFilled(
          ImGui::GetWindowPos(),
          ImVec2(ImGui::GetWindowPos().x + (float)winW,
                 ImGui::GetWindowPos().y + THUMB_PANEL_H),
          IM_COL32(242, 242, 242, 255));

      for (int i = 0; i < (int)images.size(); i++) {
        ImageItem &item = images[i];
        ImGui::PushID(i);
        bool selected = (i == current);

        if (selected) {
          ImGui::PushStyleColor(ImGuiCol_Border,
                                ImVec4(0.094f, 0.565f, 0.965f, 1.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.5f);
          ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        } else {
          ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
          ImGui::PushStyleColor(ImGuiCol_Border,
                                ImVec4(0.78f, 0.78f, 0.78f, 1.0f));
        }

        bool clicked = false;
        if (item.imguiTex != 0) {
          float s2 = std::min(THUMB_IMG_H / item.thumbH, 1.0f);
          clicked =
              ImGui::ImageButton("##thumb", item.imguiTex,
                                 ImVec2(item.thumbW * s2, item.thumbH * s2));
        } else {
          clicked = ImGui::Button(item.thumbFailed ? "x" : "...",
                                  ImVec2(96, THUMB_IMG_H));
        }
        if (clicked)
          selectImage(i);

        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        if (selected && scrollThumbsToCurrent)
          ImGui::SetScrollHereX(0.5f);
        ImGui::SameLine();
        ImGui::PopID();
      }
      scrollThumbsToCurrent = false;
      if (pendingThumbScroll != 0.0f) {
        ImGui::SetScrollX(ImGui::GetScrollX() + pendingThumbScroll * 20.0f);
        pendingThumbScroll = 0.0f;
      }
      if (thumbDragScrollDelta != 0.0f) {
        ImGui::SetScrollX(ImGui::GetScrollX() + thumbDragScrollDelta);
        thumbDragScrollDelta = 0.0f;
      }
      ImGui::End();
    }

    // === 右键菜单 ===
    if (!editMode && mainTex != 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      bool inMain =
          ((float)my >= 34.0f &&
           (!showThumbsBar || (float)my < (float)winH - THUMB_PANEL_H - 48.0f));
      if (inMain)
        ImGui::OpenPopup("##ctxmenu");
    }
    if (ImGui::BeginPopup("##ctxmenu")) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
      if (ImGui::MenuItem(u8"顺时针旋转 90°")) {
        rotateBy(90);
      }
      if (ImGui::MenuItem(u8"逆时针旋转 90°")) {
        rotateBy(270);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(u8"适应窗口 (F)"))
        fitToWindow();
      if (ImGui::MenuItem(u8"原始大小")) {
        scale = 1.0f;
        panX = panY = 0;
      }
      ImGui::Separator();
      if (ImGui::MenuItem(u8"放大增强 (空格)"))
        sharpenEnabled = !sharpenEnabled;
      {
        char buf[64];
        snprintf(buf, sizeof(buf), u8"尺寸: %d × %d px", imgW, imgH);
        ImGui::MenuItem(buf, nullptr, false, false);
      }
      ImGui::PopStyleColor();
      ImGui::EndPopup();
    }
  }

  // 更新窗口标题
  void setTitle(const std::string &msg) {
    glfwSetWindowTitle(window, msg.c_str());
  }

  void updateTitle() {
    if (current < 0 || current >= (int)images.size()) {
      setTitle(u8"Vulkan 图片浏览器 — 拖入图片 / 文件夹");
      return;
    }
    char t[512];
    snprintf(t, sizeof(t),
             u8"Vulkan 图片浏览器 — %s%s  (%d/%d)  %dx%d  %.0f%%",
             images[current].path.filename().u8string().c_str(),
             editDirty ? " *" : "", current + 1, (int)images.size(), imgW,
             imgH, scale * 100.0f);
    setTitle(t);
  }

  // 构造主图渲染四边形
  void buildQuads(std::vector<Quad> &quads) {
    if (mainTex == 0)
      return;
    int winW, winH;
    renderer.extent(winW, winH);
    Quad q;
    q.tex = (editLayerTex != 0 && editIndex == current) ? editLayerTex
                                                        : mainTex;
    q.w = displayedImageWidth() * scale;
    q.h = displayedImageHeight() * scale;
    q.x = winW * 0.5f + panX - q.w * 0.5f;
    q.y = mainAreaCenterY() + panY - q.h * 0.5f;
    q.rotDeg = (float)rotationDeg;
    q.checker = true;
    q.sharpUpscale = sharpenEnabled && scale >= HQ_MIN_SCALE ? scale : 0.0f;

    // 阴影必须先于主图绘制；若用 ImGui 背景层，实际会在 Vulkan 主图之后
    // 覆盖到图像上，造成轻微但真实的颜色偏暗。多层低透明矩形只落在外沿。
    for (int i = 5; i >= 1; --i) {
      const float spread = i * 3.0f;
      Quad shadow;
      shadow.x = q.x - spread;
      shadow.y = q.y - spread + 3.0f;
      shadow.w = q.w + spread * 2.0f;
      shadow.h = q.h + spread * 2.0f;
      shadow.tint[0] = shadow.tint[1] = shadow.tint[2] = 0.0f;
      shadow.tint[3] = 0.018f;
      shadow.blend = true;
      quads.push_back(shadow);
    }
    quads.push_back(q);
  }

  void mainLoop() {
    std::vector<Quad> quads;
    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      consumeResults();

      renderer.newImGuiFrame();
      buildImGuiUI();
      drawEditOverlay();

      // 高清采样直接在主图片元着色器内完成，避免离屏颜色空间中转。
      // 关闭旧后处理后，开启/关闭高清使用完全相同的最终输出链路。
      renderer.setPostProcess({});

      flushEditLayerTexture();

      quads.clear();
      buildQuads(quads);
      renderer.drawFrame(quads);

      updateTitle();
    }
  }

  void shutdown() {
    if (window == nullptr)
      return; // 幂等：未初始化或已清理

    quit = true;
    taskCv.notify_all();
    if (worker.joinable())
      worker.join();

    clearImportedBrushTips();
    renderer.shutdown(); // 内部含 ImGui 后端清理

    glfwDestroyWindow(window);
    window = nullptr;
    glfwTerminate();
  }
};

int main(int argc, char **argv) {
  // 支持命令行传入图片 / 文件夹路径（拖拽与工具栏对话框为主要方式）
  ImageViewerApp app;
  try {
    app.run(argc > 1 ? argv[1] : "");
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
#ifdef _WIN32
    // 双击/IDE 启动时看不到控制台，用消息框展示真实错误
    MessageBoxA(nullptr, e.what(), "Vulkan 图片浏览器 — 启动失败",
                MB_ICONERROR | MB_OK);
#endif
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
