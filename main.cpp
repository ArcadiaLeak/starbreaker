#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <generator>
#include <map>
#include <memory>
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
#include <vulkan/vulkan.h>

struct GlyphVertexInstance {
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
    throw std::runtime_error{"FT_Set_Pixel_Sizes failed!"};
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

constexpr int INITIAL_WINDOW_WIDTH = 800;
constexpr int INITIAL_WINDOW_HEIGHT = 600;

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

UniqueGPUSampler createGlyphGPUSampler(SDL_GPUDevice *device) {
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
  GPUShaderCreateInfo vertexInfo{.filepath = "rectangle.spv",
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
  std::get<0>(vertexAttributes).offset =
      offsetof(GlyphVertexInstance, rect_pos);
  std::get<1>(vertexAttributes).location = 1;
  std::get<1>(vertexAttributes).buffer_slot = 0;
  std::get<1>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<1>(vertexAttributes).offset =
      offsetof(GlyphVertexInstance, rect_size);
  std::get<2>(vertexAttributes).location = 2;
  std::get<2>(vertexAttributes).buffer_slot = 0;
  std::get<2>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  std::get<2>(vertexAttributes).offset = offsetof(GlyphVertexInstance, uv_rect);

  SDL_GPUVertexBufferDescription vertexBufferDescriptions{};
  vertexBufferDescriptions.slot = 0;
  vertexBufferDescriptions.pitch = sizeof(GlyphVertexInstance);
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
  pipelineInfo.num_readonly_storage_textures = 1;
  pipelineInfo.num_readonly_storage_buffers = 1;
  pipelineInfo.num_readwrite_storage_textures = 1;
  pipelineInfo.num_uniform_buffers = 1;
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

class AppState {
private:
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
    void blend_selected_areas();
  };

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

    static constexpr std::uint32_t ATLAS_SIDE = 1024;
    static constexpr std::uint32_t ATLAS_PADDING = 1;
    UniqueGPUTransferBuffer atlas_transfer;
    UniqueGPUTexture atlas_texture;
    int measured_width = 0, measured_height = 0;

    UniqueGPUBuffer instance_buffer;
    UniqueGPUTransferBuffer instance_transfer;
    std::uint32_t instance_capacity = 0;

    std::optional<GlyphVertexInstance>
    create_vertex_instance(unsigned int glyphIndex, hb_position_t penX);
    std::generator<GlyphVertexInstance> generate_vertex_instances();

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
  };

  UniqueHarfbuzzFont harfbuzz_font;

  UniqueGPUDevice device_ctx;
  UniqueWindow main_window;
  UniqueWindowForGPUDevice main_window_for_gpu_device;

  std::string caption_string = "Hello world kerning Text";
  int caption_x = 0, caption_y = 0;
  GlyphSequencer caption_sequencer{this};

  UniqueFreetypeGlyph load_freetype_glyph(hb_codepoint_t codepoint);

  UniqueGPUTexture offscreen_texture;
  UniqueGPUTexture processed_texture;
  std::uint32_t offscreen_w = 0, offscreen_h = 0;

  UniqueGPUComputePipeline selection_pipeline;
  UniqueGPUBuffer selection_buffer;
  UniqueGPUTransferBuffer selection_transfer;
  std::uint32_t selection_capacity = 0;
  std::vector<std::array<std::uint32_t, 4>> selection_areas;

  void verify_selection_capacity();
  void transfer_selection_areas();

  struct CopypassTask {
    CopypassTask(const CopypassTask &) = delete;
    CopypassTask &operator=(const CopypassTask &) = delete;
    CopypassTask(CopypassTask &&) = delete;
    CopypassTask &operator=(CopypassTask &&) = delete;

    CopypassTask() = default;
    virtual ~CopypassTask() = default;
    virtual void execute(SDL_GPUCopyPass *copypass) const = 0;
  };
  std::vector<std::unique_ptr<CopypassTask>> copypass_tasks;

  struct GlyphAtlasCopypassTask : CopypassTask {
    GlyphSequencer *sequencer;
    void execute(SDL_GPUCopyPass *copypass) const;
  };

  struct GlyphVertexCopypassTask : CopypassTask {
    GlyphSequencer *sequencer;
    void execute(SDL_GPUCopyPass *copypass) const;
  };

  struct SelectionCopypassTask : CopypassTask {
    AppState *app;
    void execute(SDL_GPUCopyPass *copypass) const;
  };

public:
  AppState(FT_Library library);

  SDL_AppResult iterate() noexcept;
  SDL_AppResult handle_event(SDL_Event *event) noexcept;

  void caption_calculate();
  void selection_calculate();
};

AppState::AppState(FT_Library library) {
  device_ctx = createGPUDevice();
  main_window =
      createWindow("Hello GPU", INITIAL_WINDOW_WIDTH, INITIAL_WINDOW_HEIGHT);
  setWindowMinimumSize(main_window.get(), INITIAL_WINDOW_WIDTH,
                       INITIAL_WINDOW_HEIGHT);
  main_window_for_gpu_device =
      claimWindowForGPUDevice(device_ctx.get(), main_window.get());
  harfbuzz_font = createHarfbuzzFont(library, "assets/DejaVuSans.ttf", 10 * 64,
                                     getWindowDisplayScale(main_window.get()));
  selection_pipeline = createDifferenceBlendGPUPipeline(device_ctx.get());
  caption_sequencer.instantiate(main_window.get());
}

void AppState::GPUCommandSequencer::initiate() {
  command_buffer = acquireGPUCommandBuffer(app->device_ctx.get());
}

void AppState::GPUCommandSequencer::issue_copy_pass() {
  if (app->copypass_tasks.empty())
    return;
  UniqueGPUCopyPass copypass{SDL_BeginGPUCopyPass(command_buffer.get())};
  for (const std::unique_ptr<CopypassTask> &task : app->copypass_tasks)
    task->execute(copypass.get());
  app->copypass_tasks.clear();
}

void AppState::GlyphVertexCopypassTask::execute(
    SDL_GPUCopyPass *copypass) const {
  SDL_GPUTransferBufferLocation src{
      .transfer_buffer = sequencer->instance_transfer.get(), .offset = 0};
  std::uint32_t bufferSize{static_cast<std::uint32_t>(
      hb_buffer_get_length(sequencer->harfbuzz_buffer.get()) *
      sizeof(GlyphVertexInstance))};
  SDL_GPUBufferRegion dst{.buffer = sequencer->instance_buffer.get(),
                          .offset = 0,
                          .size = bufferSize};
  SDL_UploadToGPUBuffer(copypass, &src, &dst, false);
}

void AppState::GPUCommandSequencer::issue_render_pass() {
  GPURenderSequencer render_sequencer{app, this};
  render_sequencer.swapchain_texture = waitAndAcquireGPUSwapchainTexture(
      command_buffer.get(), app->main_window.get(),
      &render_sequencer.swapchain_w, &render_sequencer.swapchain_h);
  if (not render_sequencer.swapchain_texture)
    return;
  render_sequencer.draw_scene(render_sequencer.swapchain_texture,
                              app->caption_sequencer.swapchain_pipeline.get());
  render_sequencer.verify_storage_textures();
  render_sequencer.draw_scene(app->offscreen_texture.get(),
                              app->caption_sequencer.offscreen_pipeline.get());
  render_sequencer.blend_selected_areas();
}

void AppState::GPURenderSequencer::draw_scene(
    SDL_GPUTexture *scene_texture, SDL_GPUGraphicsPipeline *pipeline) {
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

  struct RectUniform {
    float viewport[2];
    float translate[2];
  };
  RectUniform uniforms{};
  uniforms.viewport[0] = static_cast<float>(swapchain_w);
  uniforms.viewport[1] = static_cast<float>(swapchain_h);
  uniforms.translate[0] = static_cast<float>(app->caption_x);
  uniforms.translate[1] = static_cast<float>(app->caption_y);
  SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(RectUniform));
  struct BlitUniform {
    float tint[4];
  };
  BlitUniform blit{.tint = {1.0f, 1.0f, 1.0f, 1.0f}};
  SDL_PushGPUFragmentUniformData(cmd, 0, &blit, sizeof(BlitUniform));

