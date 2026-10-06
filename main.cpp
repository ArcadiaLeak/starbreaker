#include <SDL3/SDL_init.h>
#include <array>
#include <cmath>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
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

class FTLibraryScoped {
private:
  FT_Library ft_library = nullptr;

public:
  FT_Library get() const noexcept { return ft_library; }

  FTLibraryScoped() {
    if (FT_Init_FreeType(&ft_library))
      throw std::runtime_error{"FT_Init_FreeType failed!"};
  }
  ~FTLibraryScoped() noexcept {
    if (ft_library)
      FT_Done_FreeType(ft_library);
  }

  FTLibraryScoped(const FTLibraryScoped &) = delete;
  FTLibraryScoped &operator=(const FTLibraryScoped &) = delete;
  FTLibraryScoped(FTLibraryScoped &&) = delete;
  FTLibraryScoped &operator=(FTLibraryScoped &&) = delete;
};

class HBFontScoped {
private:
  std::shared_ptr<FTLibraryScoped> ft_library;
  FT_Face ft_face = nullptr;
  hb_font_t *hb_font = nullptr;

public:
  FT_Face get_face() const noexcept { return ft_face; }
  hb_font_t *get_font() const noexcept { return hb_font; }

  HBFontScoped(std::shared_ptr<FTLibraryScoped> ft_lib, const char *filepath,
               FT_UInt pixel_height);
  ~HBFontScoped();

  HBFontScoped(const HBFontScoped &) = delete;
  HBFontScoped &operator=(const HBFontScoped &) = delete;
  HBFontScoped(HBFontScoped &&) = delete;
  HBFontScoped &operator=(HBFontScoped &&) = delete;
};

HBFontScoped::HBFontScoped(std::shared_ptr<FTLibraryScoped> ft_lib,
                           const char *filepath, FT_UInt pixel_height)
    : ft_library{std::move(ft_lib)} {
  if (FT_New_Face(ft_library->get(), filepath, 0, &ft_face))
    throw std::runtime_error{"FT_New_Face failed!"};
  if (FT_Set_Pixel_Sizes(ft_face, 0, pixel_height)) {
    FT_Done_Face(ft_face);
    ft_face = nullptr;
    throw std::runtime_error{"FT_Set_Pixel_Sizes failed!"};
  }
  hb_font = hb_ft_font_create(ft_face, nullptr);
}

HBFontScoped::~HBFontScoped() noexcept {
  if (hb_font)
    hb_font_destroy(hb_font);
  if (ft_face)
    FT_Done_Face(ft_face);
}

class AppState {
private:
  hb_font_t *hb_font;
  hb_buffer_t *textBuffer;

  SDL_GPUDevice *device;
  SDL_Window *window;
  SDL_GPUSampler *sampler;
  SDL_GPUGraphicsPipeline *pipeline;

  std::string textString = "Hello world";

public:
  SDL_AppResult iterate();
  SDL_AppResult initialize();
};

SDL_AppResult AppState::initialize() {
  if (not SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  try {
    FTLibraryScoped ft_library_scoped{};
    return SDL_APP_CONTINUE;
  } catch (const std::runtime_error &e) {
    SDL_Log("%s", e.what());
    return SDL_APP_FAILURE;
  }
}

SDL_AppResult AppState::iterate() {
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  if (not cmd)
    return SDL_APP_FAILURE;

  SDL_GPUTexture *swapchain = nullptr;
  Uint32 sw = 0, sh = 0;
  SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &sw, &sh);
  if (not swapchain) {
    SDL_SubmitGPUCommandBuffer(cmd);
    return SDL_APP_CONTINUE;
  }

  SDL_GPUColorTargetInfo colorTarget{};
  colorTarget.texture = swapchain;
  colorTarget.clear_color = {0.08f, 0.08f, 0.10f, 1.0f};
  colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
  colorTarget.store_op = SDL_GPU_STOREOP_STORE;

  SDL_GPURenderPass *renderPass =
      SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);
  SDL_EndGPURenderPass(renderPass);

  SDL_SubmitGPUCommandBuffer(cmd);
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  AppState *app = new AppState{};
  *appstate = app;
  return app->initialize();
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  delete app;
  SDL_Quit();
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
