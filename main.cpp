#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <vulkan/vulkan.h>

struct TextVertex {
  float x, y, u, v;
};

struct DrawBatch {
  SDL_GPUTexture *atlasTexture;
  Uint32 indexOffset;
  Uint32 indexCount;
};

struct AppState {
  ~AppState();
  bool prepareTextGeometry();
  bool buildTextGeometry();

  SDL_Window *window = nullptr;
  SDL_GPUDevice *device = nullptr;
  SDL_GPUGraphicsPipeline *pipeline = nullptr;
  SDL_GPUSampler *sampler = nullptr;

  TTF_TextEngine *textEngine = nullptr;
  TTF_Font *font = nullptr;
  TTF_Text *text = nullptr;

  SDL_GPUBuffer *vertexBuffer = nullptr;
  SDL_GPUBuffer *indexBuffer = nullptr;
  Uint32 vertexBufferSize = 0;
  Uint32 indexBufferSize = 0;

  std::vector<DrawBatch> batches;
  std::string textBuffer = "Hello world";
  std::string geometryKey;
};

AppState::~AppState() {
  if (device)
    SDL_WaitForGPUIdle(device);
  if (text)
    TTF_DestroyText(text);
  if (font)
    TTF_CloseFont(font);
  if (textEngine)
    TTF_DestroyGPUTextEngine(textEngine);

  if (device && vertexBuffer)
    SDL_ReleaseGPUBuffer(device, vertexBuffer);
  if (device && indexBuffer)
    SDL_ReleaseGPUBuffer(device, indexBuffer);
  if (device && sampler)
    SDL_ReleaseGPUSampler(device, sampler);
  if (device && pipeline)
    SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
  if (device && window)
    SDL_ReleaseWindowFromGPUDevice(device, window);
  if (device)
    SDL_DestroyGPUDevice(device);

  if (window)
    SDL_DestroyWindow(window);
}