  std::uint32_t numInstances =
      hb_buffer_get_length(app->caption_sequencer.harfbuzz_buffer.get());
  SDL_DrawGPUPrimitives(renderPass.get(), 4, numInstances, 0, 0);
}

void AppState::GPURenderSequencer::verify_storage_textures() {
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
  GPUTextureCreateInfo processedInfo{};
  processedInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  processedInfo.texture_w = app->offscreen_w;
  processedInfo.texture_h = app->offscreen_h;
  processedInfo.usage =
      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
  app->processed_texture =
      createGPUTexture(app->device_ctx.get(), processedInfo);
}

void AppState::GPURenderSequencer::blend_selected_areas() {
  SDL_GPUCommandBuffer *cmd = command_sequencer->command_buffer.get();

  SDL_GPUStorageTextureReadWriteBinding textureRW{
      .texture = app->processed_texture.get()};
  UniqueGPUComputePass computePass = beginGPUComputePass(cmd, &textureRW);

  SDL_BindGPUComputePipeline(computePass.get(), app->selection_pipeline.get());
  SDL_GPUBuffer *areaBuf = app->selection_buffer.get();
  SDL_BindGPUComputeStorageBuffers(computePass.get(), 0, &areaBuf, 1);
  SDL_GPUTexture *baseTexture = app->offscreen_texture.get();
  SDL_BindGPUComputeStorageTextures(computePass.get(), 0, &baseTexture, 1);

  struct DifferenceBlendUniform {
    uint length;
  };
  DifferenceBlendUniform diffUni{.length = 1};
  SDL_PushGPUComputeUniformData(cmd, 0, &diffUni,
                                sizeof(DifferenceBlendUniform));

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

SDL_AppResult AppState::handle_event(SDL_Event *event) noexcept {
  if (event->type == SDL_EVENT_QUIT)
    return SDL_APP_SUCCESS;
  else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
           event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    SDL_Log("x: %f, y: %f", event->button.x, event->button.y);
    return SDL_APP_CONTINUE;
  } else if (event->type == SDL_EVENT_WINDOW_RESIZED) {
    caption_x = (event->window.data1 - caption_sequencer.measured_width) / 2;
    caption_y = (event->window.data2 - caption_sequencer.measured_height) / 2;
    return SDL_APP_CONTINUE;
  } else
    return SDL_APP_CONTINUE;
}

