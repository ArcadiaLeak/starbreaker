#include <SDL3/SDL_error.h>
#include <array>
#include <cmath>
#include <cstddef>
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

struct GlyphInfo {
  hb_codepoint_t codepoint;
  std::uint32_t cluster;
  hb_position_t x_advance;
  hb_position_t y_advance;
  hb_position_t x_offset;
  hb_position_t y_offset;
};

static FT_Library global_ft_library = nullptr;

class AppFont {
private:
  FT_Library ft_library = nullptr;
  FT_Face ft_face = nullptr;
  hb_font_t *hb_font = nullptr;

public:
  FT_Face get_face() const noexcept { return ft_face; }
  hb_font_t *get_font() const noexcept { return hb_font; }

  AppFont(FT_Library ft_lib, const char *filepath, FT_UInt pixel_height);
  ~AppFont();

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

AppFont::~AppFont() {
  if (hb_font)
    hb_font_destroy(hb_font);
  if (ft_face)
    FT_Done_Face(ft_face);
}

class AppGPUDevice {
public:
  AppGPUDevice();
  ~AppGPUDevice();

  AppGPUDevice(const AppGPUDevice &) = delete;
  AppGPUDevice &operator=(const AppGPUDevice &) = delete;
  AppGPUDevice(AppGPUDevice &&) = delete;
  AppGPUDevice &operator=(AppGPUDevice &&) = delete;

  SDL_GPUDevice *get() { return gpu_device; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
};

AppGPUDevice::AppGPUDevice() {
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
  gpu_device = SDL_CreateGPUDeviceWithProperties(device_props);
  SDL_DestroyProperties(device_props);

  if (not gpu_device)
    throw std::runtime_error{"Failed to create GPU device!"};
}

AppGPUDevice::~AppGPUDevice() {
  if (not gpu_device)
    return;
  if (not SDL_WaitForGPUIdle(gpu_device)) {
    SDL_Log("SDL_WaitForGPUIdle failed: %s", SDL_GetError());
    std::terminate();
  }
  SDL_DestroyGPUDevice(gpu_device);
}

class AppWindow {
public:
  AppWindow(SDL_GPUDevice *device);
  ~AppWindow();

  AppWindow(const AppWindow &) = delete;
  AppWindow &operator=(const AppWindow &) = delete;
  AppWindow(AppWindow &&) = delete;
  AppWindow &operator=(AppWindow &&) = delete;

