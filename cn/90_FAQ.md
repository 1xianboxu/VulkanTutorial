# 常见问题（FAQ）

## 验证层相关

### 核心验证层报访问冲突错误

确保 **MSI Afterburner / RivaTuner Statistics Server** 没有运行——它与 Vulkan 存在兼容性问题。

### 看不到验证层消息 / 验证层不可用

1. 确保程序退出后终端保持打开（Visual Studio 用 `Ctrl+F5` 运行）
2. 确认 Vulkan SDK 正确安装（运行 SDK 的 Verify Installation）
3. SDK 版本至少 **1.1.106.0** 才支持 `VK_LAYER_KHRONOS_validation`

## 交换链与实例创建

### `vkCreateSwapchainKHR` 在 SteamOverlayVulkanLayer64.dll 中报错

Steam 客户端 Beta 兼容性问题。解决方案：

- 退出 Steam Beta 计划
- 设置环境变量 `DISABLE_VK_LAYER_VALVE_steam_overlay_1=1`
- 删除注册表中 Steam overlay Vulkan 层：  
  `HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\Vulkan\ImplicitLayers`

### macOS 上 `vkCreateInstance` 返回 `VK_ERROR_INCOMPATIBLE_DRIVER`

Vulkan SDK 1.3.216+ 要求启用 MoltenVK 可移植性扩展：

```c++
requiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
```

## 常见编程错误

### 忘记设置 `sType`

Vulkan 的每个 `CreateInfo` 结构体都必须设置 `sType`，否则会导致未定义行为或崩溃。

> ⚠️ 使用 `{}` 初始化可以将 `sType` 之外的字段清零，但 `sType` 仍需手动设置。

### 验证层报 "access to resource not bound"

- 确认描述符集已正确绑定
- 确认缓冲区/图像在使用前已创建和分配内存

### 屏幕全黑（无渲染输出）

检查清单：
1. 着色器是否编译成功（`glslc` 无错误）
2. `vkCmdDraw` / `vkCmdDrawIndexed` 参数是否正确
3. 帧缓冲附件是否包含正确的图像视图
4. 管线布局是否与着色器匹配
5. 渲染通道是否正确配置
6. 视口和裁剪矩形是否设置

### 图像上下颠倒

- Vulkan Y 轴与 OpenGL 相反
- 投影矩阵：`proj[1][1] *= -1`
- 纹理坐标：OBJ 模型需要 `1.0f - texcoord.v`

### 同步问题导致闪烁/撕裂

- 确保信号量和栅栏正确配对
- `vkWaitForFences` 后记得 `vkResetFences`
- 交换链图像获取必须在栅栏等待之后

## 性能相关

### 帧率低

- 确认使用 Release 构建（Debug 模式明显更慢）
- 验证层会影响性能，发布时禁用
- 使用 `VK_PRESENT_MODE_MAILBOX_KHR` 获取最低延迟（但更耗电）
- `VK_PRESENT_MODE_FIFO_KHR` 有 V-Sync，帧率锁定刷新率

### 内存使用过高

- 使用暂存缓冲区上传后及时清理
- 纹理使用合适的分辨率和格式
- 考虑使用 VMA（Vulkan Memory Allocator）库管理内存

## 调试技巧

| 工具 | 用途 |
|------|------|
| Vulkan 验证层 | 检测 API 使用错误 |
| RenderDoc | GPU 帧分析、调试着色器 |
| Vulkan GPU Profiler | 性能分析 |
| `VK_EXT_debug_utils` | 为 Vulkan 对象命名，方便调试 |

> 💡 开发时**始终启用验证层**。很多隐蔽的错误只有验证层才能发现。
