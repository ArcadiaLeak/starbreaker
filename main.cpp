#include <QDebug>
#include <QGuiApplication>
#include <QVulkanDeviceFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>
#include <QVulkanWindowRenderer>

// 1. The Renderer: Clears the screen every frame.
class MinimalVulkanRenderer : public QVulkanWindowRenderer
{
public:
  MinimalVulkanRenderer(QVulkanWindow *w) : m_window(w) {}

  void startNextFrame() override
  {
    // Get the current command buffer.
    VkCommandBuffer cmdBuf = m_window->currentCommandBuffer();

    // Begin a render pass using the window's default render pass.
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = m_window->defaultRenderPass();
    beginInfo.framebuffer = m_window->currentFramebuffer();
    beginInfo.renderArea.extent = {uint32_t(m_window->width()),
                                   uint32_t(m_window->height())};

    // The clear color (a nice shade of blue).
    VkClearValue clearValues[2];
    clearValues[0].color = {{0.2f, 0.4f, 0.8f, 1.0f}}; // Color buffer
    clearValues[1].depthStencil = {1.0f, 0};           // Depth buffer

    beginInfo.clearValueCount = 2;
    beginInfo.pClearValues = clearValues;

    QVulkanDeviceFunctions *devFuncs =
        m_window->vulkanInstance()->deviceFunctions(m_window->device());
    // Record the commands to begin and end the render pass.
    devFuncs->vkCmdBeginRenderPass(cmdBuf, &beginInfo,
                                   VK_SUBPASS_CONTENTS_INLINE);
    devFuncs->vkCmdEndRenderPass(cmdBuf);

    // Signal that the frame is ready.
    m_window->frameReady();

    // Request an update for a continuous render loop.
    m_window->requestUpdate();
  }

private:
  QVulkanWindow *m_window;
};

// 2. The Window: Provides the renderer.
class MinimalVulkanWindow : public QVulkanWindow
{
public:
  QVulkanWindowRenderer *createRenderer() override
  {
    return new MinimalVulkanRenderer(this);
  }
};

int main(int argc, char *argv[])
{
  QGuiApplication app(argc, argv);

  // Create the Vulkan instance first. It must outlive the window.
  QVulkanInstance inst;
  // Optional: Enable validation layers for debugging.
  // inst.setLayers({ "VK_LAYER_KHRONOS_validation" });
  if (!inst.create())
  {
    qFatal("Failed to create Vulkan instance: %d", inst.errorCode());
  }

  // Create the window and associate it with the instance.
  MinimalVulkanWindow window;
  window.setVulkanInstance(&inst);
  window.resize(800, 600);
  window.setTitle("Minimal Qt Vulkan Window");
  window.show();

  return app.exec();
}
