#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <generator>
#include <inplace_vector>
#include <map>
#include <memory>
#include <meta>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include <freetype/freetype.h>
#include <freetype/ftglyph.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <unicode/utext.h>
#include <vulkan/vulkan.h>

struct RectVertexInstance {
  float rect_pos[2];
  float rect_size[2];
  float uv_rect[4];
};

struct DifferenceBlendArea {
  std::uint32_t rect[4];
  float color[4];
};

static FT_Library globalFTLibrary = nullptr;

struct FreetypeFaceDeleter {
  void operator()(FT_Face freetypeFace) const { FT_Done_Face(freetypeFace); }
};
using UniqueFreetypeFace = std::unique_ptr<FT_FaceRec, FreetypeFaceDeleter>;

struct FreetypeGlyphDeleter {
  void operator()(FT_Glyph freetypeGlyph) const;
};
using UniqueFreetypeGlyph = std::unique_ptr<FT_GlyphRec, FreetypeGlyphDeleter>;
void FreetypeGlyphDeleter::operator()(FT_Glyph freetypeGlyph) const {
  FT_Done_Glyph(freetypeGlyph);
}

FT_BitmapGlyph asBitmapGlyph(FT_Glyph freetypeGlyph) {
  if (freetypeGlyph->format != FT_GLYPH_FORMAT_BITMAP)
    throw std::runtime_error{"Unexpected FT_Glyph format!"};
  return reinterpret_cast<FT_BitmapGlyph>(freetypeGlyph);
}

struct HarfbuzzFontDeleter {
  void operator()(hb_font_t *harfbuzzFont) const;
};
using UniqueHarfbuzzFont = std::unique_ptr<hb_font_t, HarfbuzzFontDeleter>;
void HarfbuzzFontDeleter::operator()(hb_font_t *harfbuzzFont) const {
  hb_font_destroy(harfbuzzFont);
}

struct HarfbuzzBufferDeleter {
  void operator()(hb_buffer_t *harfbuzzBuffer) const;
};
using UniqueHarfbuzzBuffer =
    std::unique_ptr<hb_buffer_t, HarfbuzzBufferDeleter>;
void HarfbuzzBufferDeleter::operator()(hb_buffer_t *harfbuzzBuffer) const {
  hb_buffer_destroy(harfbuzzBuffer);
}

UniqueHarfbuzzFont createHarfbuzzFont(FT_Library library, const char *filepath,
                                      FT_F26Dot6 fontsize, float displayScale) {
  UniqueFreetypeFace face{};
  FT_Face facePtr{};
  if (FT_New_Face(library, filepath, 0, &facePtr))
    throw std::runtime_error{"FT_New_Face failed!"};
  face.reset(facePtr);
  FT_UInt fontDPI = static_cast<FT_UInt>(96.0f * displayScale);
  if (FT_Set_Char_Size(face.get(), 0, fontsize, fontDPI, fontDPI))
    throw std::runtime_error{"FT_Set_Char_Size failed!"};
  return UniqueHarfbuzzFont{hb_ft_font_create_referenced(face.get())};
}

struct GPUDeviceDeleter {
  void operator()(SDL_GPUDevice *device) const;
};
using UniqueGPUDevice = std::unique_ptr<SDL_GPUDevice, GPUDeviceDeleter>;
void GPUDeviceDeleter::operator()(SDL_GPUDevice *device) const {
  if (not SDL_WaitForGPUIdle(device))
    SDL_Log("SDL_WaitForGPUIdle failed: %s", SDL_GetError());
  SDL_DestroyGPUDevice(device);
}

UniqueGPUDevice createGPUDevice() {
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
  SDL_GPUDevice *device = SDL_CreateGPUDeviceWithProperties(device_props);
  SDL_DestroyProperties(device_props);

  if (not device)
    throw std::runtime_error{"Failed to create GPU device!"};
  return UniqueGPUDevice{device};
}

constexpr int INITIAL_WINDOW_W = 800;
constexpr int INITIAL_WINDOW_H = 600;

struct WindowDeleter {
  void operator()(SDL_Window *window) const;
};
using UniqueWindow = std::unique_ptr<SDL_Window, WindowDeleter>;
void WindowDeleter::operator()(SDL_Window *window) const {
  SDL_DestroyWindow(window);
}

