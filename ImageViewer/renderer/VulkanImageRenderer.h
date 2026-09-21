#pragma once

// 通用"带纹理四边形" Vulkan 渲染器
// 改编自 VulkanTutorial 第 29 章（29_mipmapping.cpp），去掉深度缓冲 /
// 模型加载， 变换从 UBO 改为 push constants，支持多纹理（每纹理一个 descriptor
// set）。 渲染器本身不依赖 stb_image：只接受解码好的 RGBA
// 像素，方便应用层多线程解码。

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>
#include <map>
#include <vector>

using TextureHandle = uint32_t;

// 内建 1x1 白色纹理句柄，配合 tint 可画纯色块（UI 背景、选中框等）
constexpr TextureHandle WhiteTexture = 0;

struct Quad {
  TextureHandle tex = WhiteTexture;
  float x = 0, y = 0, w = 0, h = 0;     // 像素坐标，窗口左上角为原点，y 向下
  float tint[4] = {1.f, 1.f, 1.f, 1.f}; // RGBA 乘色
  float rotDeg = 0.0f;                  // UV 旋转（0/90/180/270）
  bool checker = false; // 是否按 alpha 混合棋盘格背景（看透明图用）
  bool blend = false;   // true=按 alpha 与背景混合（文字等半透明贴片用）
};

class VulkanImageRenderer {
public:
  void init(GLFWwindow *window);
  void shutdown();

  // 等待 GPU 空闲（批量释放纹理/descriptor 之前调用，避免释放仍在使用的资源）
  void waitIdle() {
    if (device != VK_NULL_HANDLE)
      vkDeviceWaitIdle(device);
  }

  // 上传 RGBA8 像素创建纹理。mipmaps=true 时生成完整 mipmap
  // 链并启用各向异性过滤
  // （缩小不闪、放大平滑的"自动变清晰"核心）。返回句柄，失败抛
  // std::runtime_error。
  TextureHandle createTexture(const void *rgba, int width, int height,
                              bool mipmaps);

  // 延迟销毁：等 GPU 不再使用后回收（在 drawFrame 中自动清理）
  void destroyTexture(TextureHandle tex);

  // 绘制一帧；内部处理交换链重建与窗口最小化
  void drawFrame(const std::vector<Quad> &quads);

  // ---- ImGui 叠加层（可选）----
  // init 之后调用一次即可用 ImGui 画 UI；每帧顺序为：
  //   newImGuiFrame() → ImGui:: 构建 UI → drawFrame(quads)（内部自动 Render +
  //   录制）
  void initImGui();
  void newImGuiFrame();
  bool imguiEnabled() const { return imguiReady; }

  // 供 ImGui_ImplVulkan_AddTexture 使用（如把缩略图纹理直接交给 ImGui 绘制）
  bool getTextureInfo(TextureHandle tex, VkImageView &view, VkSampler &sampler);

  // 计算着色器后处理（双三次高质量放大）：
  // 开启后四边形先渲染到离屏纹理，计算着色器对源纹理做 Catmull-Rom 双三次
  // 重采样再上屏。放大超过 1:1 时启用可消除双线性采样的模糊。
  struct PostParams {
    bool enabled = false;
    TextureHandle source = 0;         // 需要高质量放大的源纹理（主图）
    float srcW = 0, srcH = 0;         // 源纹理尺寸
    float x = 0, y = 0, w = 0, h = 0; // 源纹理在屏幕上的显示矩形（帧缓冲像素）
  };
  void setPostProcess(const PostParams &p) { post = p; }

