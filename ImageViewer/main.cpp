// Vulkan 图片浏览器
// - 拖拽图片文件/文件夹到窗口打开（也支持命令行参数与工具栏对话框）
// - 支持 psd / jpg / png / bmp / tga / gif / hdr 等 stb_image 可解码的格式
// - UI 使用 Dear ImGui：顶部工具栏 + 底部缩略图栏（可点击选中、横向滚动）
// - 滚轮缩放（以光标为中心）；左键拖拽平移；双击适应窗口
// - 主图带完整 mipmap 链 + 各向异性过滤：缩小不闪、放大平滑

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // 防止 windows.h 的 min/max 宏破坏 std::min/std::max
#endif
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "renderer/VulkanImageRenderer.h"

#include "backends/imgui_impl_vulkan.h" // AddTexture / RemoveTexture（缩略图交给 ImGui 画）
#include "imgui.h"

#include "renderer/PlatformDialogs.h"

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

      uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
      for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
          const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
          r += p[0];
          g += p[1];
          b += p[2];
          a += p[3];
          cnt++;
        }
      }
      uint8_t *d = dst + ((size_t)y * dw + x) * 4;
      d[0] = (uint8_t)(r / cnt);
      d[1] = (uint8_t)(g / cnt);
      d[2] = (uint8_t)(b / cnt);
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
  float pendingThumbScroll = 0.0f; // 滚轮 → delta（每次累积后被消费）
  // 缩略图条鼠标拖拽状态（pendingThumbScroll 同时被滚轮和拖拽共享）
  bool thumbDragging = false;
  double thumbDragStartX = 0;

  // 旋转（UV 变换）
  int rotationDeg = 0;

  // 输入状态 + 自动隐藏
  bool dragging = false;
  double lastMouseX = 0, lastMouseY = 0;
  double lastClickTime = 0.0;
  double cursorX = WIDTH * 0.5, cursorY = HEIGHT * 0.5;

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

    worker = std::thread([this] { decodeLoop(); });
  }

  void initImGui() {
    renderer.initImGui();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // 中文字体：微软雅黑
    ImFont *font = io.Fonts->AddFontFromFileTTF(
        "C:/Windows/Fonts/msyh.ttc", 18.0f, nullptr,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
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

  void openPath(const fs::path &p) {
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

  float mainAreaTop() const { return 0.0f; }
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

  float fitScale() const {
    if (imgW <= 0 || imgH <= 0)
      return 1.0f;
    int winW, winH;
    renderer.extent(winW, winH);
    float mainH = mainAreaBottom() - mainAreaTop();
    return std::min((float)winW / imgW, mainH / imgH);
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

  void selectImage(int index) {
    if (images.empty())
      return;
    index = std::clamp(index, 0, (int)images.size() - 1);
    if (index == current)
      return;

    current = index;
    currentDecodeFailed = false;
    prefetchAround(current);
    scrollThumbsToCurrent = true;

    if (fullCache.count(current))
      uploadCurrentFromCache();
    else if (uploadedIndex != current)
      loadingCurrent = true; // 等待解码，期间保留旧图
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
      if (fullscreen) {
        glfwSetWindowMonitor(window, nullptr, 100, 100, WIDTH, HEIGHT, 0);
        fullscreen = false;
      } else {
        glfwSetWindowShouldClose(window, true);
      }
      return;
    }

    // Ctrl+O / Ctrl+Shift+O
    if (action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL) &&
        key == GLFW_KEY_O) {
      std::wstring f = (mods & GLFW_MOD_SHIFT) ? openFolderDialog(nativeWindow)
                                               : openFileDialog(nativeWindow);
      if (!f.empty())
        openPath(fs::path(f));
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
        int mx, my, sw, sh;
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
    zoomAt((float)mx, (float)my, std::pow(1.15f, (float)yoff));
  }

  void onMouseButton(int button, int action) {
    if (button != GLFW_MOUSE_BUTTON_LEFT)
      return;

    double mx, my;
    mouseFb(mx, my);

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
      if (now - lastClickTime < 0.35)
        fitToWindow();
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

    // thumbs 拖拽：相对增量 → 累加到 pendingThumbScroll
    // （鼠标向右拖 → 内容向右 → ScrollX 增大）
    if (thumbDragging) {
      // 鼠标左拖 → 内容跟着左走 → ScrollX 减小（方向相反）
      pendingThumbScroll += (float)(thumbDragStartX - mx);
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

  // -----------------------------------------------------------------
  // ImGui 界面（工具栏 + 缩略图面板）
  // -----------------------------------------------------------------

  void buildImGuiUI() {
    int winW, winH;
    renderer.extent(winW, winH);
    float cy = mainAreaCenterY();

    double mx, my;
    mouseFb(mx, my);
    static double lastMx = -1, lastMy = -1;
    if (std::abs(mx - lastMx) > 0.5 || std::abs(my - lastMy) > 0.5) {
      lastMx = mx;
      lastMy = my;
      lastInputTime = glfwGetTime();
    }
    if (!uiVisible)
      return;

    auto noDeco = [] {
      return ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
             ImGuiWindowFlags_NoSavedSettings |
             ImGuiWindowFlags_NoBringToFrontOnFocus;
    };

    // === 主图阴影（背景层，浅色主题用深色阴影）===
    if (mainTex != 0) {
      float qw = imgW * scale, qh = imgH * scale;
      float qx = winW * 0.5f + panX - qw * 0.5f;
      float qy = cy + panY - qh * 0.5f;
      float cr = std::min(18.0f, qw * 0.03f);
      ImDrawList *dl = ImGui::GetBackgroundDrawList();
      for (int i = 8; i >= 1; i--) {
        float s = 24.0f * (float)i / 8.0f;
        float a = 0.015f;
        dl->AddRectFilled(
            ImVec2(qx - s, qy - s), ImVec2(qx + qw + s, qy + qh + s),
            IM_COL32(0, 0, 0, (unsigned char)(a * 255.0f)), cr + s);
      }
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
                            btnR);
        dl->AddCircleFilled(
            ImVec2(cx, cy2), btnR - 1,
            IM_COL32(255, 255, 255, (unsigned char)(fade * 210)), btnR - 1);
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
                            btnR);
        dl->AddCircleFilled(
            ImVec2(cx, cy2), btnR - 1,
            IM_COL32(255, 255, 255, (unsigned char)(fade * 210)), btnR - 1);
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
      Btn group2[] = {{"-"}, {zoomStr}, {"+"}, {"F"}, {"1:1"}};
      Btn group3[] = {{"R"}};  // 旋转按钮（ASCII 字符，字体兼容）
      Btn group4[] = {{"✨"}}; // 放大增强（Emoji，msyh 支持）

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
        zoomAt(winW * 0.5f, cy, 1.0f / 1.2f);
      ImGui::SameLine();
      if (ImGui::Button(zoomStr))
        fitToWindow();
      ImGui::SameLine();
      if (ImGui::Button("+"))
        zoomAt(winW * 0.5f, cy, 1.2f);
      ImGui::SameLine();
      if (ImGui::Button("F"))
        fitToWindow();
      ImGui::SameLine();
      if (ImGui::Button("1:1") && mainTex) {
        scale = 1.0f;
        panX = panY = 0;
      }

      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();

      // Group 3: 旋转（左键快速 +90°，弹出菜单选任意角度）
      if (ImGui::Button("R")) {
        rotationDeg = (rotationDeg + 90) % 360;
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
          ImGui::IsItemHovered())
        ImGui::OpenPopup("##rotmenu");
      if (ImGui::BeginPopup("##rotmenu")) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
        char label[64];
        snprintf(label, sizeof(label), "90%c  (%d%c)", (char)0xC2, (char)0xB0,
                 90, (char)0xC2, (char)0xB0);
        if (ImGui::MenuItem(u8"90°  顺时针旋转")) {
          rotationDeg = (rotationDeg + 90) % 360;
        }
        if (ImGui::MenuItem(u8"180°  半圈翻转")) {
          rotationDeg = (rotationDeg + 180) % 360;
        }
        if (ImGui::MenuItem(u8"270°  逆时针旋转")) {
          rotationDeg = (rotationDeg + 270) % 360;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(u8"重置为 0°")) {
          rotationDeg = 0;
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
      }

      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();

      // Group 4: ✨ 放大增强
      ImGui::PushStyleColor(ImGuiCol_Button,
                            sharpenEnabled
                                ? ImVec4(0.094f, 0.565f, 0.965f, 0.85f)
                                : st.Colors[ImGuiCol_Button]);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            sharpenEnabled
                                ? ImVec4(0.149f, 0.612f, 0.976f, 0.9f)
                                : st.Colors[ImGuiCol_ButtonHovered]);
      if (ImGui::Button(u8"✨"))
        sharpenEnabled = !sharpenEnabled;
      ImGui::PopStyleColor(2);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"放大增强（双三次插值）— 空格切换");

      ImGui::PopStyleColor(3);
      ImGui::PopStyleVar(2);
      ImGui::End();
    }

    // === 顶部文件名 + 序号 ===
    {
      ImGui::SetNextWindowPos(ImVec2(16, 12), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(520, 28), ImGuiCond_Always);
      ImGui::Begin("##infobar", nullptr,
                   noDeco() | ImGuiWindowFlags_NoBackground);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
      if (current >= 0 && current < (int)images.size()) {
        std::string fname = images[current].path.filename().u8string();
        ImGui::Text("(%d/%d) %s  %dx%d  %.0f%%", current + 1,
                    (int)images.size(), fname.c_str(), imgW, imgH,
                    scale * 100.0f);
      } else {
        ImGui::Text(u8"拖入图片 / 文件夹，或按 Ctrl+O");
      }
      ImGui::PopStyleColor();
      ImGui::End();
    }

    if (images.empty())
      return;

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
      ImGui::End();
    }

    // === 右键菜单 ===
    if (mainTex != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      int w2, h2;
      renderer.extent(w2, h2);
      bool inMain =
          ((float)my >= 34.0f &&
           (!showThumbsBar || (float)my < (float)h2 - THUMB_PANEL_H - 48.0f));
      if (inMain)
        ImGui::OpenPopup("##ctxmenu");
    }
    if (ImGui::BeginPopup("##ctxmenu")) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
      if (ImGui::MenuItem(u8"顺时针旋转 90°")) {
        rotationDeg = (rotationDeg + 90) % 360;
      }
      if (ImGui::MenuItem(u8"逆时针旋转 90°")) {
        rotationDeg = (rotationDeg + 270) % 360;
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
    snprintf(t, sizeof(t), u8"Vulkan 图片浏览器 — %s  (%d/%d)  %dx%d  %.0f%%",
             images[current].path.filename().u8string().c_str(), current + 1,
             (int)images.size(), imgW, imgH, scale * 100.0f);
    setTitle(t);
  }

  // 构造主图渲染四边形
  void buildQuads(std::vector<Quad> &quads) {
    if (mainTex == 0)
      return;
    int winW, winH;
    renderer.extent(winW, winH);
    Quad q;
    q.tex = mainTex;
    q.w = imgW * scale;
    q.h = imgH * scale;
    q.x = winW * 0.5f + panX - q.w * 0.5f;
    q.y = mainAreaCenterY() + panY - q.h * 0.5f;
    q.rotDeg = (float)rotationDeg;
    quads.push_back(q);
  }

  void mainLoop() {
    std::vector<Quad> quads;
    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      consumeResults();

      renderer.newImGuiFrame();
      buildImGuiUI();

      // 放大超过 1:1 时启用计算着色器双三次放大（缩小时 mipmap 已经足够清晰）
      VulkanImageRenderer::PostParams pp;
      pp.enabled = sharpenEnabled && mainTex != 0 && scale > 1.05f;
      pp.source = mainTex;
      pp.srcW = (float)imgW;
      pp.srcH = (float)imgH;
      pp.w = imgW * scale;
      pp.h = imgH * scale;
      {
        int winW, winH;
        renderer.extent(winW, winH);
        pp.x = winW * 0.5f + panX - pp.w * 0.5f;
        pp.y = mainAreaCenterY() + panY - pp.h * 0.5f;
      }
      renderer.setPostProcess(pp);

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