void AppState::GlyphSequencer::instantiate(SDL_Window *window) {
  GPUTextureCreateInfo textureInfo{.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                                   .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                   .texture_w = ATLAS_SIDE,
                                   .texture_h = ATLAS_SIDE};
  atlas_texture = createGPUTexture(app->device_ctx.get(), textureInfo);
  atlas_transfer =
      createGPUTransferBuffer(app->device_ctx.get(), ATLAS_SIDE * ATLAS_SIDE);
  harfbuzz_buffer = UniqueHarfbuzzBuffer{hb_buffer_create()};

  sampler = createGlyphGPUSampler(app->device_ctx.get());
  swapchain_pipeline = createGlyphGPUGraphicsPipeline(
      app->device_ctx.get(),
      SDL_GetGPUSwapchainTextureFormat(app->device_ctx.get(), window));
  offscreen_pipeline = createGlyphGPUGraphicsPipeline(
      app->device_ctx.get(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
}

void AppState::GlyphSequencer::glyphs_shape(std::string_view textView) {
  hb_buffer_reset(harfbuzz_buffer.get());
  hb_buffer_add_utf8(harfbuzz_buffer.get(), textView.data(),
                     static_cast<int>(textView.size()), 0,
                     static_cast<int>(textView.size()));
  hb_buffer_guess_segment_properties(harfbuzz_buffer.get());
  hb_shape(app->harfbuzz_font.get(), harfbuzz_buffer.get(), nullptr, 0);
}

void AppState::GlyphSequencer::verify_glyph_cache() {
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
    std::unique_ptr atlasCopypass = std::make_unique<GlyphAtlasCopypassTask>();
    atlasCopypass->sequencer = this;
    app->copypass_tasks.push_back(std::move(atlasCopypass));
  }
}

void AppState::GlyphSequencer::pack_atlas() {
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
  UniqueGPUMappedBuffer atlasTransfer{
      mapGPUTransferBuffer(app->device_ctx.get(), atlas_transfer.get(), true)};
  std::memset(atlasTransfer.get(), 0, ATLAS_SIDE * ATLAS_SIDE);
  auto copyBitmapRow =
      [&atlasTransfer](std::uint32_t row,
                       const GlyphCacheItem &cacheItem) -> void {
    const FT_Bitmap &bitmap =
        asBitmapGlyph(cacheItem.freetype_glyph.get())->bitmap;
    unsigned char *source = bitmap.buffer + bitmap.rows * bitmap.pitch -
                            row * bitmap.pitch - bitmap.pitch;
    std::uint32_t atlasVertOffset = cacheItem.atlas->y + row;
    std::uint32_t atlasFullOffset =
        atlasVertOffset * ATLAS_SIDE + cacheItem.atlas->x;
    unsigned char *transferStart =
        static_cast<unsigned char *>(atlasTransfer.get());
    std::memcpy(transferStart + atlasFullOffset, source, bitmap.width);
  };
  for (const auto &[codepoint, cacheItem] : glyph_cache) {
    const FT_Bitmap &bitmap =
        asBitmapGlyph(cacheItem.freetype_glyph.get())->bitmap;
    for (std::uint32_t row = 0; row < bitmap.rows; ++row)
      copyBitmapRow(row, cacheItem);
  }
}

void AppState::GlyphSequencer::verify_instance_capacity() {
  std::uint32_t bufferSize =
      static_cast<std::uint32_t>(hb_buffer_get_length(harfbuzz_buffer.get()) *
                                 sizeof(GlyphVertexInstance));
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

std::optional<GlyphVertexInstance>
AppState::GlyphSequencer::create_vertex_instance(unsigned int glyphIndex,
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
  GlyphVertexInstance vertexInstance{};
  vertexInstance.rect_pos[0] =
      static_cast<float>(penX + positions[glyphIndex].x_offset +
                         bitmapGlyph->left * 64) /
      64.0f;
  vertexInstance.rect_pos[1] =
      -1.0f * static_cast<float>(positions[glyphIndex].y_offset) / 64.0f +
      static_cast<float>(bitmapGlyph->top -
                         static_cast<int>(bitmapGlyph->bitmap.rows));
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

std::generator<GlyphVertexInstance>
AppState::GlyphSequencer::generate_vertex_instances() {
  unsigned int glyphIndex{}, bufferLength{};
  hb_glyph_position_t *positions =
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), &bufferLength);
  hb_position_t penX{};
  while (glyphIndex < bufferLength) {
    std::optional<GlyphVertexInstance> vertexInstance{
        create_vertex_instance(glyphIndex, penX)};
    if (vertexInstance)
      co_yield vertexInstance.value();
    penX += positions[glyphIndex].x_advance;
    ++glyphIndex;
  }
}

void AppState::GlyphSequencer::transfer_vertex_instances() {
  UniqueGPUMappedBuffer mappedBuffer = mapGPUTransferBuffer(
      app->device_ctx.get(), instance_transfer.get(), true);
  for (auto [idx, data] : generate_vertex_instances() | std::views::enumerate)
    std::memcpy(static_cast<GlyphVertexInstance *>(mappedBuffer.get()) + idx,
                &data, sizeof(GlyphVertexInstance));
}

void AppState::GlyphSequencer::queue_vertex_copypass() {
  verify_instance_capacity();
  transfer_vertex_instances();
  std::unique_ptr instanceCopypass =
      std::make_unique<GlyphVertexCopypassTask>();
  instanceCopypass->sequencer = this;
  app->copypass_tasks.push_back(std::move(instanceCopypass));
}

void AppState::GlyphSequencer::measure_text() {
  hb_position_t minX = std::numeric_limits<hb_position_t>::max(),
                maxX = std::numeric_limits<hb_position_t>::min();
  hb_position_t minY = std::numeric_limits<hb_position_t>::max(),
                maxY = std::numeric_limits<hb_position_t>::min();
  hb_glyph_info_t *infos =
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr);
  hb_glyph_position_t *positions =
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), nullptr);

  struct GlyphCarry {
    unsigned int glyph_index;
    hb_position_t pen_x;
  };
  for (GlyphCarry c{};
       c.glyph_index < hb_buffer_get_length(harfbuzz_buffer.get());
       c.pen_x += positions[c.glyph_index].x_advance, ++c.glyph_index) {
    GlyphCache::iterator cacheIt =
        glyph_cache.find(infos[c.glyph_index].codepoint);
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

    minX = std::min(minX, left);
    maxX = std::max(maxX, right);
    minY = std::min(minY, top);
    maxY = std::max(maxY, bottom);
  }

  measured_width = static_cast<std::int32_t>(
      std::ceil(static_cast<float>(maxX - minX) / 64.0f));
  measured_height = static_cast<std::int32_t>(
      std::ceil(static_cast<float>(maxY - minY) / 64.0f));
}

