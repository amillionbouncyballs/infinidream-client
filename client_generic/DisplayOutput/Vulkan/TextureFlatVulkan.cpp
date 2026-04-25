#ifndef WIN32

#include "TextureFlatVulkan.h"
#include "RendererVulkan.h"
#include "Image.h"
#include "Log.h"

extern "C" {
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
}

namespace DisplayOutput
{

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
CTextureFlatVulkan::CTextureFlatVulkan(CRendererVulkan* renderer,
                                        const uint32_t flags)
    : CTextureFlat(flags), m_pRenderer(renderer)
{}

void CTextureFlatVulkan::DestroyVulkanResources()
{
    if (!m_pRenderer) return;
    VkDevice dev = m_pRenderer->GetDevice();
    if (dev == VK_NULL_HANDLE) return;

    // Wait for any in-flight upload to finish before freeing the staging buffer.
    if (m_copyPending && m_copyFence != VK_NULL_HANDLE)
    {
        vkWaitForFences(dev, 1, &m_copyFence, VK_TRUE, UINT64_MAX);
        m_copyPending = false;
    }
    if (m_uploadCmdBuffer != VK_NULL_HANDLE)
    {
        vkFreeCommandBuffers(dev, m_pRenderer->GetCommandPool(),
                             1, &m_uploadCmdBuffer);
        m_uploadCmdBuffer = VK_NULL_HANDLE;
    }
    if (m_copyFence != VK_NULL_HANDLE)
    {
        vkDestroyFence(dev, m_copyFence, nullptr);
        m_copyFence = VK_NULL_HANDLE;
    }

    if (m_stagingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(dev, m_stagingBuffer, nullptr);
        vkFreeMemory(dev, m_stagingMem, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingMem    = VK_NULL_HANDLE;
    }
    if (m_imageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(dev, m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE)
    {
        vkDestroyImage(dev, m_image, nullptr);
        vkFreeMemory(dev, m_imageMem, nullptr);
        m_image    = VK_NULL_HANDLE;
        m_imageMem = VK_NULL_HANDLE;
    }

    // YUV plane resources
    if (m_yView     != VK_NULL_HANDLE) { vkDestroyImageView(dev, m_yView,   nullptr); m_yView   = VK_NULL_HANDLE; }
    if (m_yPlane    != VK_NULL_HANDLE) { vkDestroyImage(dev, m_yPlane,   nullptr);    m_yPlane  = VK_NULL_HANDLE; }
    if (m_yPlaneMem != VK_NULL_HANDLE) { vkFreeMemory(dev, m_yPlaneMem, nullptr);     m_yPlaneMem = VK_NULL_HANDLE; }
    if (m_uvView    != VK_NULL_HANDLE) { vkDestroyImageView(dev, m_uvView,  nullptr); m_uvView   = VK_NULL_HANDLE; }
    if (m_uvPlane   != VK_NULL_HANDLE) { vkDestroyImage(dev, m_uvPlane,  nullptr);    m_uvPlane  = VK_NULL_HANDLE; }
    if (m_uvPlaneMem != VK_NULL_HANDLE) { vkFreeMemory(dev, m_uvPlaneMem, nullptr);   m_uvPlaneMem = VK_NULL_HANDLE; }
    // m_yuvDescSet freed with pool
    m_yuvDescSet = VK_NULL_HANDLE;
}

CTextureFlatVulkan::~CTextureFlatVulkan()
{
    DestroyVulkanResources();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool CTextureFlatVulkan::allocStaging(VkDeviceSize size)
{
    VkDevice dev = m_pRenderer->GetDevice();

    if (m_stagingBuffer != VK_NULL_HANDLE && size <= m_stagingSize)
        return true; // re-use existing

    if (m_stagingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(dev, m_stagingBuffer, nullptr);
        vkFreeMemory(dev, m_stagingMem, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingMem    = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = size;
    bufInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(dev, &bufInfo, nullptr, &m_stagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(dev, m_stagingBuffer, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = m_pRenderer->FindMemType(
        memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &m_stagingMem) != VK_SUCCESS)
        return false;

    vkBindBufferMemory(dev, m_stagingBuffer, m_stagingMem, 0);
    m_stagingSize = size;
    return true;
}

// ---------------------------------------------------------------------------
// Upload from CImage
// ---------------------------------------------------------------------------
bool CTextureFlatVulkan::Upload(spCImage _spImage)
{
    if (!_spImage) return false;
    m_spImage = _spImage;

    uint32_t w = _spImage->GetWidth(0);
    uint32_t h = _spImage->GetHeight(0);
    const uint8_t* pixels = _spImage->GetData(0);
    if (!pixels) return false;

    VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    if (!allocStaging(size))
        return false;

    // Copy pixels → staging
    VkDevice dev = m_pRenderer->GetDevice();
    void* mapped;
    vkMapMemory(dev, m_stagingMem, 0, size, 0, &mapped);
    memcpy(mapped, pixels, static_cast<size_t>(size));
    vkUnmapMemory(dev, m_stagingMem);

    // (Re-)create VkImage if size changed
    if (w != m_imgWidth || h != m_imgHeight)
    {
        if (m_imageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(dev, m_imageView, nullptr);
            m_imageView = VK_NULL_HANDLE;
        }
        if (m_image != VK_NULL_HANDLE)
        {
            vkDestroyImage(dev, m_image, nullptr);
            vkFreeMemory(dev, m_imageMem, nullptr);
            m_image    = VK_NULL_HANDLE;
            m_imageMem = VK_NULL_HANDLE;
        }

        // Create image
        VkImageCreateInfo imgInfo{};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_R8G8B8A8_SRGB;
        imgInfo.extent        = {w, h, 1};
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 1;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(dev, &imgInfo, nullptr, &m_image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(dev, m_image, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = m_pRenderer->FindMemType(
            memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(dev, &allocInfo, nullptr, &m_imageMem) != VK_SUCCESS)
            return false;
        vkBindImageMemory(dev, m_image, m_imageMem, 0);

        // Image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image            = m_image;
        viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format           = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(dev, &viewInfo, nullptr, &m_imageView) != VK_SUCCESS)
            return false;

        m_imgWidth  = w;
        m_imgHeight = h;

        // Allocate / update descriptor set
        if (m_descSet == VK_NULL_HANDLE)
        {
            VkDescriptorSetAllocateInfo dsAlloc{};
            dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsAlloc.descriptorPool     = m_pRenderer->GetDescriptorPool();
            dsAlloc.descriptorSetCount = 1;
            VkDescriptorSetLayout layout = m_pRenderer->GetDescriptorSetLayout();
            dsAlloc.pSetLayouts        = &layout;
            vkAllocateDescriptorSets(dev, &dsAlloc, &m_descSet);
        }

        VkDescriptorImageInfo imgDescInfo{};
        imgDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgDescInfo.imageView   = m_imageView;
        imgDescInfo.sampler     = m_pRenderer->GetDefaultSampler();

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_descSet;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgDescInfo;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }

    return uploadToImage(w, h);
}

bool CTextureFlatVulkan::uploadToImage(uint32_t w, uint32_t h)
{
    VkDevice dev = m_pRenderer->GetDevice();

    // Lazily create the persistent upload command buffer and fence.
    if (m_uploadCmdBuffer == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = m_pRenderer->GetCommandPool();
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &allocInfo, &m_uploadCmdBuffer);

        // Initially signaled so the first upload skips the wait.
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fenceInfo, nullptr, &m_copyFence);
    }

    // Wait for the previous upload to finish before overwriting the staging
    // buffer.  In practice the GPU copy finishes long before the next video
    // frame arrives (~33 ms at 30 fps vs < 1 ms for a typical copy), so this
    // returns immediately and replaces the former vkQueueWaitIdle() stall.
    if (m_copyPending)
    {
        vkWaitForFences(dev, 1, &m_copyFence, VK_TRUE, UINT64_MAX);
        m_copyPending = false;
    }
    vkResetFences(dev, 1, &m_copyFence);

    // Re-record the upload commands into the persistent command buffer.
    vkResetCommandBuffer(m_uploadCmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_uploadCmdBuffer, &beginInfo);

    // Transition: UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = 0;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(m_uploadCmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy staging → image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {w, h, 1};
    vkCmdCopyBufferToImage(m_uploadCmdBuffer, m_stagingBuffer, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition: TRANSFER_DST → SHADER_READ_ONLY
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(m_uploadCmdBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(m_uploadCmdBuffer);

    // Submit without blocking the CPU.  The graphics queue executes submissions
    // in order, so the render command buffer (submitted later in EndFrame) is
    // guaranteed to start only after this upload completes — no semaphore needed.
    // The fence is checked at the top of the *next* upload to ensure the staging
    // buffer is no longer in use before we overwrite it.
    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &m_uploadCmdBuffer;
    vkQueueSubmit(m_pRenderer->GetGraphicsQueue(), 1, &submitInfo, m_copyFence);
    m_copyPending = true;

    m_dirty = true;   // signal Apply() to re-call Bind() with the new descSet
    return true;
}

// ---------------------------------------------------------------------------
// Bind — tell the renderer to use this texture's descriptor set and pipeline
// ---------------------------------------------------------------------------
bool CTextureFlatVulkan::Bind(const uint32_t /*_index*/)
{
    if (m_isYuv)
    {
        m_pRenderer->SetYuvMode(true);
        if (m_yuvDescSet != VK_NULL_HANDLE)
            m_pRenderer->SetDescriptorSet(m_yuvDescSet);
    }
    else
    {
        m_pRenderer->SetYuvMode(false);
        if (m_descSet != VK_NULL_HANDLE)
            m_pRenderer->SetDescriptorSet(m_descSet);
    }
    m_dirty = false;
    return true;
}

// ---------------------------------------------------------------------------
// BindFrameNV12 — upload Y + UV planes directly; fragment shader converts YCbCr
// ---------------------------------------------------------------------------
bool CTextureFlatVulkan::BindFrameNV12(const uint8_t* yPlane, uint32_t yStride,
                                        const uint8_t* uvPlane, uint32_t uvStride,
                                        uint32_t w, uint32_t h)
{
    VkDevice dev = m_pRenderer->GetDevice();
    bool sizeChanged = (w != m_imgWidth || h != m_imgHeight);

    if (sizeChanged)
    {
        // Destroy old YUV plane resources
        if (m_yView     != VK_NULL_HANDLE) { vkDestroyImageView(dev, m_yView,   nullptr); m_yView   = VK_NULL_HANDLE; }
        if (m_yPlane    != VK_NULL_HANDLE) { vkDestroyImage(dev, m_yPlane,   nullptr);    m_yPlane  = VK_NULL_HANDLE; }
        if (m_yPlaneMem != VK_NULL_HANDLE) { vkFreeMemory(dev, m_yPlaneMem, nullptr);     m_yPlaneMem = VK_NULL_HANDLE; }
        if (m_uvView    != VK_NULL_HANDLE) { vkDestroyImageView(dev, m_uvView,  nullptr); m_uvView   = VK_NULL_HANDLE; }
        if (m_uvPlane   != VK_NULL_HANDLE) { vkDestroyImage(dev, m_uvPlane,  nullptr);    m_uvPlane  = VK_NULL_HANDLE; }
        if (m_uvPlaneMem != VK_NULL_HANDLE) { vkFreeMemory(dev, m_uvPlaneMem, nullptr);   m_uvPlaneMem = VK_NULL_HANDLE; }

        // Helper: create a single-plane VkImage + memory + view
        auto createPlane = [&](uint32_t pw, uint32_t ph, VkFormat fmt,
                               VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool
        {
            VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imgInfo.imageType     = VK_IMAGE_TYPE_2D;
            imgInfo.format        = fmt;
            imgInfo.extent        = {pw, ph, 1};
            imgInfo.mipLevels     = 1;
            imgInfo.arrayLayers   = 1;
            imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(dev, &imgInfo, nullptr, &img) != VK_SUCCESS) return false;

            VkMemoryRequirements memReq;
            vkGetImageMemoryRequirements(dev, img, &memReq);
            VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocInfo.allocationSize  = memReq.size;
            allocInfo.memoryTypeIndex = m_pRenderer->FindMemType(
                memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(dev, &allocInfo, nullptr, &mem) != VK_SUCCESS) return false;
            vkBindImageMemory(dev, img, mem, 0);

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image            = img;
            viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format           = fmt;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return vkCreateImageView(dev, &viewInfo, nullptr, &view) == VK_SUCCESS;
        };

        if (!createPlane(w,   h,   VK_FORMAT_R8_UNORM,   m_yPlane,  m_yPlaneMem,  m_yView))  return false;
        if (!createPlane(w/2, h/2, VK_FORMAT_R8G8_UNORM, m_uvPlane, m_uvPlaneMem, m_uvView)) return false;

        m_imgWidth  = w;
        m_imgHeight = h;

        // Allocate YUV descriptor set once; update it here and whenever the views change
        if (m_yuvDescSet == VK_NULL_HANDLE)
        {
            VkDescriptorSetAllocateInfo dsAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsAlloc.descriptorPool     = m_pRenderer->GetDescriptorPool();
            dsAlloc.descriptorSetCount = 1;
            VkDescriptorSetLayout layout = m_pRenderer->GetYuvDescriptorSetLayout();
            dsAlloc.pSetLayouts        = &layout;
            vkAllocateDescriptorSets(dev, &dsAlloc, &m_yuvDescSet);
        }

        VkDescriptorImageInfo yInfo{};
        yInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        yInfo.imageView   = m_yView;
        yInfo.sampler     = m_pRenderer->GetDefaultSampler();

        VkDescriptorImageInfo uvInfo{};
        uvInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        uvInfo.imageView   = m_uvView;
        uvInfo.sampler     = m_pRenderer->GetDefaultSampler();

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_yuvDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &yInfo;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_yuvDescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &uvInfo;
        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    // Copy Y and UV rows into staging buffer (Y at offset 0, UV follows)
    uint32_t uvW = w / 2, uvH = h / 2;
    VkDeviceSize yBytes  = static_cast<VkDeviceSize>(w)   * h;
    VkDeviceSize uvBytes = static_cast<VkDeviceSize>(uvW) * uvH * 2;
    if (!allocStaging(yBytes + uvBytes)) return false;

    void* mapped;
    vkMapMemory(dev, m_stagingMem, 0, yBytes + uvBytes, 0, &mapped);
    auto* dst = static_cast<uint8_t*>(mapped);
    for (uint32_t row = 0; row < h;   ++row)
        memcpy(dst + row * w,         yPlane  + row * yStride,  w);
    uint8_t* uvDst = dst + yBytes;
    for (uint32_t row = 0; row < uvH; ++row)
        memcpy(uvDst + row * uvW * 2, uvPlane + row * uvStride, uvW * 2);
    vkUnmapMemory(dev, m_stagingMem);

    // Lazily create upload command buffer + fence
    if (m_uploadCmdBuffer == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool        = m_pRenderer->GetCommandPool();
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &allocInfo, &m_uploadCmdBuffer);

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fenceInfo, nullptr, &m_copyFence);
    }

    if (m_copyPending)
    {
        vkWaitForFences(dev, 1, &m_copyFence, VK_TRUE, UINT64_MAX);
        m_copyPending = false;
    }
    vkResetFences(dev, 1, &m_copyFence);
    vkResetCommandBuffer(m_uploadCmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_uploadCmdBuffer, &beginInfo);

    // Transition both planes UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier barriers[2]{};
    for (int i = 0; i < 2; ++i)
    {
        barriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image               = (i == 0) ? m_yPlane : m_uvPlane;
        barriers[i].subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers[i].dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(m_uploadCmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    VkBufferImageCopy yRegion{};
    yRegion.bufferOffset     = 0;
    yRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    yRegion.imageExtent      = {w, h, 1};
    vkCmdCopyBufferToImage(m_uploadCmdBuffer, m_stagingBuffer, m_yPlane,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &yRegion);

    VkBufferImageCopy uvRegion{};
    uvRegion.bufferOffset     = yBytes;
    uvRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    uvRegion.imageExtent      = {uvW, uvH, 1};
    vkCmdCopyBufferToImage(m_uploadCmdBuffer, m_stagingBuffer, m_uvPlane,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uvRegion);

    // Transition both planes TRANSFER_DST → SHADER_READ_ONLY
    for (int i = 0; i < 2; ++i)
    {
        barriers[i].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(m_uploadCmdBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    vkEndCommandBuffer(m_uploadCmdBuffer);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &m_uploadCmdBuffer;
    vkQueueSubmit(m_pRenderer->GetGraphicsQueue(), 1, &submitInfo, m_copyFence);
    m_copyPending = true;

    m_dirty = true;
    return true;
}

// ---------------------------------------------------------------------------
// BindFrame — upload video frame; NV12 goes direct to GPU, others via swscale
// ---------------------------------------------------------------------------
bool CTextureFlatVulkan::BindFrame(ContentDecoder::spCVideoFrame _spFrame)
{
    if (!_spFrame) return false;

    AVFrame* frame = _spFrame->Frame();
    if (!frame) return false;

    uint32_t w = _spFrame->Width();
    uint32_t h = _spFrame->Height();

    // Normalize JPEG-range YUV formats (deprecated in newer FFmpeg)
    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frame->format);
    if      (srcFmt == AV_PIX_FMT_YUVJ420P) srcFmt = AV_PIX_FMT_YUV420P;
    else if (srcFmt == AV_PIX_FMT_YUVJ422P) srcFmt = AV_PIX_FMT_YUV422P;
    else if (srcFmt == AV_PIX_FMT_YUVJ444P) srcFmt = AV_PIX_FMT_YUV444P;
    else if (srcFmt == AV_PIX_FMT_YUVJ440P) srcFmt = AV_PIX_FMT_YUV440P;
    else if (srcFmt == AV_PIX_FMT_YUVJ411P) srcFmt = AV_PIX_FMT_YUV411P;

    // NV12: upload Y+UV planes, convert in the fragment shader (no CPU work)
    if (srcFmt == AV_PIX_FMT_NV12)
    {
        m_isYuv = true;
        return BindFrameNV12(
            frame->data[0], static_cast<uint32_t>(frame->linesize[0]),
            frame->data[1], static_cast<uint32_t>(frame->linesize[1]),
            w, h);
    }

    m_isYuv = false;

    // RGBA: upload directly without conversion
    if (srcFmt == AV_PIX_FMT_RGBA)
    {
        auto spImg = std::make_shared<CImage>();
        spImg->Create(w, h, eImage_RGBA8);
        if (uint8_t* dst = spImg->GetData(0))
        {
            const int srcStride = frame->linesize[0];
            const int dstStride = static_cast<int>(w * 4);
            for (uint32_t row = 0; row < h; ++row)
                memcpy(dst + row * dstStride,
                       frame->data[0] + row * srcStride,
                       static_cast<size_t>(dstStride));
        }
        return Upload(spImg);
    }

    // Fallback: convert to RGBA using swscale
    if (srcFmt == AV_PIX_FMT_NONE) return false;

    SwsContext* sws = sws_getContext(
        static_cast<int>(w), static_cast<int>(h), srcFmt,
        static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws) return false;

    const size_t needed = static_cast<size_t>(w) * h * 4;
    if (m_swscaleBuf.size() < needed)
        m_swscaleBuf.resize(needed);

    uint8_t* dstData[4]    = {m_swscaleBuf.data(), nullptr, nullptr, nullptr};
    int      dstLinesize[4] = {static_cast<int>(w * 4), 0, 0, 0};

    sws_scale(sws, frame->data, frame->linesize,
              0, static_cast<int>(h), dstData, dstLinesize);
    sws_freeContext(sws);

    auto spImg = std::make_shared<CImage>();
    spImg->Create(w, h, eImage_RGBA8);
    if (uint8_t* dst = spImg->GetData(0))
        memcpy(dst, m_swscaleBuf.data(), needed);
    return Upload(spImg);
}

// ---------------------------------------------------------------------------
// Upload from raw buffer
// ---------------------------------------------------------------------------
bool CTextureFlatVulkan::Upload(const uint8_t* data, CImageFormat fmt,
                                 uint32_t w, uint32_t h,
                                 uint32_t /*bytesPerRow*/, bool /*mipMapped*/,
                                 uint32_t /*mipLevel*/)
{
    auto spImg = std::make_shared<CImage>();
    spImg->Create(w, h, eImage_RGBA8);
    if (uint8_t* dst = spImg->GetData(0))
        memcpy(dst, data, static_cast<size_t>(w) * h * 4);
    return Upload(spImg);
}

} // namespace DisplayOutput

#endif // !WIN32
