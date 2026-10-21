#include "alchemy/darwin/metal/bridge.hpp"

#include "alchemy/darwin/metal/interop.hpp"

// Metal's side of QuartzCore's presentation contract (A7,
// src/alchemy/darwin/quartzcore/quartzcore_vulkan.hpp): every MTLDevice object is
// registered with QuartzCore as a MetalVulkanDevice, which hands QuartzCore
// the Vulkan instance/device/queue and wraps its drawable images as
// MTLTextures. Drawables are then presented by message (-[MTLCommandBuffer
// presentDrawable:] sends -present after the submission, metal_objects.cpp).
//
// The contract header lives in QuartzCore's directory; without that provider
// in the build this file compiles to nothing and no drawables exist.
#if __has_include("alchemy/darwin/quartzcore/quartzcore_vulkan.hpp")
#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"
#define ALCHEMY_METAL_QUARTZCORE_LINK 1
#endif

namespace alchemy::metal::bridge {

#if defined(ALCHEMY_METAL_QUARTZCORE_LINK) && defined(ALCHEMY_HAVE_VULKAN_HEADERS)

namespace {

class QuartzCoreDevice final : public gui::quartzcore::MetalVulkanDevice {
public:
    explicit QuartzCoreDevice(std::shared_ptr<Device> device) : device_(std::move(device)) {}

    PFN_vkGetInstanceProcAddr get_instance_proc_addr() const override { return device_->vk.vkGetInstanceProcAddr; }
    VkInstance instance() const override { return device_->instance; }
    VkPhysicalDevice physical_device() const override { return device_->physical; }
    VkDevice device() const override { return device_->device; }
    std::uint32_t queue_family() const override { return device_->queue_family; }
    void with_queue(const std::function<void(VkQueue)>& work) override {
        std::lock_guard lock(device_->queue_mutex());
        work(device_->queue);
    }
    std::uint64_t wrap_drawable_texture(const std::shared_ptr<gui::quartzcore::DrawableImage>& image) override {
        if (!image || !image->image) throw std::runtime_error("Alchemy Metal: drawable without an image");
        TextureDescriptor descriptor;
        descriptor.type = TextureType::type2d;
        descriptor.pixel_format = static_cast<std::uint32_t>(image->pixel_format);
        descriptor.width = image->extent.width;
        descriptor.height = image->extent.height;
        descriptor.usage = image->texture_usage;
        descriptor.options = ResourceOptions{StorageMode::private_};
        // The layout is QuartzCore's DrawableImage::layout; the aliasing
        // pointer keeps the image object alive with the texture.
        std::shared_ptr<std::atomic<VkImageLayout>> layout(image, &image->layout);
        auto texture = Texture::wrap(device_, image->image, descriptor, image->layout.load(), std::move(layout), image->usage);
        if (texture->format != image->format)
            throw std::runtime_error("Alchemy Metal: drawable image format does not match MTLPixelFormat " +
                                     std::to_string(image->pixel_format));
        return new_texture_object(std::move(texture));
    }

private:
    std::shared_ptr<Device> device_;
};

} // namespace

void publish_to_quartzcore(Address device_object, const std::shared_ptr<Device>& device) {
    gui::quartzcore::register_metal_device(device_object, std::make_shared<QuartzCoreDevice>(device));
}

#else

void publish_to_quartzcore(Address, const std::shared_ptr<Device>&) {}

#endif

} // namespace alchemy::metal::bridge
