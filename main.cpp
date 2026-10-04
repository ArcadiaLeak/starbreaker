#include <array>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include <freetype/freetype.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <vulkan/vulkan.h>

struct TextVertex {
  float position[2];
  float uv[2];
};

class AppState {
private:
  SDL_GPUDevice *device = nullptr;
  SDL_Window *window = nullptr;
  SDL_GPUGraphicsPipeline *pipeline = nullptr;
  SDL_GPUSampler *sampler = nullptr;

  FT_Library ft_library = nullptr;
  hb_font_t *hb_font = nullptr;

  hb_buffer_t *textBuffer = nullptr;
  std::string textString = "Hello world";

  std::expected<void, std::string> initialize_font();
  std::expected<void, std::string> initialize_device();
  std::expected<void, std::string> initialize_window();
  std::expected<void, std::string> initialize_sampler();
  std::expected<void, std::string> initialize_pipeline();

  void destroy_font();
  void destroy_freetype();
  void destroy_device();
  void destroy_window();
  void destroy_sampler();
  void destroy_pipeline();

public:
  SDL_AppResult initialize();
  SDL_AppResult iterate();
  void quit();
};

std::expected<void, std::string> AppState::initialize_font() {
  FT_Face ft_face{};
  if (FT_New_Face(ft_library, "assets/DejaVuSans.ttf", 0, &ft_face))
    return std::unexpected{"FT_New_Face failed!"};
  FT_Set_Pixel_Sizes(ft_face, 0, 14);
  hb_font = hb_ft_font_create_referenced(ft_face);
  FT_Done_Face(ft_face);
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_device() {
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
  device = SDL_CreateGPUDeviceWithProperties(device_props);

  SDL_DestroyProperties(device_props);

  if (not device)
    return std::unexpected{
        std::format("Failed to create GPU device: {}", SDL_GetError())};
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_window() {
  window =
      SDL_CreateWindow("Hello GPU", 800, 600,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (not window)
    return std::unexpected{std::format("CreateWindow: {}", SDL_GetError())};
  if (not SDL_ClaimWindowForGPUDevice(device, window))
    return std::unexpected{
        std::format("ClaimWindowForGPUDevice: {}", SDL_GetError())};
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_sampler() {
  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

  sampler = SDL_CreateGPUSampler(device, &samplerInfo);
  if (not sampler)
    return std::unexpected{
        std::format("SDL_CreateGPUSampler failed: {}", SDL_GetError())};
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_pipeline() {
  struct CommonDeleter {
    void operator()(void *m) const {
      if (m)
        SDL_free(m);
    }
  };
  struct ShaderDeleter {
    SDL_GPUDevice *device;
    void operator()(SDL_GPUShader *shader) const {
      if (shader)
        SDL_ReleaseGPUShader(device, shader);
    }
  };

  std::unique_ptr<SDL_GPUShader, ShaderDeleter> vertex_shader{
      nullptr, ShaderDeleter{device}};
  {
    size_t shader_size{};
    std::unique_ptr<void, CommonDeleter> shader_code{
        SDL_LoadFile("text.vert.spv", &shader_size)};
    if (not shader_code)
      return std::unexpected{
          std::format("Failed to load vertex shader: {}", SDL_GetError())};

    SDL_GPUShaderCreateInfo shader_info{};
    shader_info.code_size = shader_size;
    shader_info.code = static_cast<const Uint8 *>(shader_code.get());
    shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shader_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    shader_info.num_samplers = 0;
    shader_info.num_storage_textures = 0;
    shader_info.num_storage_buffers = 0;
    shader_info.num_uniform_buffers = 1;
    vertex_shader.reset(SDL_CreateGPUShader(device, &shader_info));
    if (not vertex_shader)
      return std::unexpected{std::format(
          "SDL_CreateGPUShader for vertex failed: {}", SDL_GetError())};
  }

  std::unique_ptr<SDL_GPUShader, ShaderDeleter> fragment_shader{
      nullptr, ShaderDeleter{device}};
  {
    size_t shader_size{};
    std::unique_ptr<void, CommonDeleter> shader_code{
        SDL_LoadFile("text.frag.spv", &shader_size)};
    if (not shader_code)
      return std::unexpected{
          std::format("Failed to load fragment shader: {}", SDL_GetError())};

    SDL_GPUShaderCreateInfo shader_info{};
    shader_info.code_size = shader_size;
    shader_info.code = static_cast<const Uint8 *>(shader_code.get());
    shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    shader_info.num_samplers = 1; // combined image sampler
    shader_info.num_storage_textures = 0;
    shader_info.num_storage_buffers = 0;
    shader_info.num_uniform_buffers = 0;
    fragment_shader.reset(SDL_CreateGPUShader(device, &shader_info));
    if (not fragment_shader)
      return std::unexpected{std::format(
          "SDL_CreateGPUShader for fragment failed: {}", SDL_GetError())};
  }

  std::array<SDL_GPUVertexAttribute, 2> attrs{};
  attrs[0].location = 0;
  attrs[0].buffer_slot = 0;
  attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[0].offset = offsetof(TextVertex, position);

  attrs[1].location = 1;
  attrs[1].buffer_slot = 0;
  attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[1].offset = offsetof(TextVertex, uv);

  SDL_GPUVertexBufferDescription vtxDesc{};
  vtxDesc.slot = 0;
  vtxDesc.pitch = sizeof(TextVertex);
  vtxDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

  SDL_GPUVertexInputState vtxInput{};
  vtxInput.vertex_buffer_descriptions = &vtxDesc;
  vtxInput.num_vertex_buffers = 1;
  vtxInput.vertex_attributes = attrs.data();
  vtxInput.num_vertex_attributes = 2;

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
  pipeInfo.vertex_shader = vertex_shader.get();
  pipeInfo.fragment_shader = fragment_shader.get();
  pipeInfo.vertex_input_state = vtxInput;
  pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pipeInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  pipeInfo.target_info = targetInfo;

  pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeInfo);
  if (not pipeline)
    return std::unexpected{std::format(
        "SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError())};

  return std::expected<void, std::string>{};
}

SDL_AppResult AppState::initialize() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  } else if (FT_Init_FreeType(&ft_library)) {
    SDL_Log("FT_Init_FreeType failed!");
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_font(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_device(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_window(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_sampler(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_pipeline(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else
    return SDL_APP_CONTINUE;
}

void AppState::quit() {
  destroy_pipeline();
  destroy_sampler();
  destroy_window();
  destroy_device();
  destroy_font();
  destroy_freetype();
  SDL_Quit();
}

void AppState::destroy_font() {
  if (not hb_font)
    return;
  hb_font_destroy(hb_font);
}

void AppState::destroy_freetype() {
  if (not ft_library)
    return;
  FT_Done_FreeType(ft_library);
}

void AppState::destroy_device() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  SDL_DestroyGPUDevice(device);
}

void AppState::destroy_window() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  if (not window)
    return;
  SDL_ReleaseWindowFromGPUDevice(device, window);
  SDL_DestroyWindow(window);
}

void AppState::destroy_sampler() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  if (not sampler)
    return;
  SDL_ReleaseGPUSampler(device, sampler);
}

void AppState::destroy_pipeline() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  if (not sampler)
    return;
  SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  AppState *app = new AppState{};
  *appstate = app;
  return app->initialize();
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  app->quit();
  delete app;
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