  SDL_Window *get() { return window; }

private:
  SDL_Window *window = nullptr;
  SDL_GPUDevice *gpu_device = nullptr;
};

AppWindow::AppWindow(SDL_GPUDevice *device) : gpu_device{device} {
  window =
      SDL_CreateWindow("Hello GPU", 800, 600,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (not window)
    throw std::runtime_error{"SDL_CreateWindow failed!"};
  if (not SDL_ClaimWindowForGPUDevice(device, window))
    throw std::runtime_error{"SDL_ClaimWindowForGPUDevice failed!"};
}

AppWindow::~AppWindow() {
  if (not window)
    return;
  SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
  SDL_DestroyWindow(window);
}

class AppFile {
public:
  AppFile(const char *filepath);
  ~AppFile();

  AppFile(const AppFile &) = delete;
  AppFile &operator=(const AppFile &) = delete;
  AppFile(AppFile &&) = delete;
  AppFile &operator=(AppFile &&) = delete;

  void *get_data() { return file_data; }
  std::size_t get_size() { return file_size; }

private:
  void *file_data = nullptr;
  size_t file_size = 0;
};

AppFile::AppFile(const char *filepath) {
  file_data = SDL_LoadFile(filepath, &file_size);
  if (not file_data) {
    std::string errorMsg{"Failed to load file: "};
    errorMsg.append(filepath);
    throw std::runtime_error{errorMsg};
  }
}

AppFile::~AppFile() {
  if (not file_data)
    return;
  SDL_free(file_data);
}

class AppGPUShader {
public:
  struct CreateInfo {
    const char *filepath;
    SDL_GPUShaderStage stage;
    Uint32 num_samplers;
    Uint32 num_uniform_buffers;
  };

  AppGPUShader(SDL_GPUDevice *device, CreateInfo createInfo);
  ~AppGPUShader();

  AppGPUShader(const AppGPUShader &) = delete;
  AppGPUShader &operator=(const AppGPUShader &) = delete;
  AppGPUShader(AppGPUShader &&) = delete;
  AppGPUShader &operator=(AppGPUShader &&) = delete;

  SDL_GPUShader *get() { return gpu_shader; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUShader *gpu_shader = nullptr;
};

AppGPUShader::AppGPUShader(SDL_GPUDevice *device, CreateInfo createInfo)
    : gpu_device{device} {
  AppFile appFile{createInfo.filepath};

  SDL_GPUShaderCreateInfo shaderInfo{};
  shaderInfo.code_size = appFile.get_size();
  shaderInfo.code = static_cast<const Uint8 *>(appFile.get_data());
  shaderInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  shaderInfo.stage = createInfo.stage;
  shaderInfo.num_samplers = createInfo.num_samplers;
  shaderInfo.num_storage_textures = 0;
  shaderInfo.num_storage_buffers = 0;
  shaderInfo.num_uniform_buffers = createInfo.num_uniform_buffers;
  gpu_shader = SDL_CreateGPUShader(gpu_device, &shaderInfo);
  if (not gpu_shader) {
    std::string errorMsg{"SDL_CreateGPUShader failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

AppGPUShader::~AppGPUShader() {
  if (not gpu_shader)
    return;
  SDL_ReleaseGPUShader(gpu_device, gpu_shader);
}

class AppGPUSampler {
public:
  AppGPUSampler(SDL_GPUDevice *device);
  ~AppGPUSampler();

  AppGPUSampler(const AppGPUSampler &) = delete;
  AppGPUSampler &operator=(const AppGPUSampler &) = delete;
  AppGPUSampler(AppGPUSampler &&) = delete;
  AppGPUSampler &operator=(AppGPUSampler &&) = delete;

  SDL_GPUSampler *get() { return gpu_sampler; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUSampler *gpu_sampler = nullptr;
};

AppGPUSampler::AppGPUSampler(SDL_GPUDevice *device) : gpu_device{device} {
  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

  gpu_sampler = SDL_CreateGPUSampler(gpu_device, &samplerInfo);
  if (not gpu_sampler) {
    std::string errorMsg{"SDL_CreateGPUSampler failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

AppGPUSampler::~AppGPUSampler() {
  if (not gpu_sampler)
    return;
  SDL_ReleaseGPUSampler(gpu_device, gpu_sampler);
}

class MainGPUGraphicsPipeline {
public:
  MainGPUGraphicsPipeline(SDL_GPUDevice *device, SDL_Window *window);
  ~MainGPUGraphicsPipeline();

  MainGPUGraphicsPipeline(const MainGPUGraphicsPipeline &) = delete;
  MainGPUGraphicsPipeline &operator=(const MainGPUGraphicsPipeline &) = delete;
  MainGPUGraphicsPipeline(MainGPUGraphicsPipeline &&) = delete;
  MainGPUGraphicsPipeline &operator=(MainGPUGraphicsPipeline &&) = delete;

  SDL_GPUGraphicsPipeline *get() { return gpu_pipeline; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUGraphicsPipeline *gpu_pipeline = nullptr;
};

MainGPUGraphicsPipeline::MainGPUGraphicsPipeline(SDL_GPUDevice *device,
                                                 SDL_Window *window)
    : gpu_device{device} {
  AppGPUShader::CreateInfo vertexInfo{.filepath = "text.vert.spv",
                                      .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                      .num_samplers = 0,
                                      .num_uniform_buffers = 1};
  AppGPUShader vertexShader{gpu_device, vertexInfo};

  AppGPUShader::CreateInfo fragmentInfo{.filepath = "text.frag.spv",
                                        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                        .num_samplers = 1,
                                        .num_uniform_buffers = 0};
  AppGPUShader fragmentShader{gpu_device, fragmentInfo};

  std::array<SDL_GPUVertexAttribute, 2> attrs{};
  attrs[0].location = 0;
  attrs[0].buffer_slot = 0;
  attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[0].offset = offsetof(Vertex, position);

  attrs[1].location = 1;
  attrs[1].buffer_slot = 0;
  attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[1].offset = offsetof(Vertex, uv);

  SDL_GPUVertexBufferDescription vertexDescription{};
  vertexDescription.slot = 0;
  vertexDescription.pitch = sizeof(Vertex);
  vertexDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

  SDL_GPUVertexInputState vertexInput{};
  vertexInput.vertex_buffer_descriptions = &vertexDescription;
  vertexInput.num_vertex_buffers = 1;
  vertexInput.vertex_attributes = attrs.data();
  vertexInput.num_vertex_attributes = 2;

  SDL_GPUColorTargetDescription colorTarget{};
  colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);
  colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  colorTarget.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  colorTarget.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  colorTarget.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
  targetInfo.color_target_descriptions = &colorTarget;
  targetInfo.num_color_targets = 1;

  SDL_GPUGraphicsPipelineCreateInfo pipeInfo{};
  pipeInfo.vertex_shader = vertexShader.get();
  pipeInfo.fragment_shader = fragmentShader.get();
  pipeInfo.vertex_input_state = vertexInput;
  pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pipeInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  pipeInfo.target_info = targetInfo;

  gpu_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipeInfo);
  if (not gpu_pipeline) {
    std::string errorMsg{"SDL_CreateGPUGraphicsPipeline failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

MainGPUGraphicsPipeline::~MainGPUGraphicsPipeline() {
  if (not gpu_pipeline)
    return;
  SDL_ReleaseGPUGraphicsPipeline(gpu_device, gpu_pipeline);
}

class AppState {
private:
  AppFont app_font;

  AppGPUDevice app_device;
  AppWindow app_window;
  AppGPUSampler app_sampler;
  MainGPUGraphicsPipeline app_pipeline;

  std::string text_string = "Hello world";

public:
  AppState(FT_Library ft_library)
      : app_font{ft_library, "assets/DejaVuSans.ttf", 14}, app_device{},
        app_window{app_device.get()}, app_sampler{app_device.get()},
        app_pipeline{app_device.get(), app_window.get()} {}

  SDL_AppResult iterate() noexcept;
  std::vector<GlyphInfo> shape_text_string() noexcept;
};

SDL_AppResult AppState::iterate() noexcept {
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(app_device.get());
  if (not cmd) {
    SDL_Log("SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_GPUTexture *swapchain = nullptr;
  Uint32 sw = 0, sh = 0;
  if (not SDL_WaitAndAcquireGPUSwapchainTexture(cmd, app_window.get(),
                                                &swapchain, &sw, &sh)) {
    SDL_Log("SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
    std::terminate();
  }

  if (swapchain) {
    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = swapchain;
    colorTarget.clear_color = {0.08f, 0.08f, 0.10f, 1.0f};
    colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *renderPass =
        SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);
    SDL_EndGPURenderPass(renderPass);
  }

  if (SDL_SubmitGPUCommandBuffer(cmd))
    return SDL_APP_CONTINUE;
  else {
    SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
    std::terminate();
  }
}

std::vector<GlyphInfo> AppState::shape_text_string() noexcept {
  hb_buffer_t *textBuffer = hb_buffer_create();

  hb_buffer_add_utf8(textBuffer, text_string.data(), -1, 0, -1);
  hb_buffer_guess_segment_properties(textBuffer);
  hb_shape(app_font.get_font(), textBuffer, nullptr, 0);

  unsigned int glyphCount = 0;
  hb_glyph_info_t *glyphInfos =
      hb_buffer_get_glyph_infos(textBuffer, &glyphCount);
  hb_glyph_position_t *glyphPositions =
      hb_buffer_get_glyph_positions(textBuffer, &glyphCount);

  std::vector<GlyphInfo> glyphInfoVec(glyphCount);
  for (unsigned int i = 0; i < glyphCount; ++i) {
    glyphInfoVec[i].codepoint = glyphInfos[i].codepoint;
    glyphInfoVec[i].cluster = glyphInfos[i].cluster;
    glyphInfoVec[i].x_advance = glyphPositions[i].x_advance;
    glyphInfoVec[i].y_advance = glyphPositions[i].y_advance;
    glyphInfoVec[i].x_offset = glyphPositions[i].x_offset;
    glyphInfoVec[i].y_offset = glyphPositions[i].y_offset;
  }

  hb_buffer_destroy(textBuffer);
  return glyphInfoVec;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  if (not SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
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
    SDL_Log("[App] %s", e.what());
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
