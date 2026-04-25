#ifndef _TEXTUREFLATVULKAN_H_
#define _TEXTUREFLATVULKAN_H_

#include "TextureFlat.h"
#include <atomic>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

// Forward declaration to avoid circular include
namespace DisplayOutput { class CRendererVulkan; }

namespace DisplayOutput
{

/*
    CTextureFlatVulkan.
    A flat (2-D) texture backed by a VkImage.
    Uploading data goes through a HOST_VISIBLE staging buffer.
*/
class CTextureFlatVulkan : public CTextureFlat
{
    // Back-pointer to renderer for device / command pool access
    CRendererVulkan* m_pRenderer = nullptr;

    // Vulkan objects
    VkImage        m_image      = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMem   = VK_NULL_HANDLE;
    VkImageView    m_imageView  = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet   = VK_NULL_HANDLE;

    uint32_t m_imgWidth  = 0;
    uint32_t m_imgHeight = 0;
    bool     m_dirty     = false;

    // Reusable scratch buffer for swscale YUV→RGBA conversion (avoids per-frame heap alloc)
    std::vector<uint8_t> m_swscaleBuf;

    // Staging buffer (re-used across uploads to the same texture)
    VkBuffer       m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMem    = VK_NULL_HANDLE;
    VkDeviceSize   m_stagingSize   = 0;

    // Persistent upload command buffer + fence for async uploads.
    // The fence signals when the GPU has finished consuming the staging buffer,
    // so we can safely overwrite it with new frame data on the next upload.
    VkCommandBuffer      m_uploadCmdBuffer = VK_NULL_HANDLE;
    VkFence              m_copyFence       = VK_NULL_HANDLE;
    std::atomic<bool>    m_copyPending{false};

    // NV12 YUV planes (GPU-side colour conversion path)
    VkImage         m_yPlane    = VK_NULL_HANDLE;
    VkDeviceMemory  m_yPlaneMem = VK_NULL_HANDLE;
    VkImageView     m_yView     = VK_NULL_HANDLE;
    VkImage         m_uvPlane    = VK_NULL_HANDLE;
    VkDeviceMemory  m_uvPlaneMem = VK_NULL_HANDLE;
    VkImageView     m_uvView     = VK_NULL_HANDLE;
    VkDescriptorSet m_yuvDescSet = VK_NULL_HANDLE;
    bool            m_isYuv      = false;

    bool allocStaging(VkDeviceSize size);
    bool uploadToImage(uint32_t w, uint32_t h);
    bool BindFrameNV12(const uint8_t* yPlane, uint32_t yStride,
                       const uint8_t* uvPlane, uint32_t uvStride,
                       uint32_t w, uint32_t h);

  public:
    CTextureFlatVulkan(CRendererVulkan* renderer, const uint32_t flags);
    virtual ~CTextureFlatVulkan();

    // CTextureFlat interface
    virtual bool Upload(spCImage _spImage) override;
    virtual bool Upload(const uint8_t* data, CImageFormat fmt,
                        uint32_t w, uint32_t h,
                        uint32_t bytesPerRow, bool mipMapped,
                        uint32_t mipLevel);
    virtual bool BindFrame(ContentDecoder::spCVideoFrame _spFrame) override;
    virtual bool Bind(const uint32_t _index) override;
    virtual bool Dirty() override { return m_dirty; }

    VkDescriptorSet DescSet() const { return m_descSet; }

    // Explicitly free Vulkan resources while the device is still alive.
    // Called by the renderer destructor before vkDestroyDevice.
    // After this the destructor becomes a no-op (all handles are NULL).
    void DestroyVulkanResources();
};

MakeSmartPointers(CTextureFlatVulkan);

} // namespace DisplayOutput

#endif