UniqueWindow createWindow(const char *title, int width, int height) {
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  SDL_Window *window = SDL_CreateWindow(title, width, height, flags);
  if (not window) {
    std::string errorMsg{"SDL_CreateWindow failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueWindow{window};
}

struct WindowForGPUDeviceDeleter {
  SDL_GPUDevice *device;
  void operator()(SDL_Window *window) const;
};
using UniqueWindowForGPUDevice =
    std::unique_ptr<SDL_Window, WindowForGPUDeviceDeleter>;
void WindowForGPUDeviceDeleter::operator()(SDL_Window *window) const {
  SDL_ReleaseWindowFromGPUDevice(device, window);
}

UniqueWindowForGPUDevice claimWindowForGPUDevice(SDL_GPUDevice *device,
                                                 SDL_Window *window) {
  if (not SDL_ClaimWindowForGPUDevice(device, window)) {
    std::string errorMsg{"SDL_ClaimWindowForGPUDevice failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueWindowForGPUDevice{window, WindowForGPUDeviceDeleter{device}};
}

float getWindowDisplayScale(SDL_Window *window) {
  float scale = SDL_GetWindowDisplayScale(window);
  if (scale == 0.0f) {
    std::string errorMsg{"SDL_GetWindowDisplayScale failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return scale;
}

void setWindowMinimumSize(SDL_Window *window, int minW, int minH) {
  if (not SDL_SetWindowMinimumSize(window, minW, minH)) {
    std::string errorMsg{"SDL_SetWindowMinimumSize failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
}

struct FileContentDeleter {
  void operator()(void *fileContent) const { SDL_free(fileContent); }
};
using UniqueFileContent = std::unique_ptr<void, FileContentDeleter>;

UniqueFileContent loadFile(const char *filepath, size_t *datasize) {
  void *fileContent = SDL_LoadFile(filepath, datasize);
  if (fileContent)
    return UniqueFileContent{fileContent};
  std::string errorMsg =
      std::format("Failed to load file: {}. {}", filepath, SDL_GetError());
  throw std::runtime_error{std::move(errorMsg)};
}

struct GPUShaderCreateInfo {
  const char *filepath;
  SDL_GPUShaderStage stage;
  Uint32 num_samplers;
  Uint32 num_uniform_buffers;
};

struct GPUShaderDeleter {
  SDL_GPUDevice *device;
  void operator()(SDL_GPUShader *shader) const;
};
using UniqueGPUShader = std::unique_ptr<SDL_GPUShader, GPUShaderDeleter>;
void GPUShaderDeleter::operator()(SDL_GPUShader *shader) const {
  SDL_ReleaseGPUShader(device, shader);
}

UniqueGPUShader createGPUShader(SDL_GPUDevice *device,
                                GPUShaderCreateInfo createInfo) {
  SDL_GPUShaderCreateInfo shaderInfo{};
  UniqueFileContent shaderCode =
      loadFile(createInfo.filepath, &shaderInfo.code_size);
  shaderInfo.code = static_cast<const Uint8 *>(shaderCode.get());
  shaderInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  shaderInfo.stage = createInfo.stage;
  shaderInfo.num_samplers = createInfo.num_samplers;
  shaderInfo.num_uniform_buffers = createInfo.num_uniform_buffers;
  SDL_GPUShader *shader = SDL_CreateGPUShader(device, &shaderInfo);
  if (not shader) {
    std::string errorMsg{"SDL_CreateGPUShader failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUShader{shader, GPUShaderDeleter{device}};
}

struct GPUSamplerDeleter {
  SDL_GPUDevice *device;
  void operator()(SDL_GPUSampler *sampler) const;
};
using UniqueGPUSampler = std::unique_ptr<SDL_GPUSampler, GPUSamplerDeleter>;
void GPUSamplerDeleter::operator()(SDL_GPUSampler *sampler) const {
  SDL_ReleaseGPUSampler(device, sampler);
}

UniqueGPUSampler createCommonGPUSampler(SDL_GPUDevice *device) {
  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

  SDL_GPUSampler *sampler = SDL_CreateGPUSampler(device, &samplerInfo);
  if (not sampler) {
    std::string errorMsg{"SDL_CreateGPUSampler failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUSampler{sampler, GPUSamplerDeleter{device}};
}

struct GPUTextureDeleter {
  SDL_GPUDevice *device = nullptr;
  void operator()(SDL_GPUTexture *texture) const;
};
using UniqueGPUTexture = std::unique_ptr<SDL_GPUTexture, GPUTextureDeleter>;
void GPUTextureDeleter::operator()(SDL_GPUTexture *texture) const {
  SDL_ReleaseGPUTexture(device, texture);
}

struct GPUTextureCreateInfo {
  SDL_GPUTextureFormat format;
  SDL_GPUTextureUsageFlags usage;
  std::uint32_t texture_w;
  std::uint32_t texture_h;
};
UniqueGPUTexture createGPUTexture(SDL_GPUDevice *device,
                                  GPUTextureCreateInfo createInfo) {
  SDL_GPUTextureCreateInfo textureInfo{};
  textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
  textureInfo.format = createInfo.format;
  textureInfo.usage = createInfo.usage;
  textureInfo.width = createInfo.texture_w;
  textureInfo.height = createInfo.texture_h;
  textureInfo.layer_count_or_depth = 1;
  textureInfo.num_levels = 1;
  textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &textureInfo);
  if (not texture) {
    std::string errorMsg{"SDL_CreateGPUTexture failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return {texture, GPUTextureDeleter{device}};
}

struct GPUTransferBufferDeleter {
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUTransferBuffer *transferBuffer) const;
};
using UniqueGPUTransferBuffer =
    std::unique_ptr<SDL_GPUTransferBuffer, GPUTransferBufferDeleter>;
void GPUTransferBufferDeleter::operator()(
    SDL_GPUTransferBuffer *transferBuffer) const {
  SDL_ReleaseGPUTransferBuffer(device_ctx, transferBuffer);
}

struct GPUTransferBufferUnmapper {
  SDL_GPUDevice *device_ctx;
  SDL_GPUTransferBuffer *transfer_buffer;
  void operator()(void *mappedBuffer) const;
};
using UniqueGPUMappedBuffer = std::unique_ptr<void, GPUTransferBufferUnmapper>;
void GPUTransferBufferUnmapper::operator()(void *mappedBuffer) const {
  SDL_UnmapGPUTransferBuffer(device_ctx, transfer_buffer);
}

UniqueGPUTransferBuffer createGPUTransferBuffer(SDL_GPUDevice *device,
                                                std::uint32_t bufferSize) {
  SDL_GPUTransferBufferCreateInfo bufferInfo{};
  bufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  bufferInfo.size = bufferSize;
  SDL_GPUTransferBuffer *transferBuffer =
      SDL_CreateGPUTransferBuffer(device, &bufferInfo);
  if (not transferBuffer) {
    std::string errorMsg{"SDL_CreateGPUTransferBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUTransferBuffer{transferBuffer,
                                 GPUTransferBufferDeleter{device}};
}

UniqueGPUMappedBuffer
mapGPUTransferBuffer(SDL_GPUDevice *device,
                     SDL_GPUTransferBuffer *transferBuffer, bool cycle) {
  void *mappedBuffer = SDL_MapGPUTransferBuffer(device, transferBuffer, cycle);
  if (not mappedBuffer) {
    std::string errorMsg{"SDL_MapGPUTransferBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUMappedBuffer{
      mappedBuffer, GPUTransferBufferUnmapper{device, transferBuffer}};
}

struct GPUBufferDeleter {
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUBuffer *buffer) const;
};
using UniqueGPUBuffer = std::unique_ptr<SDL_GPUBuffer, GPUBufferDeleter>;
void GPUBufferDeleter::operator()(SDL_GPUBuffer *buffer) const {
  SDL_ReleaseGPUBuffer(device_ctx, buffer);
}

UniqueGPUBuffer createGPUBuffer(SDL_GPUDevice *device,
                                const SDL_GPUBufferCreateInfo *createInfo) {
  SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, createInfo);
  if (not buffer) {
    std::string errorMsg{"SDL_CreateGPUBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUBuffer{buffer, GPUBufferDeleter{device}};
}

struct GPUGraphicsPipelineDeleter {
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUGraphicsPipeline *pipeline) const;
};
using UniqueGPUGraphicsPipeline =
    std::unique_ptr<SDL_GPUGraphicsPipeline, GPUGraphicsPipelineDeleter>;
void GPUGraphicsPipelineDeleter::operator()(
    SDL_GPUGraphicsPipeline *pipeline) const {
  SDL_ReleaseGPUGraphicsPipeline(device_ctx, pipeline);
}

UniqueGPUGraphicsPipeline
createGPUGraphicsPipeline(SDL_GPUDevice *device,
                          SDL_GPUGraphicsPipelineCreateInfo *pipelineInfo) {
  SDL_GPUGraphicsPipeline *pipeline =
      SDL_CreateGPUGraphicsPipeline(device, pipelineInfo);
  if (not pipeline) {
    std::string errorMsg{"SDL_CreateGPUGraphicsPipeline failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUGraphicsPipeline{pipeline,
                                   GPUGraphicsPipelineDeleter{device}};
}

UniqueGPUGraphicsPipeline
createGlyphGPUGraphicsPipeline(SDL_GPUDevice *device,
                               SDL_GPUTextureFormat targetFormat) {
  GPUShaderCreateInfo vertexInfo{.filepath = "rect_instance.spv",
                                 .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                 .num_samplers = 0,
                                 .num_uniform_buffers = 1};
  UniqueGPUShader vertexShader = createGPUShader(device, vertexInfo);

  GPUShaderCreateInfo fragmentInfo{.filepath = "coverage_tint.spv",
                                   .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 1,
                                   .num_uniform_buffers = 1};
  UniqueGPUShader fragmentShader = createGPUShader(device, fragmentInfo);

  SDL_GPUColorTargetDescription colorTarget{};
  colorTarget.format = targetFormat;
  colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  colorTarget.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  colorTarget.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  colorTarget.blend_state.color_write_mask =
      SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
      SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
  colorTarget.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
  targetInfo.color_target_descriptions = &colorTarget;
  targetInfo.num_color_targets = 1;

  std::array<SDL_GPUVertexAttribute, 3> vertexAttributes{};
  std::get<0>(vertexAttributes).location = 0;
  std::get<0>(vertexAttributes).buffer_slot = 0;
  std::get<0>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<0>(vertexAttributes).offset = offsetof(RectVertexInstance, rect_pos);
  std::get<1>(vertexAttributes).location = 1;
  std::get<1>(vertexAttributes).buffer_slot = 0;
  std::get<1>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<1>(vertexAttributes).offset =
      offsetof(RectVertexInstance, rect_size);
  std::get<2>(vertexAttributes).location = 2;
  std::get<2>(vertexAttributes).buffer_slot = 0;
  std::get<2>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  std::get<2>(vertexAttributes).offset = offsetof(RectVertexInstance, uv_rect);

  SDL_GPUVertexBufferDescription vertexBufferDescriptions{};
  vertexBufferDescriptions.slot = 0;
  vertexBufferDescriptions.pitch = sizeof(RectVertexInstance);
  vertexBufferDescriptions.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  vertexBufferDescriptions.instance_step_rate = 0;

  SDL_GPUVertexInputState vertexInputState{};
  vertexInputState.vertex_buffer_descriptions = &vertexBufferDescriptions;
  vertexInputState.num_vertex_buffers = 1;
  vertexInputState.vertex_attributes = vertexAttributes.data();
  vertexInputState.num_vertex_attributes = vertexAttributes.size();

  SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.vertex_shader = vertexShader.get();
  pipelineInfo.fragment_shader = fragmentShader.get();
  pipelineInfo.vertex_input_state = vertexInputState;
  pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipelineInfo.target_info = targetInfo;

  return createGPUGraphicsPipeline(device, &pipelineInfo);
}

UniqueGPUGraphicsPipeline
createSelectionGPUGraphicsPipeline(SDL_GPUDevice *device) {
  GPUShaderCreateInfo vertexInfo{.filepath = "rect_instance.spv",
                                 .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                 .num_samplers = 0,
                                 .num_uniform_buffers = 1};
  UniqueGPUShader vertexShader = createGPUShader(device, vertexInfo);

  GPUShaderCreateInfo fragmentInfo{.filepath = "selection_box.spv",
                                   .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 0,
                                   .num_uniform_buffers = 1};
  UniqueGPUShader fragmentShader = createGPUShader(device, fragmentInfo);

  SDL_GPUColorTargetDescription colorTarget{};
  colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  colorTarget.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  colorTarget.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  colorTarget.blend_state.color_write_mask =
      SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
      SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
  colorTarget.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
  targetInfo.color_target_descriptions = &colorTarget;
  targetInfo.num_color_targets = 1;

  std::array<SDL_GPUVertexAttribute, 3> vertexAttributes{};
  std::get<0>(vertexAttributes).location = 0;
  std::get<0>(vertexAttributes).buffer_slot = 0;
  std::get<0>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<0>(vertexAttributes).offset = offsetof(RectVertexInstance, rect_pos);
  std::get<1>(vertexAttributes).location = 1;
  std::get<1>(vertexAttributes).buffer_slot = 0;
  std::get<1>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<1>(vertexAttributes).offset =
      offsetof(RectVertexInstance, rect_size);
  std::get<2>(vertexAttributes).location = 2;
  std::get<2>(vertexAttributes).buffer_slot = 0;
  std::get<2>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  std::get<2>(vertexAttributes).offset = offsetof(RectVertexInstance, uv_rect);

  SDL_GPUVertexBufferDescription vertexBufferDescriptions{};
  vertexBufferDescriptions.slot = 0;
  vertexBufferDescriptions.pitch = sizeof(RectVertexInstance);
  vertexBufferDescriptions.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  vertexBufferDescriptions.instance_step_rate = 0;

  SDL_GPUVertexInputState vertexInputState{};
  vertexInputState.vertex_buffer_descriptions = &vertexBufferDescriptions;
  vertexInputState.num_vertex_buffers = 1;
  vertexInputState.vertex_attributes = vertexAttributes.data();
  vertexInputState.num_vertex_attributes = vertexAttributes.size();

  SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.vertex_shader = vertexShader.get();
  pipelineInfo.fragment_shader = fragmentShader.get();
  pipelineInfo.vertex_input_state = vertexInputState;
  pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipelineInfo.target_info = targetInfo;

  return createGPUGraphicsPipeline(device, &pipelineInfo);
}

struct GPUComputePipelineDeleter {
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUComputePipeline *pipeline) const;
};
using UniqueGPUComputePipeline =
    std::unique_ptr<SDL_GPUComputePipeline, GPUComputePipelineDeleter>;
void GPUComputePipelineDeleter::operator()(
    SDL_GPUComputePipeline *pipeline) const {
  SDL_ReleaseGPUComputePipeline(device_ctx, pipeline);
}

UniqueGPUComputePipeline
createGPUComputePipeline(SDL_GPUDevice *device,
                         SDL_GPUComputePipelineCreateInfo *pipelineInfo) {
  SDL_GPUComputePipeline *pipeline =
      SDL_CreateGPUComputePipeline(device, pipelineInfo);
  if (not pipeline) {
    std::string errorMsg{"SDL_CreateGPUComputePipeline failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUComputePipeline{pipeline, GPUComputePipelineDeleter{device}};
}

UniqueGPUComputePipeline
createDifferenceBlendGPUPipeline(SDL_GPUDevice *device) {
  SDL_GPUComputePipelineCreateInfo pipelineInfo{};
  UniqueFileContent shaderCode =
      loadFile("difference_blend.spv", &pipelineInfo.code_size);
  pipelineInfo.code = static_cast<const Uint8 *>(shaderCode.get());
  pipelineInfo.entrypoint = "main";
  pipelineInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  pipelineInfo.num_readonly_storage_textures = 2;
  pipelineInfo.num_readwrite_storage_textures = 1;
  pipelineInfo.threadcount_x = 8;
  pipelineInfo.threadcount_y = 8;
  pipelineInfo.threadcount_z = 1;
  return createGPUComputePipeline(device, &pipelineInfo);
}

struct GPUCommandBufferSubmitter {
  void operator()(SDL_GPUCommandBuffer *commandBuffer) const;
};
using EngagedGPUCommandBuffer =
    std::unique_ptr<SDL_GPUCommandBuffer, GPUCommandBufferSubmitter>;
void GPUCommandBufferSubmitter::operator()(
    SDL_GPUCommandBuffer *commandBuffer) const {
  if (not SDL_SubmitGPUCommandBuffer(commandBuffer))
    SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
}

struct GPUCommandBufferCanceller {
  void operator()(SDL_GPUCommandBuffer *commandBuffer) const;
};
using DismissedGPUCommandBuffer =
    std::unique_ptr<SDL_GPUCommandBuffer, GPUCommandBufferCanceller>;
void GPUCommandBufferCanceller::operator()(
    SDL_GPUCommandBuffer *commandBuffer) const {
  if (not SDL_CancelGPUCommandBuffer(commandBuffer))
    SDL_Log("SDL_CancelGPUCommandBuffer failed: %s", SDL_GetError());
}

EngagedGPUCommandBuffer acquireGPUCommandBuffer(SDL_GPUDevice *device) {
  SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
  if (not commandBuffer) {
    std::string errorMsg{"SDL_AcquireGPUCommandBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return EngagedGPUCommandBuffer{commandBuffer};
}

struct GPURenderPassDeleter {
  void operator()(SDL_GPURenderPass *renderPass) const;
};
using UniqueGPURenderPass =
    std::unique_ptr<SDL_GPURenderPass, GPURenderPassDeleter>;
void GPURenderPassDeleter::operator()(SDL_GPURenderPass *renderPass) const {
  SDL_EndGPURenderPass(renderPass);
}

UniqueGPURenderPass
beginGPURenderPass(SDL_GPUCommandBuffer *commandBuffer,
                   const SDL_GPUColorTargetInfo *targetInfo) {
  SDL_GPURenderPass *renderPass =
      SDL_BeginGPURenderPass(commandBuffer, targetInfo, 1, nullptr);
  return UniqueGPURenderPass{renderPass};
}

SDL_GPUTexture *waitAndAcquireGPUSwapchainTexture(
    SDL_GPUCommandBuffer *commandBuffer, SDL_Window *window,
    std::uint32_t *texture_width, std::uint32_t *texture_height) {
  SDL_GPUTexture *texture{};
  if (not SDL_WaitAndAcquireGPUSwapchainTexture(
          commandBuffer, window, &texture, texture_width, texture_height)) {
    std::string errorMsg{"SDL_WaitAndAcquireGPUSwapchainTexture failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return texture;
}

struct GPUCopyPassDeleter {
  void operator()(SDL_GPUCopyPass *copyPass) const;
};
using UniqueGPUCopyPass = std::unique_ptr<SDL_GPUCopyPass, GPUCopyPassDeleter>;
void GPUCopyPassDeleter::operator()(SDL_GPUCopyPass *copyPass) const {
  SDL_EndGPUCopyPass(copyPass);
}

struct GPUComputePassDeleter {
  void operator()(SDL_GPUComputePass *computePass) const;
};
using UniqueGPUComputePass =
    std::unique_ptr<SDL_GPUComputePass, GPUComputePassDeleter>;
void GPUComputePassDeleter::operator()(SDL_GPUComputePass *computePass) const {
  SDL_EndGPUComputePass(computePass);
}

UniqueGPUComputePass beginGPUComputePass(
    SDL_GPUCommandBuffer *commandBuffer,
    const SDL_GPUStorageTextureReadWriteBinding *textureBinding) {
  SDL_GPUComputePass *computePass =
      SDL_BeginGPUComputePass(commandBuffer, textureBinding, 1, nullptr, 0);
  return UniqueGPUComputePass{computePass};
}

struct UTextDeleter {
  void operator()(UText *utextPtr) const { utext_close(utextPtr); }
};
using UniqueUText = std::unique_ptr<UText, UTextDeleter>;

UniqueUText utextOpenUTF8(std::string_view strView) {
  UErrorCode status{U_ZERO_ERROR};
  UText *utextPtr =
      utext_openUTF8(nullptr, strView.data(),
                     static_cast<std::int64_t>(strView.size()), &status);
  if (U_FAILURE(status))
    throw std::runtime_error{std::string{"utext_openUTF8 failed: "} +
                             u_errorName(status)};
  return UniqueUText{utextPtr};
}

struct CodepointInfo {
  std::uint32_t byte_offset;
  UChar32 codepoint;
};
std::generator<CodepointInfo> emitCodepointInfo(std::string_view strView) {
  UniqueUText u8Text = utextOpenUTF8(strView);
  while (true) {
    std::int64_t byteOffset = utext_getNativeIndex(u8Text.get());
    UChar32 codepoint = utext_next32(u8Text.get());
    if (codepoint == U_SENTINEL)
      co_return;
    CodepointInfo codepointInfo{static_cast<std::uint32_t>(byteOffset),
                                codepoint};
    co_yield codepointInfo;
  }
}

struct AppState;

struct GlyphSequencer {
  AppState *app;
  UniqueHarfbuzzBuffer harfbuzz_buffer;

  struct GlyphAtlasInfo {
    std::uint32_t x, y, page;
  };
  struct GlyphCacheItem {
    UniqueFreetypeGlyph freetype_glyph;
    std::unique_ptr<GlyphAtlasInfo> atlas;
  };
  using GlyphCache = std::map<hb_codepoint_t, GlyphCacheItem>;
  GlyphCache glyph_cache;

  struct AtlasCopier {
    UniqueGPUMappedBuffer mapped_atlas;
    const GlyphAtlasInfo *atlas_info;
    const FT_Bitmap *bitmap;
    void copy_bitmap(const GlyphCacheItem &cacheItem);
    void copy_bitmap_row(std::uint32_t row);
  };
  static constexpr std::uint32_t ATLAS_SIDE = 1024;
  static constexpr std::uint32_t ATLAS_PADDING = 1;
  UniqueGPUTransferBuffer atlas_transfer;
  UniqueGPUTexture atlas_texture;

  hb_position_t horz_min = 0, horz_max = 0;
  hb_position_t vert_min = 0, vert_max = 0;
  int measured_w() { return static_cast<int>(horz_max - horz_min + 63) / 64; }
  int measured_h() { return static_cast<int>(vert_max - vert_min + 63) / 64; }

  struct ClusterElement {
    std::uint32_t cluster;
    std::optional<CodepointInfo> codepoint_info;
    const hb_glyph_position_t *position;
  };
  std::vector<ClusterElement> cluster_elements;

  UniqueGPUBuffer instance_buffer;
  UniqueGPUTransferBuffer instance_transfer;
  std::uint32_t instance_capacity = 0;

  std::optional<RectVertexInstance>
  create_vertex_instance(unsigned int glyphIndex, hb_position_t penX);
  std::generator<RectVertexInstance> emit_vertex_instances();

  void verify_instance_capacity();
  void transfer_vertex_instances();
  void queue_vertex_copypass();

  UniqueGPUSampler sampler;
  UniqueGPUGraphicsPipeline swapchain_pipeline;
  UniqueGPUGraphicsPipeline offscreen_pipeline;

  void instantiate(SDL_Window *window);
  void glyphs_shape(std::string_view textView);
  void verify_glyph_cache();
  void pack_atlas();
  void measure_text();
  std::generator<ClusterElement> match_cluster_elements();
};

struct CopyGlyphAtlas {
  GlyphSequencer *sequencer;
};
struct CopyGlyphVertices {
  GlyphSequencer *sequencer;
};
struct CopySelectionAreas {
  AppState *app;
};
using CopypassTask =
    std::variant<CopyGlyphAtlas, CopyGlyphVertices, CopySelectionAreas>;
struct CopypassTable {
  std::optional<CopyGlyphAtlas> glyph_atlas;
  std::optional<CopyGlyphVertices> glyph_vertices;
  std::optional<CopySelectionAreas> selection_areas;
};
struct CopypassTaskVisitor {
  void operator()(CopyGlyphAtlas task) const;
  void operator()(CopyGlyphVertices task) const;
  void operator()(CopySelectionAreas task) const;
  SDL_GPUCopyPass *copypass;
};

struct GPUCommandSequencer {
  AppState *app;
  EngagedGPUCommandBuffer command_buffer;

  void initiate();
  void issue_copy_pass();
  void issue_render_pass();
};

struct GPURenderSequencer {
  AppState *app;
  GPUCommandSequencer *command_sequencer;

  std::uint32_t swapchain_w, swapchain_h;
  SDL_GPUTexture *swapchain_texture;

  void draw_scene(SDL_GPUTexture *texture, SDL_GPUGraphicsPipeline *pipeline);
  void verify_storage_textures();
  void selected_areas_draw();
  void selected_areas_blend();
  void draw_processed_image();
};

struct AppState {
  UniqueHarfbuzzFont harfbuzz_font;

  UniqueGPUDevice device_ctx;
  UniqueWindow main_window;
  UniqueWindowForGPUDevice main_window_for_gpu_device;

  std::string caption_string = "Hello figure kerning Text";
  int caption_x = 0, caption_y = 0;
  GlyphSequencer caption_sequencer{this};

  UniqueFreetypeGlyph load_freetype_glyph(hb_codepoint_t codepoint);

  UniqueGPUTexture offscreen_texture;
  UniqueGPUTexture selection_texture;
  UniqueGPUTexture processed_texture;
  std::uint32_t offscreen_w = 0, offscreen_h = 0;

  std::optional<std::array<std::uint32_t, 4>> current_selection;
  std::vector<std::array<std::uint32_t, 4>> selection_areas;

  UniqueGPUGraphicsPipeline selection_pipeline;
  UniqueGPUComputePipeline difference_blend_pipeline;

  UniqueGPUBuffer selection_instances;
  UniqueGPUTransferBuffer selection_transfer;
  std::uint32_t selection_instances_capacity = 0;

  void verify_selection_capacity();
  void transfer_selection_areas();
  void selection_reflow();

  CopypassTable copypass_tasks;

  SDL_AppResult iterate() noexcept;
  SDL_AppResult handle_event(SDL_Event *event) noexcept;

  void instantiate(FT_Library library);

  void caption_measure(SDL_Rect &captionRect) noexcept;
  void caption_reflow();
};

void AppState::instantiate(FT_Library library) {
  device_ctx = createGPUDevice();
  main_window = createWindow("Hello GPU", INITIAL_WINDOW_W, INITIAL_WINDOW_H);
  setWindowMinimumSize(main_window.get(), INITIAL_WINDOW_W, INITIAL_WINDOW_H);
  main_window_for_gpu_device =
      claimWindowForGPUDevice(device_ctx.get(), main_window.get());
  harfbuzz_font = createHarfbuzzFont(library, "assets/DejaVuSans.ttf", 24 * 64,
                                     getWindowDisplayScale(main_window.get()));
  selection_pipeline = createSelectionGPUGraphicsPipeline(device_ctx.get());
  difference_blend_pipeline =
      createDifferenceBlendGPUPipeline(device_ctx.get());
  caption_sequencer.instantiate(main_window.get());
}

void GPUCommandSequencer::initiate() {
  command_buffer = acquireGPUCommandBuffer(app->device_ctx.get());
}

void GPUCommandSequencer::issue_copy_pass() {
  constexpr std::meta::info tableType = ^^CopypassTable;
  constexpr std::meta::access_context tableAccess =
      std::meta::access_context::unprivileged();
  static constexpr auto tableMembers = std::define_static_array(
      std::meta::nonstatic_data_members_of(tableType, tableAccess));

  std::inplace_vector<CopypassTask, tableMembers.size()> taskQueue{};
  template for (constexpr std::meta::info tableMember : tableMembers) {
    if (not app->copypass_tasks.[:tableMember:])
      continue;
    taskQueue.push_back(*app->copypass_tasks.[:tableMember:]);
    app->copypass_tasks.[:tableMember:].reset();
  }
  if (taskQueue.empty())
    return;
  UniqueGPUCopyPass copypass{SDL_BeginGPUCopyPass(command_buffer.get())};
  for (const CopypassTask &task : taskQueue)
    task.visit(CopypassTaskVisitor{copypass.get()});
}

void CopypassTaskVisitor::operator()(CopyGlyphVertices task) const {
  SDL_GPUTransferBufferLocation src{
      .transfer_buffer = task.sequencer->instance_transfer.get(), .offset = 0};
  std::uint32_t bufferSize{static_cast<std::uint32_t>(
      hb_buffer_get_length(task.sequencer->harfbuzz_buffer.get()) *
      sizeof(RectVertexInstance))};
  SDL_GPUBufferRegion dst{.buffer = task.sequencer->instance_buffer.get(),
                          .offset = 0,
                          .size = bufferSize};
  SDL_UploadToGPUBuffer(copypass, &src, &dst, false);
}

void GPUCommandSequencer::issue_render_pass() {
  GPURenderSequencer render_sequencer{app, this};
  render_sequencer.swapchain_texture = waitAndAcquireGPUSwapchainTexture(
      command_buffer.get(), app->main_window.get(),
      &render_sequencer.swapchain_w, &render_sequencer.swapchain_h);
  if (not render_sequencer.swapchain_texture)
    return;
  if (app->selection_areas.empty()) {
    SDL_GPUGraphicsPipeline *scenePipeline =
        app->caption_sequencer.swapchain_pipeline.get();
    render_sequencer.draw_scene(render_sequencer.swapchain_texture,
                                scenePipeline);
  } else {
    render_sequencer.verify_storage_textures();
    SDL_GPUGraphicsPipeline *scenePipeline =
        app->caption_sequencer.offscreen_pipeline.get();
    render_sequencer.draw_scene(app->offscreen_texture.get(), scenePipeline);
    render_sequencer.selected_areas_draw();
    render_sequencer.selected_areas_blend();
    render_sequencer.draw_processed_image();
  }
}

void GPURenderSequencer::draw_processed_image() {
  SDL_GPUBlitInfo blitInfo{};
  blitInfo.load_op = SDL_GPU_LOADOP_DONT_CARE;
  blitInfo.source.texture = app->processed_texture.get();
  blitInfo.source.w = swapchain_w;
  blitInfo.source.h = swapchain_h;
  blitInfo.destination.texture = swapchain_texture;
  blitInfo.destination.w = swapchain_w;
  blitInfo.destination.h = swapchain_h;
  SDL_BlitGPUTexture(command_sequencer->command_buffer.get(), &blitInfo);
}

void GPURenderSequencer::draw_scene(SDL_GPUTexture *scene_texture,
                                    SDL_GPUGraphicsPipeline *pipeline) {
  SDL_GPUCommandBuffer *cmd = command_sequencer->command_buffer.get();
  SDL_GPUColorTargetInfo colorTarget{.texture = scene_texture,
                                     .clear_color = {0.08f, 0.08f, 0.10f, 1.0f},
                                     .load_op = SDL_GPU_LOADOP_CLEAR,
                                     .store_op = SDL_GPU_STOREOP_STORE};
  UniqueGPURenderPass renderPass = beginGPURenderPass(cmd, &colorTarget);

  SDL_GPUViewport viewport{.x = 0,
                           .y = 0,
                           .w = static_cast<float>(swapchain_w),
                           .h = static_cast<float>(swapchain_h),
                           .min_depth = 0.0f,
                           .max_depth = 1.0f};
  SDL_SetGPUViewport(renderPass.get(), &viewport);
  SDL_Rect scissor{0, 0, static_cast<int>(swapchain_w),
                   static_cast<int>(swapchain_h)};
  SDL_SetGPUScissor(renderPass.get(), &scissor);

  SDL_BindGPUGraphicsPipeline(renderPass.get(), pipeline);
  SDL_GPUBufferBinding instanceBinding{
      app->caption_sequencer.instance_buffer.get(), 0};
  SDL_BindGPUVertexBuffers(renderPass.get(), 0, &instanceBinding, 1);
  SDL_GPUTextureSamplerBinding samplerBinding{};
  samplerBinding.texture = app->caption_sequencer.atlas_texture.get();
  samplerBinding.sampler = app->caption_sequencer.sampler.get();
  SDL_BindGPUFragmentSamplers(renderPass.get(), 0, &samplerBinding, 1);

  struct alignas(16) RectUniform {
    float viewport[2];
    float translate[2];
  };
  RectUniform rectUni{};
  rectUni.viewport[0] = static_cast<float>(swapchain_w);
  rectUni.viewport[1] = static_cast<float>(swapchain_h);
  rectUni.translate[0] = static_cast<float>(app->caption_x);
  rectUni.translate[1] = static_cast<float>(app->caption_y);
  SDL_PushGPUVertexUniformData(cmd, 0, &rectUni, sizeof(RectUniform));
  struct BlitUniform {
    float tint[4];
  };
  BlitUniform blit{.tint = {1.0f, 1.0f, 1.0f, 1.0f}};
  SDL_PushGPUFragmentUniformData(cmd, 0, &blit, sizeof(BlitUniform));

  std::uint32_t numInstances =
      hb_buffer_get_length(app->caption_sequencer.harfbuzz_buffer.get());
  SDL_DrawGPUPrimitives(renderPass.get(), 4, numInstances, 0, 0);
}

void GPURenderSequencer::verify_storage_textures() {
  bool isFineW = app->offscreen_w >= swapchain_w;
  bool isFineH = app->offscreen_h >= swapchain_h;
  if (isFineW && isFineH)
    return;
  if (not isFineW)
    app->offscreen_w =
        std::max(1u << std::bit_width(swapchain_w), app->offscreen_w * 2u);
  if (not isFineH)
    app->offscreen_h =
        std::max(1u << std::bit_width(swapchain_h), app->offscreen_h * 2u);
  GPUTextureCreateInfo offscreenInfo{};
  offscreenInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  offscreenInfo.texture_w = app->offscreen_w;
  offscreenInfo.texture_h = app->offscreen_h;
  offscreenInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  app->offscreen_texture =
      createGPUTexture(app->device_ctx.get(), offscreenInfo);
  GPUTextureCreateInfo selectionInfo{};
  selectionInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  selectionInfo.texture_w = app->offscreen_w;
  selectionInfo.texture_h = app->offscreen_h;
  selectionInfo.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  app->selection_texture =
      createGPUTexture(app->device_ctx.get(), selectionInfo);
  GPUTextureCreateInfo processedInfo{};
  processedInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  processedInfo.texture_w = app->offscreen_w;
  processedInfo.texture_h = app->offscreen_h;
  processedInfo.usage =
      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
  app->processed_texture =
      createGPUTexture(app->device_ctx.get(), processedInfo);
}

void GPURenderSequencer::selected_areas_draw() {
  SDL_GPUCommandBuffer *cmd = command_sequencer->command_buffer.get();
  SDL_GPUColorTargetInfo colorTarget{.texture = app->selection_texture.get(),
                                     .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
                                     .load_op = SDL_GPU_LOADOP_CLEAR,
                                     .store_op = SDL_GPU_STOREOP_STORE};
  UniqueGPURenderPass renderPass = beginGPURenderPass(cmd, &colorTarget);

  SDL_GPUViewport viewport{.x = 0,
                           .y = 0,
                           .w = static_cast<float>(swapchain_w),
                           .h = static_cast<float>(swapchain_h),
                           .min_depth = 0.0f,
                           .max_depth = 1.0f};
  SDL_SetGPUViewport(renderPass.get(), &viewport);
  SDL_Rect scissor{0, 0, static_cast<int>(swapchain_w),
                   static_cast<int>(swapchain_h)};
  SDL_SetGPUScissor(renderPass.get(), &scissor);

  SDL_BindGPUGraphicsPipeline(renderPass.get(), app->selection_pipeline.get());
  SDL_GPUBufferBinding instanceBinding{app->selection_instances.get(), 0};
  SDL_BindGPUVertexBuffers(renderPass.get(), 0, &instanceBinding, 1);

  struct alignas(16) RectUniform {
    float viewport[2];
    float translate[2];
  };
  RectUniform rectUni{};
  rectUni.viewport[0] = static_cast<float>(swapchain_w);
  rectUni.viewport[1] = static_cast<float>(swapchain_h);
  std::array<float, 2> tx{0, static_cast<float>(swapchain_h)};
  std::memcpy(&rectUni.translate, tx.data(), sizeof(RectUniform::translate));
  SDL_PushGPUVertexUniformData(cmd, 0, &rectUni, sizeof(RectUniform));

  struct alignas(16) BoxUniform {
    float blend_color[4];
    float radius;
  };
  BoxUniform boxUniform{.blend_color = {1.0f, 1.0f, 1.0f, 1.0f},
                        .radius = 0.2f};
  SDL_PushGPUFragmentUniformData(cmd, 0, &boxUniform, sizeof(BoxUniform));

  std::uint32_t instanceCount{
      static_cast<std::uint32_t>(app->selection_areas.size())};
  SDL_DrawGPUPrimitives(renderPass.get(), 4, instanceCount, 0, 0);
}

void GPURenderSequencer::selected_areas_blend() {
  SDL_GPUCommandBuffer *cmd = command_sequencer->command_buffer.get();

  SDL_GPUStorageTextureReadWriteBinding textureRW{
      .texture = app->processed_texture.get()};
  UniqueGPUComputePass computePass = beginGPUComputePass(cmd, &textureRW);

  SDL_BindGPUComputePipeline(computePass.get(),
                             app->difference_blend_pipeline.get());
  std::array<SDL_GPUTexture *, 2> storageTextures{app->offscreen_texture.get(),
                                                  app->selection_texture.get()};
  SDL_BindGPUComputeStorageTextures(
      computePass.get(), 0, storageTextures.data(), storageTextures.size());

  std::uint32_t ceiledX{(swapchain_w + 7) / 8};
  std::uint32_t ceiledY{(swapchain_h + 7) / 8};
  SDL_DispatchGPUCompute(computePass.get(), ceiledX, ceiledY, 1);
}

SDL_AppResult AppState::iterate() noexcept {
  try {
    GPUCommandSequencer command_sequencer{this};
    command_sequencer.initiate();
    command_sequencer.issue_copy_pass();
    command_sequencer.issue_render_pass();
    return SDL_APP_CONTINUE;
  } catch (const std::runtime_error &e) {
    SDL_Log("[AppIterate] %s", e.what());
    return SDL_APP_FAILURE;
  }
}

void AppState::caption_measure(SDL_Rect &captionRect) noexcept {
  static constexpr int PADDING_X = 1;
  static constexpr int PADDING_Y = 4;
  int horzOffset = static_cast<int>(caption_sequencer.horz_min / 64);
  int vertOffset = static_cast<int>(caption_sequencer.vert_max / 64);
  captionRect.x = caption_x + horzOffset - PADDING_X;
  captionRect.y = caption_y + vertOffset - PADDING_Y;
  captionRect.w = caption_sequencer.measured_w() + PADDING_X * 2;
  captionRect.h = caption_sequencer.measured_h() + PADDING_Y * 2;
}

SDL_AppResult AppState::handle_event(SDL_Event *event) noexcept {
  if (event->type == SDL_EVENT_QUIT)
    return SDL_APP_SUCCESS;

  if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    SDL_Point clickPoint{};
    clickPoint.x = static_cast<int>(event->button.x);
    clickPoint.y = static_cast<int>(event->button.y);
    SDL_Rect captionRect{};
    caption_measure(captionRect);
    bool captionHit = SDL_PointInRect(&clickPoint, &captionRect);
    current_selection.reset();
    if (captionHit) {
      std::array<std::uint32_t, 4> clickedArea{};
      std::get<0>(clickedArea) = static_cast<std::uint32_t>(captionRect.x);
      std::get<1>(clickedArea) = static_cast<std::uint32_t>(captionRect.y);
      std::get<2>(clickedArea) = static_cast<std::uint32_t>(captionRect.w);
      std::get<3>(clickedArea) = static_cast<std::uint32_t>(captionRect.h);
      current_selection = clickedArea;
    }
    selection_areas.clear();
    selection_reflow();
    return SDL_APP_CONTINUE;
  }

  if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
    SDL_Log("x: %f, y: %f", event->button.x, event->button.y);
    return SDL_APP_CONTINUE;
  }

  if (event->type == SDL_EVENT_WINDOW_RESIZED) {
    caption_x = (event->window.data1 - caption_sequencer.measured_w()) / 2;
    caption_y = (event->window.data2 - caption_sequencer.measured_h()) / 2;
    return SDL_APP_CONTINUE;
  }

  return SDL_APP_CONTINUE;
}

void GlyphSequencer::instantiate(SDL_Window *window) {
  GPUTextureCreateInfo textureInfo{.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                                   .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                   .texture_w = ATLAS_SIDE,
                                   .texture_h = ATLAS_SIDE};
  atlas_texture = createGPUTexture(app->device_ctx.get(), textureInfo);
  atlas_transfer =
      createGPUTransferBuffer(app->device_ctx.get(), ATLAS_SIDE * ATLAS_SIDE);
  harfbuzz_buffer = UniqueHarfbuzzBuffer{hb_buffer_create()};

  sampler = createCommonGPUSampler(app->device_ctx.get());
  swapchain_pipeline = createGlyphGPUGraphicsPipeline(
      app->device_ctx.get(),
      SDL_GetGPUSwapchainTextureFormat(app->device_ctx.get(), window));
  offscreen_pipeline = createGlyphGPUGraphicsPipeline(
      app->device_ctx.get(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
}

void GlyphSequencer::glyphs_shape(std::string_view textView) {
  hb_buffer_reset(harfbuzz_buffer.get());
  hb_buffer_add_utf8(harfbuzz_buffer.get(), textView.data(),
                     static_cast<int>(textView.size()), 0,
                     static_cast<int>(textView.size()));
  hb_buffer_guess_segment_properties(harfbuzz_buffer.get());
  hb_shape(app->harfbuzz_font.get(), harfbuzz_buffer.get(), nullptr, 0);
}

void GlyphSequencer::verify_glyph_cache() {
  std::span<hb_glyph_info_t> glyphInfos{
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr),
      hb_buffer_get_length(harfbuzz_buffer.get())};
  bool everInserted{};
  for (const hb_glyph_info_t &info : glyphInfos) {
    if (glyph_cache.contains(info.codepoint))
      continue;
    UniqueFreetypeGlyph glyph = app->load_freetype_glyph(info.codepoint);
    const FT_Bitmap &bitmap = asBitmapGlyph(glyph.get())->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0)
      continue;
    glyph_cache[info.codepoint] =
        GlyphCacheItem{std::move(glyph), std::make_unique<GlyphAtlasInfo>()};
    everInserted = true;
  }
  if (everInserted) {
    pack_atlas();
    app->copypass_tasks.glyph_atlas.emplace(this);
  }
}

void GlyphSequencer::pack_atlas() {
  struct CacheEntryView {
    hb_codepoint_t codepoint;
    FT_Glyph freetype_glyph;
    GlyphAtlasInfo *atlas;
    std::uint32_t accumulated_x;
  };
  auto entryProject = [](const auto &cacheEntry) {
    const auto &[codepoint, cacheItem] = cacheEntry;
    return CacheEntryView{.codepoint = codepoint,
                          .freetype_glyph = cacheItem.freetype_glyph.get(),
                          .atlas = cacheItem.atlas.get(),
                          .accumulated_x = 0};
  };
  auto entriesView = glyph_cache | std::views::transform(entryProject);
  std::vector<CacheEntryView> entriesViewVec{std::from_range, entriesView};
  auto rowsProject = [](const CacheEntryView &entryView) {
    return asBitmapGlyph(entryView.freetype_glyph)->bitmap.rows;
  };
  std::ranges::sort(entriesViewVec, std::ranges::greater{}, rowsProject);
  for (std::uint32_t accumulatedX{};
       CacheEntryView &entryView : entriesViewVec) {
    std::uint32_t bitmapWidth =
        asBitmapGlyph(entryView.freetype_glyph)->bitmap.width;
    if (bitmapWidth > ATLAS_SIDE)
      continue;
    entryView.accumulated_x =
        accumulatedX + bitmapWidth > ATLAS_SIDE ? 0 : accumulatedX;
    accumulatedX = entryView.accumulated_x + bitmapWidth + ATLAS_PADDING;
  }
  struct CacheShelfView {
    std::vector<CacheEntryView> entries;
    std::uint32_t accumulated_y;
  };
  auto shelfPredicate = [](const CacheEntryView &viewA,
                           const CacheEntryView &viewB) {
    return viewA.accumulated_x < viewB.accumulated_x;
  };
  auto shelfProject = [](auto entriesSubrange) {
    std::vector<CacheEntryView> entries{std::from_range, entriesSubrange};
    return CacheShelfView{.entries = std::move(entries), .accumulated_y = 0};
  };
  auto shelvesView = entriesViewVec | std::views::chunk_by(shelfPredicate) |
                     std::views::transform(shelfProject);
  std::vector<CacheShelfView> shelvesViewVec{std::from_range, shelvesView};
  for (std::uint32_t accumulatedY{};
       CacheShelfView &shelfView : shelvesViewVec) {
    std::uint32_t shelfHeight =
        asBitmapGlyph(shelfView.entries.front().freetype_glyph)->bitmap.rows;
    if (shelfHeight > ATLAS_SIDE)
      continue;
    shelfView.accumulated_y =
        accumulatedY + shelfHeight > ATLAS_SIDE ? 0 : accumulatedY;
    accumulatedY = shelfView.accumulated_y + shelfHeight + ATLAS_PADDING;
  }
  auto pagePredicate = [](const CacheShelfView &viewA,
                          const CacheShelfView &viewB) {
    return viewA.accumulated_y < viewB.accumulated_y;
  };
  auto pagesView = shelvesViewVec | std::views::chunk_by(pagePredicate) |
                   std::views::enumerate;
  for (const auto &[pageIdx, shelves] : pagesView)
    for (const CacheShelfView &shelfView : shelves)
      for (const CacheEntryView &entryView : shelfView.entries) {
        entryView.atlas->page = static_cast<std::uint32_t>(pageIdx);
        entryView.atlas->y = shelfView.accumulated_y;
        entryView.atlas->x = entryView.accumulated_x;
      }
  AtlasCopier atlasCopier{};
  atlasCopier.mapped_atlas =
      mapGPUTransferBuffer(app->device_ctx.get(), atlas_transfer.get(), true);
  std::memset(atlasCopier.mapped_atlas.get(), 0, ATLAS_SIDE * ATLAS_SIDE);
  for (const auto &[codepoint, cacheItem] : glyph_cache)
    atlasCopier.copy_bitmap(cacheItem);
}

void GlyphSequencer::AtlasCopier::copy_bitmap(const GlyphCacheItem &cacheItem) {
  atlas_info = cacheItem.atlas.get();
  bitmap = &asBitmapGlyph(cacheItem.freetype_glyph.get())->bitmap;
  for (std::uint32_t row = 0; row < bitmap->rows; ++row)
    copy_bitmap_row(row);
}

void GlyphSequencer::AtlasCopier::copy_bitmap_row(std::uint32_t row) {
  unsigned char *source = bitmap->buffer + row * bitmap->pitch;
  std::uint32_t atlasVertOffset = atlas_info->y + row;
  std::uint32_t atlasFullOffset = atlasVertOffset * ATLAS_SIDE + atlas_info->x;
  unsigned char *transferStart =
      static_cast<unsigned char *>(mapped_atlas.get());
  std::memcpy(transferStart + atlasFullOffset, source, bitmap->width);
}

void GlyphSequencer::verify_instance_capacity() {
  std::uint32_t bufferSize = static_cast<std::uint32_t>(
      hb_buffer_get_length(harfbuzz_buffer.get()) * sizeof(RectVertexInstance));
  if (instance_capacity >= bufferSize)
    return;
  instance_capacity =
      std::max(1u << std::bit_width(bufferSize), instance_capacity * 2u);
  SDL_GPUBufferCreateInfo bufferInfo{.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
                                     .size = instance_capacity};
  instance_buffer = createGPUBuffer(app->device_ctx.get(), &bufferInfo);
  instance_transfer =
      createGPUTransferBuffer(app->device_ctx.get(), instance_capacity);
}

std::optional<RectVertexInstance>
GlyphSequencer::create_vertex_instance(unsigned int glyphIndex,
                                       hb_position_t penX) {
  hb_glyph_info_t *glyphInfos =
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr);
  hb_glyph_position_t *positions =
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), nullptr);
  GlyphCache::iterator cacheIt =
      glyph_cache.find(glyphInfos[glyphIndex].codepoint);
  if (cacheIt == glyph_cache.end())
    return std::nullopt;
  FT_BitmapGlyph bitmapGlyph =
      asBitmapGlyph(cacheIt->second.freetype_glyph.get());
  RectVertexInstance vertexInstance{};
  vertexInstance.rect_pos[0] =
      static_cast<float>(penX + positions[glyphIndex].x_offset +
                         bitmapGlyph->left * 64) /
      64.0f;
  vertexInstance.rect_pos[1] =
      static_cast<float>(bitmapGlyph->top -
                         static_cast<int>(bitmapGlyph->bitmap.rows)) -
      static_cast<float>(positions[glyphIndex].y_offset) / 64.0f;
  float bitmapW = static_cast<float>(bitmapGlyph->bitmap.width);
  float bitmapH = static_cast<float>(bitmapGlyph->bitmap.rows);
  vertexInstance.rect_size[0] = bitmapW;
  vertexInstance.rect_size[1] = bitmapH;
  float atlasU = static_cast<float>(cacheIt->second.atlas->x) /
                 static_cast<float>(ATLAS_SIDE);
  float atlasV = static_cast<float>(cacheIt->second.atlas->y) /
                 static_cast<float>(ATLAS_SIDE);
  vertexInstance.uv_rect[0] = atlasU;
  vertexInstance.uv_rect[1] = atlasV;
  vertexInstance.uv_rect[2] = atlasU + bitmapW / static_cast<float>(ATLAS_SIDE);
  vertexInstance.uv_rect[3] = atlasV + bitmapH / static_cast<float>(ATLAS_SIDE);
  return vertexInstance;
}

std::generator<RectVertexInstance> GlyphSequencer::emit_vertex_instances() {
  unsigned int glyphIndex{}, bufferLength{};
  hb_glyph_position_t *positions =
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), &bufferLength);
  hb_position_t penX{};
  while (glyphIndex < bufferLength) {
    std::optional<RectVertexInstance> vertexInstance{
        create_vertex_instance(glyphIndex, penX)};
    if (vertexInstance)
      co_yield vertexInstance.value();
    penX += positions[glyphIndex].x_advance;
    ++glyphIndex;
  }
}

void GlyphSequencer::transfer_vertex_instances() {
  UniqueGPUMappedBuffer mappedBuffer = mapGPUTransferBuffer(
      app->device_ctx.get(), instance_transfer.get(), true);
  for (auto [idx, data] : emit_vertex_instances() | std::views::enumerate)
    std::memcpy(static_cast<RectVertexInstance *>(mappedBuffer.get()) + idx,
                &data, sizeof(RectVertexInstance));
}

void GlyphSequencer::queue_vertex_copypass() {
  verify_instance_capacity();
  transfer_vertex_instances();
  app->copypass_tasks.glyph_vertices.emplace(this);
}

void GlyphSequencer::measure_text() {
  horz_min = std::numeric_limits<hb_position_t>::max();
  horz_max = std::numeric_limits<hb_position_t>::min();
  vert_min = std::numeric_limits<hb_position_t>::max();
  vert_max = std::numeric_limits<hb_position_t>::min();
  const hb_glyph_info_t *glyphInfos =
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr);
  const hb_glyph_position_t *positions =
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), nullptr);

  struct GlyphCarry {
    unsigned int glyph_index;
    hb_position_t pen_x;
  };
  for (GlyphCarry c{};
       c.glyph_index < hb_buffer_get_length(harfbuzz_buffer.get());
       c.pen_x += positions[c.glyph_index].x_advance, ++c.glyph_index) {
    GlyphCache::iterator cacheIt =
        glyph_cache.find(glyphInfos[c.glyph_index].codepoint);
    if (cacheIt == glyph_cache.end())
      continue;

    FT_BitmapGlyph bitmapGlyph =
        asBitmapGlyph(cacheIt->second.freetype_glyph.get());
    hb_position_t left =
        c.pen_x + positions[c.glyph_index].x_offset + bitmapGlyph->left * 64;
    hb_position_t right = left + bitmapGlyph->bitmap.width * 64;
    hb_position_t top =
        -positions[c.glyph_index].y_offset - bitmapGlyph->top * 64;
    hb_position_t bottom = top + bitmapGlyph->bitmap.rows * 64;

    horz_min = std::min(horz_min, left);
    horz_max = std::max(horz_max, right);
    vert_min = std::min(vert_min, top);
    vert_max = std::max(vert_max, bottom);
  }
}

std::generator<GlyphSequencer::ClusterElement>
GlyphSequencer::match_cluster_elements() {
  const hb_glyph_info_t *glyphInfos =
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr);
  const hb_glyph_position_t *positions =
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), nullptr);
  using BufferPair = std::pair<std::uint32_t, const hb_glyph_position_t *>;
  std::vector<BufferPair> bufferPairs{};
  for (unsigned int i{}; i < hb_buffer_get_length(harfbuzz_buffer.get()); ++i)
    bufferPairs.emplace_back(glyphInfos[i].cluster, positions + i);

  std::vector<std::span<BufferPair>> clusterSpans{};
  auto clusterPredicate = [](const BufferPair &infoA, const BufferPair &infoB) {
    return infoA.first == infoB.first;
  };
  for (const auto &clusterChunk :
       bufferPairs | std::views::chunk_by(clusterPredicate))
    clusterSpans.emplace_back(clusterChunk);
  std::vector<CodepointInfo> codepointInfos{
      std::from_range, emitCodepointInfo(app->caption_string)};

  for (const auto &[spanA, spanB] : clusterSpans | std::views::pairwise) {
    std::uint32_t clusterA = spanA.front().first;
    std::uint32_t clusterB = spanB.front().first;
    auto codepointA =
        std::ranges::lower_bound(codepointInfos, clusterA, std::ranges::less{},
                                 &CodepointInfo::byte_offset);
    auto codepointB =
        std::ranges::lower_bound(codepointInfos, clusterB, std::ranges::less{},
                                 &CodepointInfo::byte_offset);
    std::ranges::subrange codepointsA{codepointA, codepointB};
    auto codepointProject = [](const CodepointInfo &codepointInfo) {
      return std::make_optional(codepointInfo);
    };
    std::ranges::concat_view codepointOptionals{
        codepointsA | std::views::transform(codepointProject),
        std::ranges::repeat_view{std::nullopt}};
    auto clusterProject = [](const BufferPair &bufferPair) {
      return bufferPair.second;
    };
    std::ranges::concat_view clusterPositions{
        spanA | std::views::transform(clusterProject),
        std::ranges::repeat_view{nullptr}};
    std::size_t elementCount = std::max(spanA.size(), codepointsA.size());
    std::ranges::zip_view elementTuples{std::ranges::repeat_view{clusterA},
                                        codepointOptionals, clusterPositions};
    auto elementProject = [](const auto &elementTuple) {
      return std::make_from_tuple<ClusterElement>(elementTuple);
    };
    auto elementBatch = elementTuples | std::views::transform(elementProject) |
                        std::views::take(elementCount);
    co_yield std::ranges::elements_of(elementBatch);
  }
}

void CopypassTaskVisitor::operator()(CopyGlyphAtlas task) const {
  SDL_GPUTextureTransferInfo source{
      .transfer_buffer = task.sequencer->atlas_transfer.get(),
      .offset = 0,
      .pixels_per_row = GlyphSequencer::ATLAS_SIDE,
      .rows_per_layer = GlyphSequencer::ATLAS_SIDE};
  SDL_GPUTextureRegion destination{};
  destination.texture = task.sequencer->atlas_texture.get();
  destination.w = GlyphSequencer::ATLAS_SIDE;
  destination.h = GlyphSequencer::ATLAS_SIDE;
  destination.d = 1;
  SDL_UploadToGPUTexture(copypass, &source, &destination, false);
}

UniqueFreetypeGlyph AppState::load_freetype_glyph(hb_codepoint_t codepoint) {
  FT_Error errcode{};
  FT_Face face = hb_ft_font_get_ft_face(harfbuzz_font.get());
  errcode = FT_Load_Glyph(
      face, codepoint, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER);
  if (errcode) {
    std::string errorMsg{"FT_Load_Glyph failed for glyph: "};
    throw std::runtime_error{errorMsg + std::to_string(codepoint)};
  }
  FT_Glyph glyph{};
  errcode = FT_Get_Glyph(face->glyph, &glyph);
  if (errcode) {
    std::string errorMsg{"FT_Get_Glyph failed for glyph: "};
    throw std::runtime_error{errorMsg + std::to_string(codepoint)};
  }
  return UniqueFreetypeGlyph{glyph};
}

void AppState::caption_reflow() {
  caption_sequencer.glyphs_shape(caption_string);
  caption_sequencer.verify_glyph_cache();
  caption_sequencer.measure_text();
  caption_sequencer.cluster_elements.clear();
  caption_sequencer.cluster_elements.append_range(
      caption_sequencer.match_cluster_elements());
  caption_x = (INITIAL_WINDOW_W - caption_sequencer.measured_w()) / 2;
  caption_y = (INITIAL_WINDOW_H - caption_sequencer.measured_h()) / 2;
  caption_sequencer.queue_vertex_copypass();
}

void CopypassTaskVisitor::operator()(CopySelectionAreas task) const {
  SDL_GPUTransferBufferLocation instancesSource{
      .transfer_buffer = task.app->selection_transfer.get(), .offset = 0};
  SDL_GPUBufferRegion instancesDest{};
  instancesDest.buffer = task.app->selection_instances.get();
  instancesDest.size = static_cast<std::uint32_t>(
      task.app->selection_areas.size() * sizeof(RectVertexInstance));
  SDL_UploadToGPUBuffer(copypass, &instancesSource, &instancesDest, false);
}

void AppState::selection_reflow() {
  if (selection_areas.empty())
    return;
  verify_selection_capacity();
  transfer_selection_areas();
  copypass_tasks.selection_areas.emplace(this);
}

void AppState::verify_selection_capacity() {
  std::uint32_t instancesSize{static_cast<std::uint32_t>(
      selection_areas.size() * sizeof(RectVertexInstance))};
  if (instancesSize <= selection_instances_capacity)
    return;
  selection_instances_capacity = std::max(1u << std::bit_width(instancesSize),
                                          selection_instances_capacity * 2u);
  SDL_GPUBufferCreateInfo instancesInfo{.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
                                        .size = selection_instances_capacity};
  selection_instances = createGPUBuffer(device_ctx.get(), &instancesInfo);
  selection_transfer =
      createGPUTransferBuffer(device_ctx.get(), selection_instances_capacity);
}

void AppState::transfer_selection_areas() {
  UniqueGPUMappedBuffer selectionMapped =
      mapGPUTransferBuffer(device_ctx.get(), selection_transfer.get(), true);
  for (auto [idx, rect] : selection_areas | std::views::enumerate) {
    RectVertexInstance rectInstance{};
    rectInstance.rect_pos[0] = static_cast<float>(std::get<0>(rect));
    rectInstance.rect_pos[1] =
        -1.0f * static_cast<float>(std::get<1>(rect) + std::get<3>(rect));
    rectInstance.rect_size[0] = static_cast<float>(std::get<2>(rect));
    rectInstance.rect_size[1] = static_cast<float>(std::get<3>(rect));
    rectInstance.uv_rect[2] = 1.0f;
    rectInstance.uv_rect[3] = 1.0f;
    std::memcpy(static_cast<RectVertexInstance *>(selectionMapped.get()) + idx,
                &rectInstance, sizeof(RectVertexInstance));
  }
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  if (not SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init_Subsystem failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  if (FT_Init_FreeType(&globalFTLibrary)) {
    SDL_Log("FT_Init_FreeType failed!");
    return SDL_APP_FAILURE;
  }
  try {
    AppState *app = new AppState{};
    *appstate = app;
    app->instantiate(globalFTLibrary);
    app->caption_reflow();
    app->selection_reflow();
    return SDL_APP_CONTINUE;
  } catch (const std::runtime_error &e) {
    SDL_Log("[AppInit] %s", e.what());
    return SDL_APP_FAILURE;
  }
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = static_cast<AppState *>(appstate);
  delete app;
  if (globalFTLibrary) {
    FT_Done_FreeType(globalFTLibrary);
    globalFTLibrary = nullptr;
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  AppState *app = static_cast<AppState *>(appstate);
  return app->iterate();
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  AppState *app = static_cast<AppState *>(appstate);
  return app->handle_event(event);
}