  void extent(int &w, int &h) const {
    w = (int)swapChainExtent.width;
    h = (int)swapChainExtent.height;
  }
  int maxTextureSize() const { return (int)maxImageDimension; }

private:
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint32_t mipLevels = 1;
    int width = 0, height = 0;
    bool mipmapped = false;
  };

  GLFWwindow *window = nullptr;

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  VkQueue presentQueue = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily = 0;
  uint32_t maxImageDimension = 2048;
  bool imguiReady = false;

  VkSwapchainKHR swapChain = VK_NULL_HANDLE;
  std::vector<VkImage> swapChainImages;
  VkFormat swapChainImageFormat;
  VkExtent2D swapChainExtent{};
  std::vector<VkImageView> swapChainImageViews;
  std::vector<VkFramebuffer> swapChainFramebuffers;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline graphicsPipeline = VK_NULL_HANDLE;

  VkCommandPool commandPool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> commandBuffers;

  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;

  VkSampler samplerMip = VK_NULL_HANDLE;   // 线性 + mipmap + 各向异性（主图）
  VkSampler samplerPlain = VK_NULL_HANDLE; // 线性、无 mipmap（缩略图 / 纯色块）
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

  std::map<TextureHandle, Texture> textures;
  TextureHandle nextHandle = 1; // 0 保留给内建白色纹理
  std::vector<std::pair<Texture, uint64_t>> pendingDestroy; // (纹理, 退役帧号)
  uint64_t frameCounter = 0;

  std::vector<VkSemaphore> imageAvailableSemaphores;
  std::vector<VkSemaphore> renderFinishedSemaphores;
  std::vector<VkFence> inFlightFences;
  uint32_t currentFrame = 0;

  // ---- 计算着色器后处理（双三次放大）----
  PostParams post;
  TextureHandle boundSource = 0; // 计算 descriptor set 当前绑定的源纹理

  VkRenderPass offscreenRenderPass =
      VK_NULL_HANDLE; // 与 swapchain renderPass
                      // 兼容，finalLayout=SHADER_READ_ONLY
  VkImage postInputImage = VK_NULL_HANDLE; // 四边形先画到这里（可采样渲染目标）
  VkDeviceMemory postInputMemory = VK_NULL_HANDLE;
  VkImageView postInputView = VK_NULL_HANDLE;
  VkFramebuffer postFramebuffer = VK_NULL_HANDLE;
  VkImage postOutputImage = VK_NULL_HANDLE; // CAS 输出（storage + sampled）
  VkDeviceMemory postOutputMemory = VK_NULL_HANDLE;
  VkImageView postOutputView = VK_NULL_HANDLE;
  VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
  VkPipeline computePipeline = VK_NULL_HANDLE;
  VkDescriptorSet computeDescriptorSet =
      VK_NULL_HANDLE; // in: postInput，out: postOutput
  VkDescriptorSet postDisplaySet =
      VK_NULL_HANDLE; // postOutput 作为普通纹理全屏上屏

  // ---- 初始化步骤 ----
  void createInstance();
  void setupDebugMessenger();
  void createSurface();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createSwapChain();
  void createImageViews();
  void createRenderPass();
  void createDescriptorSetLayout();
  void createGraphicsPipeline();
  void createFramebuffers();
  void createCommandPool();
  void createVertexBuffer();
  void createSamplers();
  void createDescriptorPool();
  void createWhiteTexture();
  void createCommandBuffers();
  void createSyncObjects();

  void recreateSwapChain();
  void cleanupSwapChain();
  void retireTextures();
  void shutdownImGui();

  // ---- 后处理内部实现 ----
  void createComputePipeline();
  void createPostResources();
  void cleanupPostResources();
  void updateComputeDescriptors();
  void updateComputeSource(VkImageView sourceView);

  // ---- 纹理内部实现 ----
  Texture createTextureInternal(const void *rgba, int w, int h, bool mipmaps);
  void destroyTextureInternal(Texture &tex);
  void generateMipmaps(VkImage image, VkFormat format, int32_t w, int32_t h,
                       uint32_t mipLevels);

  // ---- 通用工具（与教程一致） ----
  void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer &buffer,
                    VkDeviceMemory &bufferMemory);
  void createImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                   VkFormat format, VkImageTiling tiling,
                   VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                   VkImage &image, VkDeviceMemory &imageMemory);
  VkImageView createImageView(VkImage image, VkFormat format,
                              VkImageAspectFlags aspectFlags,
                              uint32_t mipLevels);
  void transitionImageLayout(VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout, uint32_t mipLevels);
  void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                         uint32_t height);
  VkCommandBuffer beginSingleTimeCommands();
  void endSingleTimeCommands(VkCommandBuffer commandBuffer);
  uint32_t findMemoryType(uint32_t typeFilter,
                          VkMemoryPropertyFlags properties);
  VkShaderModule createShaderModule(const std::vector<char> &code);

  // ---- 设备 / 交换链能力查询 ----
  bool isDeviceSuitable(VkPhysicalDevice device);
  bool checkDeviceExtensionSupport(VkPhysicalDevice device);
  VkSurfaceFormatKHR chooseSwapSurfaceFormat(
      const std::vector<VkSurfaceFormatKHR> &availableFormats);
  VkPresentModeKHR chooseSwapPresentMode(
      const std::vector<VkPresentModeKHR> &availablePresentModes);
  VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);

  bool checkValidationLayerSupport();
  std::vector<const char *> getRequiredExtensions();
};
