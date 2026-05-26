# 生成 Mipmaps

## 📍 本章在教程中的位置

```mermaid
graph LR
    A[28 加载模型]:::done --> B[29 生成Mipmaps]:::current
    B --> C[30 多重采样]:::next
    
    classDef done fill:#c8e6c9,stroke:#2e7d32
    classDef current fill:#ff9800,stroke:#e65100,color:#fff
    classDef next fill:#bbdefb,stroke:#1565c0
```

> 📍 **依赖与影响**：
> - **回溯修改**：
>   - [纹理图像](06_纹理映射/00_纹理图像.md)：`createImage()` 添加 `mipLevels` 参数 + `TRANSFER_SRC_BIT`
>   - [图像视图与采样器](06_纹理映射/01_图像视图与采样器.md)：`createImageView()` 添加 `levelCount`、采样器设置 `maxLod`
>   - `transitionImageLayout()` 删除（布局转换改由 `generateMipmaps()` 处理）
> - **复用**：`beginSingleTimeCommands()` ← [暂存缓冲区](04_顶点缓冲区/02_暂存缓冲区.md)
> - **产出**：`generateMipmaps()` 函数、多级纹理

## 什么是 Mipmap

**Mipmap** 是纹理的预计算缩小版本序列，每级分辨率减半。远处物体使用低级 mipmap，避免摩尔纹和闪烁。

```mermaid
graph LR
    L0["Level 0<br>256×256"] --> L1["Level 1<br>128×128"]
    L1 --> L2["Level 2<br>64×64"]
    L2 --> L3["Level 3<br>32×32"]
    L3 --> L4["..."]
    L4 --> LN["Level N<br>1×1"]
    
    style L0 fill:#e3f2fd
    style L1 fill:#bbdefb
    style L2 fill:#90caf9
    style L3 fill:#64b5f6
```

Mipmap 层级数：$\lfloor\log_2(\max(width, height))\rfloor + 1$

```c++
uint32_t mipLevels;
mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;
```

## 创建支持 Mipmap 的图像

```c++
createImage(texWidth, texHeight, mipLevels, VK_FORMAT_R8G8B8A8_SRGB,
    VK_IMAGE_TILING_OPTIMAL,
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, textureImage, textureImageMemory);
```

> ⚠️ `TRANSFER_SRC_BIT` 也是必需的，因为生成 mipmap 需要从上一级读取（blit 源）。

## 生成 Mipmap（使用 vkCmdBlitImage）

核心是逐级使用 `vkCmdBlitImage` 将上一级缩小一半：

```c++
void generateMipmaps(VkImage image, VkFormat imageFormat,
                     int32_t texWidth, int32_t texHeight, uint32_t mipLevels) {
    // ⚠️ 检查格式是否支持线性 blit
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, imageFormat, &formatProperties);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        throw std::runtime_error("texture image format does not support linear blitting!");
    }

    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t i = 1; i < mipLevels; i++) {
        // 将 level i-1 转为 TRANSFER_SRC
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Blit：从 level i-1 缩小到 level i
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mipWidth > 1 ? mipWidth / 2 : 1,
                              mipHeight > 1 ? mipHeight / 2 : 1, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(commandBuffer,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // 将 level i-1 转为 SHADER_READ_ONLY
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    // 最后一级也转为 SHADER_READ_ONLY
    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    endSingleTimeCommands(commandBuffer);
}
```

```mermaid
graph TD
    subgraph "每级 Mipmap 生成流程"
        A["Level i-1: TRANSFER_DST → TRANSFER_SRC"] --> B["vkCmdBlitImage<br>从 i-1 缩小到 i"]
        B --> C["Level i-1: TRANSFER_SRC → SHADER_READ_ONLY"]
        C --> D["处理下一级"]
    end
```

## 采样器配置

```c++
samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  // mipmap 间线性插值
samplerInfo.minLod = 0.0f;
samplerInfo.maxLod = static_cast<float>(mipLevels);
samplerInfo.mipLodBias = 0.0f;
```

## 知识点总结

| 概念 | 说明 |
|------|------|
| Mipmap | 预计算的纹理缩小级链 |
| `vkCmdBlitImage` | 图像缩放/复制，用于逐级生成 mipmap |
| 线性 blit 支持 | 必须检查格式是否支持 `FILTER_LINEAR` |
| 布局转换 | 每级需要 DST→SRC→SHADER_READ_ONLY 三步 |
| LOD | Level of Detail，采样器根据距离自动选择 mipmap 级 |

### 🔴 Mipmap 常见错误

#### 错误 1：`mipLevels` 只传给了图像，忘了图像视图

```c++
// ❌ 图像创建用了 mipLevels，但图像视图的 levelCount 仍然是 1
imageViewInfo.subresourceRange.levelCount = 1;  // 只能看到 Level 0！
// 采样器即使配了 maxLod 也没用，因为视图不包含其他 mip 级

// ✅ 图像视图必须包含所有 mip 级
imageViewInfo.subresourceRange.levelCount = mipLevels;
```

#### 错误 2：图像缺少 `TRANSFER_SRC_BIT`

```c++
// ❌ 只加了 TRANSFER_DST 和 SAMPLED
createImage(..., VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, ...);
// → vkCmdBlitImage 需要源图像有 TRANSFER_SRC → 验证层报错

// ✅ 需要三个标志
createImage(...,
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |   // blit 源（上一级读取）
    VK_IMAGE_USAGE_TRANSFER_DST_BIT |   // blit 目标 + 初始上传
    VK_IMAGE_USAGE_SAMPLED_BIT,         // 着色器采样
    ...);
```

#### 错误 3：格式不支持线性 blit → 沉默失败

```c++
// ⚠️ 并非所有格式都支持 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
// 例如某些移动 GPU 不支持 SRGB 格式的线性 blit

// ✅ 运行时检查（教程已做）
VkFormatProperties props;
vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
    // 方案 A：CPU 端生成 mipmap 然后逐级上传
    // 方案 B：使用 KTX/DDS 预生成 mipmap 的纹理文件
    throw std::runtime_error("不支持线性 blit！");
}
```

#### 错误 4：最后一级 mipmap 布局没有转换

```c++
// ❌ 循环中每次转换 level i-1，但最后一级没人转换
// 最后一级仍然停在 TRANSFER_DST_OPTIMAL → 着色器采样报错

// ✅ 循环结束后单独处理最后一级（教程已做）
barrier.subresourceRange.baseMipLevel = mipLevels - 1;
barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
// 不要忘记这个！
```

#### 错误 5：采样器 `maxLod` 设为 0 → mipmap 无效

```c++
// ❌ maxLod = 0 → 只使用 Level 0，无论距离多远
samplerInfo.maxLod = 0.0f;

// ✅ maxLod 设为 mipLevels（或 VK_LOD_CLAMP_NONE = 1000.0f）
samplerInfo.maxLod = static_cast<float>(mipLevels);
// 或
samplerInfo.maxLod = VK_LOD_CLAMP_NONE;  // 自动使用所有可用级
```

> 💡 **调试技巧**：设置 `mipLodBias = 2.0f` 可以让纹理显得更模糊（强制使用更高的 mip 级），从而确认 mipmap 确实在工作。

接下来学习[多重采样](10_多重采样.md)抗锯齿。

[C++ 代码](/code/29_mipmapping.cpp)