void AppState::GlyphAtlasCopypassTask::execute(
    SDL_GPUCopyPass *copypass) const {
  SDL_GPUTextureTransferInfo source{
      .transfer_buffer = sequencer->atlas_transfer.get(),
      .offset = 0,
      .pixels_per_row = GlyphSequencer::ATLAS_SIDE,
      .rows_per_layer = GlyphSequencer::ATLAS_SIDE};
  SDL_GPUTextureRegion destination{.texture = sequencer->atlas_texture.get(),
                                   .w = GlyphSequencer::ATLAS_SIDE,
                                   .h = GlyphSequencer::ATLAS_SIDE,
                                   .d = 1};
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

void AppState::caption_calculate() {
  caption_sequencer.glyphs_shape(caption_string);
  caption_sequencer.verify_glyph_cache();
  caption_sequencer.measure_text();
  caption_x = (INITIAL_WINDOW_WIDTH - caption_sequencer.measured_width) / 2;
  caption_y = (INITIAL_WINDOW_HEIGHT - caption_sequencer.measured_height) / 2;
  caption_sequencer.queue_vertex_copypass();
}

void AppState::SelectionCopypassTask::execute(SDL_GPUCopyPass *copypass) const {
  SDL_GPUTransferBufferLocation source{
      .transfer_buffer = app->selection_transfer.get(), .offset = 0};
  std::uint32_t bufferSize{static_cast<std::uint32_t>(
      app->selection_areas.size() * sizeof(DifferenceBlendArea))};
  SDL_GPUBufferRegion destination{
      .buffer = app->selection_buffer.get(), .offset = 0, .size = bufferSize};
  SDL_UploadToGPUBuffer(copypass, &source, &destination, false);
}

void AppState::selection_calculate() {
  selection_areas = {{200, 100, 300, 300}};
  verify_selection_capacity();
  transfer_selection_areas();
  std::unique_ptr selectionCopypass = std::make_unique<SelectionCopypassTask>();
  selectionCopypass->app = this;
  copypass_tasks.push_back(std::move(selectionCopypass));
}

void AppState::verify_selection_capacity() {
  std::uint32_t bufferSize{static_cast<std::uint32_t>(
      selection_areas.size() * sizeof(DifferenceBlendArea))};
  if (bufferSize <= selection_capacity)
    return;
  selection_capacity =
      std::max(1u << std::bit_width(bufferSize), selection_capacity * 2u);
  SDL_GPUBufferCreateInfo bufferInfo{
      .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
      .size = selection_capacity};
  selection_buffer = createGPUBuffer(device_ctx.get(), &bufferInfo);
  selection_transfer =
      createGPUTransferBuffer(device_ctx.get(), selection_capacity);
}

void AppState::transfer_selection_areas() {
  UniqueGPUMappedBuffer mappedBuffer =
      mapGPUTransferBuffer(device_ctx.get(), selection_transfer.get(), true);
  for (auto [idx, rect] : selection_areas | std::views::enumerate) {
    DifferenceBlendArea area{.rect = {std::get<0>(rect), std::get<1>(rect),
                                      std::get<2>(rect), std::get<3>(rect)},
                             .color = {1.0f, 1.0f, 1.0f, 1.0f}};
    std::memcpy(static_cast<DifferenceBlendArea *>(mappedBuffer.get()) + idx,
                &area, sizeof(DifferenceBlendArea));
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
    AppState *app = new AppState{globalFTLibrary};
    *appstate = app;
    app->caption_calculate();
    app->selection_calculate();
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
