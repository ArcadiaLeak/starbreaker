#include <cstdio>
#include <cstdlib>
#include <cstring>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include <vulkan/vulkan.h>

struct AppState {
  SDL_Window *window;
  SDL_GPUDevice *device;
  SDL_GPUGraphicsPipeline *pipeline;
  SDL_GPUTexture *textTexture;
  SDL_GPUSampler *sampler;
  int textureWidth, textureHeight;
};

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  AppState *app = (AppState *)SDL_calloc(1, sizeof(AppState));
  *appstate = app;

  app->window = SDL_CreateWindow("Hello GPU", 800, 600, SDL_WINDOW_RESIZABLE);
  if (!app->window) {
    SDL_Log("CreateWindow: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

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

  size_t shader_size;
  void *shader_code = SDL_LoadFile("fullscreen.spv", &shader_size);
  if (!shader_code) {
    SDL_Log("Failed to load file: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_GPUShaderCreateInfo fs_info{
      .code_size = shader_size,
      .code = (const Uint8 *)shader_code,
      .entrypoint = "fs_main",
      .format = SDL_GPU_SHADERFORMAT_SPIRV,
      .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
      .num_samplers = 0,
      .num_storage_textures = 0,
      .num_storage_buffers = 0,
      .num_uniform_buffers = 0,
  };
  SDL_GPUShader *fs_program = SDL_CreateGPUShader(app->device, &fs_info);

  SDL_GPUShaderCreateInfo vs_info{
      .code_size = shader_size,
      .code = (const Uint8 *)shader_code,
      .entrypoint = "vs_main",
      .format = SDL_GPU_SHADERFORMAT_SPIRV,
      .stage = SDL_GPU_SHADERSTAGE_VERTEX,
      .num_samplers = 0,
      .num_storage_textures = 0,
      .num_storage_buffers = 0,
      .num_uniform_buffers = 0,
  };
  SDL_GPUShader *vs_program = SDL_CreateGPUShader(app->device, &vs_info);

  SDL_GPUColorTargetDescription colorTarget = {
      .format = SDL_GetGPUSwapchainTextureFormat(app->device, app->window),
  };
  SDL_GPUGraphicsPipelineTargetInfo targetInfo = {
      .color_target_descriptions = &colorTarget,
      .num_color_targets = 1,
  };
  SDL_GPUGraphicsPipelineCreateInfo pipeInfo = {
      .vertex_shader = vs_program,
      .fragment_shader = fs_program,
      .vertex_input_state = SDL_GPUVertexInputState{},
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL,
                           .cull_mode = SDL_GPU_CULLMODE_NONE},
      .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
      .target_info = targetInfo,
  };
  app->pipeline = SDL_CreateGPUGraphicsPipeline(app->device, &pipeInfo);

  SDL_ReleaseGPUShader(app->device, fs_program);
  SDL_ReleaseGPUShader(app->device, vs_program);
  SDL_free(shader_code);

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_KEY_DOWN) {
    return SDL_APP_SUCCESS;
  }
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  AppState *app = (AppState *)appstate;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(app->device);
  SDL_GPUTexture *swapchain;
  Uint32 sw, sh;
  SDL_WaitAndAcquireGPUSwapchainTexture(cmd, app->window, &swapchain, &sw, &sh);
  if (!swapchain) {
    SDL_SubmitGPUCommandBuffer(cmd);
    return SDL_APP_CONTINUE;
  }

  SDL_GPUColorTargetInfo colorTarget{
      .texture = swapchain,
      .clear_color = {1.0f, 1.0f, 1.0f, 1.0f},
      .load_op = SDL_GPU_LOADOP_CLEAR,
      .store_op = SDL_GPU_STOREOP_STORE,
  };
  SDL_GPURenderPass *pass =
      SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, app->pipeline);

  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
  SDL_SubmitGPUCommandBuffer(cmd);

  return SDL_APP_CONTINUE;
}

static void teardown_device_and_related(AppState *app) {
  if (!app->device)
    return;
  if (app->sampler)
    SDL_ReleaseGPUSampler(app->device, app->sampler);
  if (app->textTexture)
    SDL_ReleaseGPUTexture(app->device, app->textTexture);
  if (app->pipeline)
    SDL_ReleaseGPUGraphicsPipeline(app->device, app->pipeline);
  if (app->window)
    SDL_ReleaseWindowFromGPUDevice(app->device, app->window);
  SDL_DestroyGPUDevice(app->device);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  if (!app)
    return;

  teardown_device_and_related(app);

  if (app->window)
    SDL_DestroyWindow(app->window);

  SDL_free(app);
}
