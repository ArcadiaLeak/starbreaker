#include <cstdio>
#include <cstdlib>
#include <cstring>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <vulkan/vulkan.h>

struct AppState {
  SDL_Window *window = nullptr;
  SDL_GPUDevice *device = nullptr;
  SDL_GPUGraphicsPipeline *pipeline = nullptr;
  SDL_GPUTexture *textTexture = nullptr;
  SDL_GPUSampler *sampler = nullptr;
  int textureWidth = 0;
  int textureHeight = 0;
};

static void teardown_app_state(AppState *app) {
  if (app->device && app->sampler)
    SDL_ReleaseGPUSampler(app->device, app->sampler);
  if (app->device && app->textTexture)
    SDL_ReleaseGPUTexture(app->device, app->textTexture);
  if (app->device && app->pipeline)
    SDL_ReleaseGPUGraphicsPipeline(app->device, app->pipeline);
  if (app->device && app->window)
    SDL_ReleaseWindowFromGPUDevice(app->device, app->window);
  if (app->device)
    SDL_DestroyGPUDevice(app->device);
  if (app->window)
    SDL_DestroyWindow(app->window);
}

// Upload an RGBA32 SDL_Surface to a freshly created SDL_GPUTexture.
static SDL_GPUTexture *create_texture_from_surface(SDL_GPUDevice *device,
                                                   SDL_Surface *rgba, int *outW,
                                                   int *outH) {
  *outW = rgba->w;
  *outH = rgba->h;

  SDL_GPUTextureCreateInfo texInfo{};
  texInfo.type = SDL_GPU_TEXTURETYPE_2D;
  texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  texInfo.width = (Uint32)rgba->w;
  texInfo.height = (Uint32)rgba->h;
  texInfo.layer_count_or_depth = 1;
  texInfo.num_levels = 1;
  texInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

  SDL_GPUTexture *tex = SDL_CreateGPUTexture(device, &texInfo);
  if (!tex)
    return nullptr;

  const Uint32 bytes = (Uint32)(rgba->pitch * rgba->h);

  SDL_GPUTransferBufferCreateInfo tbInfo{};
  tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  tbInfo.size = bytes;

  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbInfo);
  void *mapped = SDL_MapGPUTransferBuffer(device, tb, false);
  SDL_memcpy(mapped, rgba->pixels, bytes);
  SDL_UnmapGPUTransferBuffer(device, tb);

  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);

  SDL_GPUTextureTransferInfo src{};
  src.transfer_buffer = tb;
  src.offset = 0;

  SDL_GPUTextureRegion dst{};
  dst.texture = tex;
  dst.w = (Uint32)rgba->w;
  dst.h = (Uint32)rgba->h;
  dst.d = 1;

  SDL_UploadToGPUTexture(pass, &src, &dst, false);

  SDL_EndGPUCopyPass(pass);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(device, tb);

  return tex;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  if (!TTF_Init()) {
    SDL_Log("TTF_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  AppState *app = new AppState{};
  *appstate = app;

  app->window =
      SDL_CreateWindow("Hello GPU", 800, 600,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!app->window) {
    SDL_Log("CreateWindow: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  // ---- GPU device ----
  SDL_GPUVulkanOptions vulkan_options{};
  vulkan_options.vulkan_api_version = VK_API_VERSION_1_3;

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
                        "vulkan");
  SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
                         true);
  SDL_SetBooleanProperty(
      props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
  SDL_SetPointerProperty(props,
                         SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER,
                         &vulkan_options);

  app->device = SDL_CreateGPUDeviceWithProperties(props);
  SDL_DestroyProperties(props);
  if (!app->device) {
    SDL_Log("Failed to create GPU device: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  SDL_ClaimWindowForGPUDevice(app->device, app->window);

  // ---- Render text with SDL3_ttf ----
  TTF_Font *font = TTF_OpenFont("assets/DejaVuSans.ttf", 14.0f);
  if (!font) {
    SDL_Log("TTF_OpenFont failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_Color white = {255, 255, 255, 255};
  SDL_Surface *textSurface = TTF_RenderText_Blended(
      font, "Hello world", 0, white); // 0 = NUL-terminated
  TTF_CloseFont(font);
  if (!textSurface) {
    SDL_Log("TTF_RenderText_Blended failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_Surface *rgba = SDL_ConvertSurface(textSurface, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(textSurface);
  if (!rgba) {
    SDL_Log("SDL_ConvertSurface failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  app->textTexture = create_texture_from_surface(
      app->device, rgba, &app->textureWidth, &app->textureHeight);
  SDL_DestroySurface(rgba);
  if (!app->textTexture) {
    SDL_Log("Failed to create text texture: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  // ---- Sampler ----
  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

  app->sampler = SDL_CreateGPUSampler(app->device, &samplerInfo);
  if (!app->sampler) {
    SDL_Log("SDL_CreateGPUSampler failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  // ---- Shaders ----
  size_t shader_size = 0;
  void *shader_code = SDL_LoadFile("fullscreen.spv", &shader_size);
  if (!shader_code) {
    SDL_Log("Failed to load shader: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_GPUShaderCreateInfo fs_info{};
  fs_info.code_size = shader_size;
  fs_info.code = (const Uint8 *)shader_code;
  fs_info.entrypoint = "fs_main";
  fs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fs_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fs_info.num_samplers = 1; // <-- we now sample the text texture
  fs_info.num_storage_textures = 0;
  fs_info.num_storage_buffers = 0;
  fs_info.num_uniform_buffers = 0;
  SDL_GPUShader *fs_program = SDL_CreateGPUShader(app->device, &fs_info);

  SDL_GPUShaderCreateInfo vs_info{};
  vs_info.code_size = shader_size;
  vs_info.code = (const Uint8 *)shader_code;
  vs_info.entrypoint = "vs_main";
  vs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs_info.num_samplers = 0;
  vs_info.num_storage_textures = 0;
  vs_info.num_storage_buffers = 0;
  vs_info.num_uniform_buffers = 0;
  SDL_GPUShader *vs_program = SDL_CreateGPUShader(app->device, &vs_info);

  SDL_free(shader_code);

  if (!fs_program || !vs_program) {
    SDL_Log("SDL_CreateGPUShader failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  // ---- Pipeline (with alpha blending for the text) ----
  SDL_GPUColorTargetDescription colorTarget{};
  colorTarget.format =
      SDL_GetGPUSwapchainTextureFormat(app->device, app->window);
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
  pipeInfo.vertex_shader = vs_program;
  pipeInfo.fragment_shader = fs_program;
  pipeInfo.vertex_input_state = SDL_GPUVertexInputState{};
  pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pipeInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  pipeInfo.target_info = targetInfo;

  app->pipeline = SDL_CreateGPUGraphicsPipeline(app->device, &pipeInfo);

  SDL_ReleaseGPUShader(app->device, fs_program);
  SDL_ReleaseGPUShader(app->device, vs_program);

  if (!app->pipeline) {
    SDL_Log("SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  (void)appstate;
  if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_KEY_DOWN) {
    return SDL_APP_SUCCESS;
  }
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  AppState *app = (AppState *)appstate;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(app->device);
  if (!cmd)
    return SDL_APP_FAILURE;

  SDL_GPUTexture *swapchain = nullptr;
  Uint32 sw = 0, sh = 0;
  SDL_WaitAndAcquireGPUSwapchainTexture(cmd, app->window, &swapchain, &sw, &sh);

  if (!swapchain) {
    SDL_SubmitGPUCommandBuffer(cmd);
    return SDL_APP_CONTINUE;
  }

  SDL_GPUColorTargetInfo colorTarget{};
  colorTarget.texture = swapchain;
  colorTarget.clear_color = {0.08f, 0.08f, 0.10f, 1.0f};
  colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
  colorTarget.store_op = SDL_GPU_STOREOP_STORE;

  SDL_GPURenderPass *pass =
      SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);

  // Center the text quad by setting the viewport. The fullscreen
  // triangle in the vertex shader gets mapped into this rect.
  SDL_GPUViewport viewport{};
  int vx = ((int)sw - app->textureWidth)  / 2;   // integer division
  int vy = ((int)sh - app->textureHeight) / 2;
  viewport.x = (float)vx;
  viewport.y = (float)vy;
  viewport.w = (float)app->textureWidth;
  viewport.h = (float)app->textureHeight;
  viewport.min_depth = 0.0f;
  viewport.max_depth = 1.0f;
  SDL_SetGPUViewport(pass, &viewport);

  SDL_BindGPUGraphicsPipeline(pass, app->pipeline);

  SDL_GPUTextureSamplerBinding binding{};
  binding.texture = app->textTexture;
  binding.sampler = app->sampler;
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

  SDL_EndGPURenderPass(pass);
  SDL_SubmitGPUCommandBuffer(cmd);

  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  (void)result;
  AppState *app = (AppState *)appstate;
  if (app) {
    teardown_app_state(app);
    delete app;
  }
  TTF_Quit();
}
