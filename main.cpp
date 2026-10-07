#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include <freetype/freetype.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <vulkan/vulkan.h>

struct Vertex {
  float position[2];
  float uv[2];
};

struct GlyphTexture {
  SDL_GPUTexture *texture = nullptr;
  int width = 0;
  int height = 0;
  int bitmap_left = 0;
  int bitmap_top = 0;
  float x_advance = 0.0f;
  float x_offset = 0.0f;
  float y_offset = 0.0f;
};

static FT_Library global_ft_library = nullptr;
static SDL_GPUDevice *global_gpu_device = nullptr;

static bool initialize_gpu_device() {
  SDL_GPUVulkanOptions vulkan_options{};
  vulkan_options.vulkan_api_version = VK_API_VERSION_1_3;

  SDL_PropertiesID device_props = SDL_CreateProperties();
  SDL_SetStringProperty(device_props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
                        "vulkan");
  SDL_SetBooleanProperty(device_props,
                         SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
  SDL_SetBooleanProperty(
      device_props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
  SDL_SetPointerProperty(device_props,
                         SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER,
                         &vulkan_options);
  global_gpu_device = SDL_CreateGPUDeviceWithProperties(device_props);
  SDL_DestroyProperties(device_props);

  return static_cast<bool>(global_gpu_device);
}

class AppFont {
private:
  FT_Library ft_library = nullptr;
  FT_Face ft_face = nullptr;
  hb_font_t *hb_font = nullptr;

public:
  FT_Face get_face() const noexcept { return ft_face; }
  hb_font_t *get_font() const noexcept { return hb_font; }

  AppFont(FT_Library ft_lib, const char *filepath, FT_UInt pixel_height);
  ~AppFont() noexcept;

  AppFont(const AppFont &) = delete;
  AppFont &operator=(const AppFont &) = delete;
  AppFont(AppFont &&) = delete;
  AppFont &operator=(AppFont &&) = delete;
};

AppFont::AppFont(FT_Library ft_lib, const char *filepath, FT_UInt pixel_height)
    : ft_library{ft_lib} {
  if (FT_New_Face(ft_library, filepath, 0, &ft_face))
    throw std::runtime_error{"FT_New_Face failed!"};
  if (FT_Set_Pixel_Sizes(ft_face, 0, pixel_height)) {
    FT_Done_Face(ft_face);
    ft_face = nullptr;
    throw std::runtime_error{"FT_Set_Pixel_Sizes failed!"};
  }
  hb_font = hb_ft_font_create(ft_face, nullptr);
}

AppFont::~AppFont() noexcept {
  if (hb_font)
    hb_font_destroy(hb_font);
  if (ft_face)
    FT_Done_Face(ft_face);
}

class AppGPUCommandBuffer {
public:
  struct Empty {};
  struct AcquiredBuffer {
    SDL_GPUCommandBuffer *buffer = nullptr;
  };
  struct AcquiredTexture {
    SDL_GPUCommandBuffer *buffer = nullptr;
    SDL_GPUTexture *swapchain_texture = nullptr;
    Uint32 swapchain_texture_width = 0;
    Uint32 swapchain_texture_height = 0;
  };
  using Status = std::variant<Empty, AcquiredBuffer, AcquiredTexture>;

  AppGPUCommandBuffer(SDL_GPUDevice *device);
  ~AppGPUCommandBuffer();

  AppGPUCommandBuffer(const AppGPUCommandBuffer &) = delete;
  AppGPUCommandBuffer &operator=(const AppGPUCommandBuffer &) = delete;
  AppGPUCommandBuffer(AppGPUCommandBuffer &&) = delete;
  AppGPUCommandBuffer &operator=(AppGPUCommandBuffer &&) = delete;

  void wait_and_acquire_swapchain_texture(SDL_Window *window);
  void submit_now();
  void cancel_now();

  SDL_GPUCommandBuffer *get_buffer() const;
  SDL_GPUTexture *get_swapchain_texture() const;

private:
  Status status;
};

AppGPUCommandBuffer::AppGPUCommandBuffer(SDL_GPUDevice *device) {
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  if (not cmd)
    throw std::runtime_error{"SDL_AcquireGPUCommandBuffer failed!"};
  status = AcquiredBuffer{cmd};
}

AppGPUCommandBuffer::~AppGPUCommandBuffer() {
  if (std::holds_alternative<Empty>(status))
    return;
  if (AcquiredBuffer *acquired = std::get_if<AcquiredBuffer>(&status);
      acquired) {
    if (not SDL_CancelGPUCommandBuffer(acquired->buffer))
      SDL_Log("Finalization failed: %s", SDL_GetError());
    return;
  }
  if (AcquiredTexture *acquired = std::get_if<AcquiredTexture>(&status);
      acquired) {
    if (not SDL_SubmitGPUCommandBuffer(acquired->buffer))
      SDL_Log("Finalization failed: %s", SDL_GetError());
    return;
  }
  std::unreachable();
}

void AppGPUCommandBuffer::wait_and_acquire_swapchain_texture(
    SDL_Window *window) {
  if (not std::holds_alternative<AcquiredBuffer>(status))
    return;
  AcquiredTexture acquiredTexture{get_buffer()};
  bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(
      std::get<AcquiredBuffer>(status).buffer, window,
      &acquiredTexture.swapchain_texture,
      &acquiredTexture.swapchain_texture_width,
      &acquiredTexture.swapchain_texture_height);
  if (not ok)
    throw std::runtime_error{"SDL_WaitAndAcquireGPUSwapchainTexture failed!"};
  status = acquiredTexture;
}

void AppGPUCommandBuffer::submit_now() {
  SDL_GPUCommandBuffer *cmd = get_buffer();
  if (not cmd)
    return;
  if (not SDL_SubmitGPUCommandBuffer(cmd))
    throw std::runtime_error{"SDL_SubmitGPUCommandBuffer failed!"};
  status.emplace<Empty>();
}

void AppGPUCommandBuffer::cancel_now() {
  SDL_GPUCommandBuffer *cmd = get_buffer();
  if (not cmd)
    return;
  if (not SDL_CancelGPUCommandBuffer(cmd))
    throw std::runtime_error{"SDL_CancelGPUCommandBuffer failed!"};
  status.emplace<Empty>();
}

SDL_GPUCommandBuffer *AppGPUCommandBuffer::get_buffer() const {
  if (std::holds_alternative<AcquiredBuffer>(status))
    return std::get<AcquiredBuffer>(status).buffer;
  if (std::holds_alternative<AcquiredTexture>(status))
    return std::get<AcquiredTexture>(status).buffer;
  return nullptr;
}

SDL_GPUTexture *AppGPUCommandBuffer::get_swapchain_texture() const {
  if (std::holds_alternative<AcquiredTexture>(status))
    return std::get<AcquiredTexture>(status).swapchain_texture;
  return nullptr;
}

class AppGPURenderPass {
public:
  AppGPURenderPass(AppGPUCommandBuffer *appCmd,
                   SDL_GPUColorTargetInfo *colorTarget)
      : app_cmd{appCmd} {
    render_pass =
        SDL_BeginGPURenderPass(app_cmd->get_buffer(), colorTarget, 1, nullptr);
  }

  ~AppGPURenderPass() {
    if (not app_cmd->get_buffer())
      return;
    SDL_EndGPURenderPass(render_pass);
  }

private:
  AppGPUCommandBuffer *app_cmd = nullptr;
  SDL_GPURenderPass *render_pass = nullptr;
};

class AppState {
private:
  AppFont app_font;

  SDL_Window *window;
  SDL_GPUSampler *sampler;
  SDL_GPUGraphicsPipeline *pipeline;

  std::string textString = "Hello world";

public:
  AppState(FT_Library ft_library)
      : app_font{ft_library, "assets/DejaVuSans.ttf", 14} {}
  SDL_AppResult iterate();
};

SDL_AppResult AppState::iterate() {
  AppGPUCommandBuffer appCmd{global_gpu_device};
  appCmd.wait_and_acquire_swapchain_texture(window);

  SDL_GPUTexture *swapchain = appCmd.get_swapchain_texture();
  if (not swapchain)
    return SDL_APP_CONTINUE;

  SDL_GPUColorTargetInfo colorTarget{};
  colorTarget.texture = swapchain;
  colorTarget.clear_color = {0.08f, 0.08f, 0.10f, 1.0f};
  colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
  colorTarget.store_op = SDL_GPU_STOREOP_STORE;

  {
    AppGPURenderPass renderPass{&appCmd, &colorTarget};
  }
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  if (not SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  if (not initialize_gpu_device()) {
    SDL_Log("Failed to create GPU device: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  if (FT_Init_FreeType(&global_ft_library)) {
    SDL_Log("FT_Init_FreeType failed!");
    return SDL_APP_FAILURE;
  }
  try {
    AppState *app = new AppState{global_ft_library};
    *appstate = app;
    return SDL_APP_CONTINUE;
  } catch (const std::runtime_error &e) {
    SDL_Log("App error: %s", e.what());
    return SDL_APP_FAILURE;
  }
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  delete app;
  if (global_ft_library) {
    FT_Done_FreeType(global_ft_library);
    global_ft_library = nullptr;
  }
  if (global_gpu_device) {
    SDL_WaitForGPUIdle(global_gpu_device);
    SDL_DestroyGPUDevice(global_gpu_device);
    global_gpu_device = nullptr;
  }
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  AppState *app = (AppState *)appstate;
  return app->iterate();
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_KEY_DOWN)
    return SDL_APP_SUCCESS;
  else
    return SDL_APP_CONTINUE;
}