static bool ensure_gpu_buffer(SDL_GPUDevice *device, SDL_GPUBuffer **buf,
                              Uint32 *currentSize, Uint32 required,
                              SDL_GPUBufferUsageFlags usage) {
  if (*buf && *currentSize >= required)
    return true;

  if (*buf) {
    SDL_ReleaseGPUBuffer(device, *buf);
    *buf = nullptr;
    *currentSize = 0;
  }

  SDL_GPUBufferCreateInfo info{};
  info.usage = usage;
  info.size = required;

  *buf = SDL_CreateGPUBuffer(device, &info);
  if (*buf)
    *currentSize = required;
  return *buf != nullptr;
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

  app->device = SDL_CreateGPUDeviceWithProperties(device_props);
  SDL_DestroyProperties(device_props);
  if (!app->device) {
    SDL_Log("Failed to create GPU device: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  SDL_ClaimWindowForGPUDevice(app->device, app->window);

  app->font = TTF_OpenFont("assets/DejaVuSans.ttf", 14.0f);
  if (!app->font) {
    SDL_Log("TTF_OpenFont failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  TTF_SetFontHinting(app->font, TTF_HINTING_LIGHT);

  app->textEngine = TTF_CreateGPUTextEngine(app->device);
  if (!app->textEngine) {
    SDL_Log("TTF_CreateGPUTextEngine failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_Color white = {255, 255, 255, 255};
  app->text = TTF_CreateText(app->textEngine, app->font, "Hello world", 0);
  if (!app->text) {
    SDL_Log("TTF_CreateText failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  TTF_SetTextColor(app->text, white.r, white.g, white.b, white.a);

  if (not app->buildTextGeometry())
    return SDL_APP_FAILURE;

  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
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

  SDL_GPUShaderCreateInfo vs_info{};
  vs_info.code_size = shader_size;
  vs_info.code = (const Uint8 *)shader_code;
  vs_info.entrypoint = "vs_main";
  vs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs_info.num_samplers = 0;
  vs_info.num_storage_textures = 0;
  vs_info.num_storage_buffers = 0;
  vs_info.num_uniform_buffers = 1;
  SDL_GPUShader *vs_program = SDL_CreateGPUShader(app->device, &vs_info);

  SDL_GPUShaderCreateInfo fs_info{};
  fs_info.code_size = shader_size;
  fs_info.code = (const Uint8 *)shader_code;
  fs_info.entrypoint = "fs_main";
  fs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fs_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fs_info.num_samplers = 1; // combined image sampler
  fs_info.num_storage_textures = 0;
  fs_info.num_storage_buffers = 0;
  fs_info.num_uniform_buffers = 0;
  SDL_GPUShader *fs_program = SDL_CreateGPUShader(app->device, &fs_info);

  SDL_free(shader_code);

  if (!fs_program || !vs_program) {
    SDL_Log("SDL_CreateGPUShader failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  // ---- Vertex input layout matching TextVertex / shader ----
  std::array<SDL_GPUVertexAttribute, 2> attrs{};
  attrs[0].location = 0;
  attrs[0].buffer_slot = 0;
  attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[0].offset = offsetof(TextVertex, x);

  attrs[1].location = 1;
  attrs[1].buffer_slot = 0;
  attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[1].offset = offsetof(TextVertex, u);

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
  pipeInfo.vertex_input_state = vtxInput;
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

bool AppState::buildTextGeometry() {
  batches.clear();

  TTF_GPUAtlasDrawSequence *drawData = TTF_GetGPUTextDrawData(text);
  if (!drawData)
    return true;

  Uint32 totalVertices = 0, totalIndices = 0;
  for (TTF_GPUAtlasDrawSequence *seq = drawData; seq; seq = seq->next) {
    totalVertices += static_cast<Uint32>(seq->num_vertices);
    totalIndices += static_cast<Uint32>(seq->num_indices);
  }

  if (totalVertices == 0 || totalIndices == 0)
    return true;

  std::vector<TextVertex> vertices;
  vertices.reserve(totalVertices);
  std::vector<int> indices;
  indices.reserve(totalIndices);

  Uint32 baseVertex = 0, indexOffset = 0;
  for (TTF_GPUAtlasDrawSequence *seq = drawData; seq; seq = seq->next) {
    for (int i = 0; i < seq->num_vertices; ++i) {
      TextVertex text_vtx{seq->xy[i].x, seq->xy[i].y, seq->uv[i].x,
                          seq->uv[i].y};
      vertices.push_back(text_vtx);
    }
    for (int i = 0; i < seq->num_indices; ++i)
      indices.push_back(seq->indices[i] + (int)baseVertex);
    batches.push_back(
        {seq->atlas_texture, indexOffset, (Uint32)seq->num_indices});
    baseVertex += (Uint32)seq->num_vertices;
    indexOffset += (Uint32)seq->num_indices;
  }

  const Uint32 vbBytes = totalVertices * (Uint32)sizeof(TextVertex);
  const Uint32 ibBytes = totalIndices * (Uint32)sizeof(int);

  if (not ensure_gpu_buffer(device, &vertexBuffer, &vertexBufferSize, vbBytes,
                            SDL_GPU_BUFFERUSAGE_VERTEX)) {
    SDL_Log("Failed to allocate GPU vertex buffer: %s", SDL_GetError());
    batches.clear();
    return false;
  }

  if (not ensure_gpu_buffer(device, &indexBuffer, &indexBufferSize, ibBytes,
                            SDL_GPU_BUFFERUSAGE_INDEX)) {
    SDL_Log("Failed to allocate GPU index buffer: %s", SDL_GetError());
    batches.clear();
    return false;
  }

  SDL_GPUTransferBufferCreateInfo tbInfo{};
  tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  tbInfo.size = vbBytes + ibBytes;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbInfo);
  if (!tb) {
    batches.clear();
    return false;
  }

  void *mapped = SDL_MapGPUTransferBuffer(device, tb, false);
  SDL_memcpy(mapped, vertices.data(), vbBytes);
  SDL_memcpy((Uint8 *)mapped + vbBytes, indices.data(), ibBytes);
  SDL_UnmapGPUTransferBuffer(device, tb);

  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);

  SDL_GPUTransferBufferLocation src{};
  src.transfer_buffer = tb;
  src.offset = 0;

  SDL_GPUBufferRegion vDst{};
  vDst.buffer = vertexBuffer;
  vDst.size = vbBytes;
  SDL_UploadToGPUBuffer(copyPass, &src, &vDst, false);

  src.offset = vbBytes;

  SDL_GPUBufferRegion iDst{};
  iDst.buffer = indexBuffer;
  iDst.size = ibBytes;
  SDL_UploadToGPUBuffer(copyPass, &src, &iDst, false);

  SDL_EndGPUCopyPass(copyPass);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(device, tb);

  return true;
}

bool AppState::prepareTextGeometry() {
  if (geometryKey == textBuffer)
    return 1;
  else if (not TTF_SetTextString(text, textBuffer.c_str(), 0)) {
    SDL_Log("TTF_SetTextString failed: %s", SDL_GetError());
    return 0;
  } else if (not buildTextGeometry())
    return 0;
  else {
    geometryKey = textBuffer;
    return 1;
  }
}

struct FrameRunner {
  AppState *app;
  SDL_GPUCommandBuffer *cmd;
  SDL_GPURenderPass *renderPass;

  void executeRenderPass();
  SDL_AppResult operator()();
};

void FrameRunner::executeRenderPass() {
  if (app->batches.empty())
    return;

  SDL_BindGPUGraphicsPipeline(renderPass, app->pipeline);

  SDL_GPUBufferBinding vb{};
  vb.buffer = app->vertexBuffer;
  SDL_BindGPUVertexBuffers(renderPass, 0, &vb, 1);

  SDL_GPUBufferBinding ib{};
  ib.buffer = app->indexBuffer;
  SDL_BindGPUIndexBuffer(renderPass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

  for (const DrawBatch &btch : app->batches) {
    SDL_GPUTextureSamplerBinding tsb{};
    tsb.texture = btch.atlasTexture;
    tsb.sampler = app->sampler;
    SDL_BindGPUFragmentSamplers(renderPass, 0, &tsb, 1);
    SDL_DrawGPUIndexedPrimitives(renderPass, btch.indexCount, 1,
                                 btch.indexOffset, 0, 0);
  }
}

SDL_AppResult FrameRunner::operator()() {
  cmd = SDL_AcquireGPUCommandBuffer(app->device);
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

  if (not app->prepareTextGeometry())
    return SDL_APP_FAILURE;

  int textW = 0, textH = 0;
  TTF_GetTextSize(app->text, &textW, &textH);
  const int textX = (sw - textW) / 2;
  const int textY = (sh - textH) / 2;

  const float transform[4] = {
      2.0f / static_cast<float>(sw),
      2.0f / static_cast<float>(sh),
      2.0f / static_cast<float>(sw) * static_cast<float>(textX) - 1.0f,
      2.0f / static_cast<float>(sh) * static_cast<float>(textY + textH) - 1.0f,
  };
  SDL_PushGPUVertexUniformData(cmd, 0, transform, sizeof(transform));

  renderPass = SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);
  executeRenderPass();
  SDL_EndGPURenderPass(renderPass);

  SDL_SubmitGPUCommandBuffer(cmd);
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  FrameRunner iterate_frame{};
  iterate_frame.app = (AppState *)appstate;
  return iterate_frame();
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  if (app)
    delete app;
  TTF_Quit();
}
