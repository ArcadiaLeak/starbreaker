#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <flat_map>
#include <format>
#include <generator>
#include <inplace_vector>
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
#include <unicode/utext.h>
#include <vulkan/vulkan.h>

struct RectInstanceData
{
  float rect_position[2];
  float rect_size[2];
  float uv_rect[4];
};

struct DifferenceBlendArea
{
  std::uint32_t rect[4];
  float color[4];
};

static FT_Library globalFTLibrary = nullptr;

struct FreetypeFaceDeleter
{
  void operator()(FT_Face freetypeFace) const { FT_Done_Face(freetypeFace); }
};
using UniqueFreetypeFace = std::unique_ptr<FT_FaceRec, FreetypeFaceDeleter>;

struct FreetypeGlyphDeleter
{
  void operator()(FT_Glyph freetypeGlyph) const;
};
using UniqueFreetypeGlyph = std::unique_ptr<FT_GlyphRec, FreetypeGlyphDeleter>;
void FreetypeGlyphDeleter::operator()(FT_Glyph freetypeGlyph) const
{
  FT_Done_Glyph(freetypeGlyph);
}

FT_BitmapGlyph asBitmapGlyph(FT_Glyph freetypeGlyph)
{
  if (freetypeGlyph->format != FT_GLYPH_FORMAT_BITMAP)
    throw std::runtime_error{"Unexpected FT_Glyph format!"};
  return reinterpret_cast<FT_BitmapGlyph>(freetypeGlyph);
}

struct HarfbuzzFontDeleter
{
  void operator()(hb_font_t *harfbuzzFont) const;
};
using UniqueHarfbuzzFont = std::unique_ptr<hb_font_t, HarfbuzzFontDeleter>;
void HarfbuzzFontDeleter::operator()(hb_font_t *harfbuzzFont) const
{
  hb_font_destroy(harfbuzzFont);
}

struct HarfbuzzBufferDeleter
{
  void operator()(hb_buffer_t *harfbuzzBuffer) const;
};
using UniqueHarfbuzzBuffer =
    std::unique_ptr<hb_buffer_t, HarfbuzzBufferDeleter>;
void HarfbuzzBufferDeleter::operator()(hb_buffer_t *harfbuzzBuffer) const
{
  hb_buffer_destroy(harfbuzzBuffer);
}

UniqueHarfbuzzFont createHarfbuzzFont(FT_Library library, const char *filepath,
                                      FT_F26Dot6 fontsize, float displayScale)
{
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

struct GPUDeviceDeleter
{
  void operator()(SDL_GPUDevice *device) const;
};
using UniqueGPUDevice = std::unique_ptr<SDL_GPUDevice, GPUDeviceDeleter>;
void GPUDeviceDeleter::operator()(SDL_GPUDevice *device) const
{
  if (not SDL_WaitForGPUIdle(device))
    SDL_Log("SDL_WaitForGPUIdle failed: %s", SDL_GetError());
  SDL_DestroyGPUDevice(device);
}

UniqueGPUDevice createGPUDevice()
{
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

constexpr double INITIAL_WINDOW_W = 800.0;
constexpr double INITIAL_WINDOW_H = 600.0;

struct WindowDeleter
{
  void operator()(SDL_Window *window) const;
};
using UniqueWindow = std::unique_ptr<SDL_Window, WindowDeleter>;
void WindowDeleter::operator()(SDL_Window *window) const
{
  SDL_DestroyWindow(window);
}

UniqueWindow createWindow(const char *title, int width, int height)
{
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  SDL_Window *window = SDL_CreateWindow(title, width, height, flags);
  if (not window)
  {
    std::string errorMsg{"SDL_CreateWindow failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueWindow{window};
}

struct WindowForGPUDeviceDeleter
{
  SDL_GPUDevice *device;
  void operator()(SDL_Window *window) const;
};
using UniqueWindowForGPUDevice =
    std::unique_ptr<SDL_Window, WindowForGPUDeviceDeleter>;
void WindowForGPUDeviceDeleter::operator()(SDL_Window *window) const
{
  SDL_ReleaseWindowFromGPUDevice(device, window);
}

UniqueWindowForGPUDevice claimWindowForGPUDevice(SDL_GPUDevice *device,
                                                 SDL_Window *window)
{
  if (not SDL_ClaimWindowForGPUDevice(device, window))
  {
    std::string errorMsg{"SDL_ClaimWindowForGPUDevice failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueWindowForGPUDevice{window, WindowForGPUDeviceDeleter{device}};
}

float getWindowDisplayScale(SDL_Window *window)
{
  float scale = SDL_GetWindowDisplayScale(window);
  if (scale == 0.0f)
  {
    std::string errorMsg{"SDL_GetWindowDisplayScale failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return scale;
}

void setWindowMinimumSize(SDL_Window *window, int minW, int minH)
{
  if (not SDL_SetWindowMinimumSize(window, minW, minH))
  {
    std::string errorMsg{"SDL_SetWindowMinimumSize failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
}

std::array<int, 2> getWindowSizeInPixels(SDL_Window *window)
{
  int windowW{}, windowH{};
  if (not SDL_GetWindowSizeInPixels(window, &windowW, &windowH))
  {
    std::string errorMsg{"SDL_GetWindowSizeInPixels failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return {windowW, windowH};
}

struct FileContentDeleter
{
  void operator()(void *fileContent) const { SDL_free(fileContent); }
};
using UniqueFileContent = std::unique_ptr<void, FileContentDeleter>;

UniqueFileContent loadFile(const char *filepath, size_t *datasize)
{
  void *fileContent = SDL_LoadFile(filepath, datasize);
  if (fileContent)
    return UniqueFileContent{fileContent};
  std::string errorMsg =
      std::format("Failed to load file: {}. {}", filepath, SDL_GetError());
  throw std::runtime_error{std::move(errorMsg)};
}

struct GPUShaderDeleter
{
  SDL_GPUDevice *device;
  void operator()(SDL_GPUShader *shader) const;
};
using UniqueGPUShader = std::unique_ptr<SDL_GPUShader, GPUShaderDeleter>;
void GPUShaderDeleter::operator()(SDL_GPUShader *shader) const
{
  SDL_ReleaseGPUShader(device, shader);
}

const std::array vertexRectInstance = std::to_array<std::uint8_t>({
#embed "rect_instance.spv"
});
const std::array fragmentSelection = std::to_array<std::uint8_t>({
#embed "selection_box.spv"
});
const std::array fragmentTint = std::to_array<std::uint8_t>({
#embed "coverage_tint.spv"
});
const std::array computeDifferenceBlend = std::to_array<std::uint8_t>({
#embed "difference_blend.spv"
});

UniqueGPUShader createGPUShader(SDL_GPUDevice *device,
                                SDL_GPUShaderCreateInfo createInfo)
{
  SDL_GPUShader *shader = SDL_CreateGPUShader(device, &createInfo);
  if (not shader)
  {
    std::string errorMsg{"SDL_CreateGPUShader failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUShader{shader, GPUShaderDeleter{device}};
}

struct GPUSamplerDeleter
{
  SDL_GPUDevice *device;
  void operator()(SDL_GPUSampler *sampler) const;
};
using UniqueGPUSampler = std::unique_ptr<SDL_GPUSampler, GPUSamplerDeleter>;
void GPUSamplerDeleter::operator()(SDL_GPUSampler *sampler) const
{
  SDL_ReleaseGPUSampler(device, sampler);
}

UniqueGPUSampler createCommonGPUSampler(SDL_GPUDevice *device)
{
  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

  SDL_GPUSampler *sampler = SDL_CreateGPUSampler(device, &samplerInfo);
  if (not sampler)
  {
    std::string errorMsg{"SDL_CreateGPUSampler failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUSampler{sampler, GPUSamplerDeleter{device}};
}

struct GPUTextureDeleter
{
  SDL_GPUDevice *device = nullptr;
  void operator()(SDL_GPUTexture *texture) const;
};
using UniqueGPUTexture = std::unique_ptr<SDL_GPUTexture, GPUTextureDeleter>;
void GPUTextureDeleter::operator()(SDL_GPUTexture *texture) const
{
  SDL_ReleaseGPUTexture(device, texture);
}

struct GPUTextureCreateInfo
{
  SDL_GPUTextureFormat format;
  SDL_GPUTextureUsageFlags usage;
  std::uint32_t texture_w;
  std::uint32_t texture_h;
};
UniqueGPUTexture createGPUTexture(SDL_GPUDevice *device,
                                  GPUTextureCreateInfo createInfo)
{
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
  if (not texture)
  {
    std::string errorMsg{"SDL_CreateGPUTexture failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return {texture, GPUTextureDeleter{device}};
}

struct GPUTransferBufferDeleter
{
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUTransferBuffer *transferBuffer) const;
};
using UniqueGPUTransferBuffer =
    std::unique_ptr<SDL_GPUTransferBuffer, GPUTransferBufferDeleter>;
void GPUTransferBufferDeleter::operator()(
    SDL_GPUTransferBuffer *transferBuffer) const
{
  SDL_ReleaseGPUTransferBuffer(device_ctx, transferBuffer);
}

struct GPUTransferBufferUnmapper
{
  SDL_GPUDevice *device_ctx;
  SDL_GPUTransferBuffer *transfer_buffer;
  void operator()(void *mappedBuffer) const;
};
using UniqueGPUMappedBuffer = std::unique_ptr<void, GPUTransferBufferUnmapper>;
void GPUTransferBufferUnmapper::operator()(void *mappedBuffer) const
{
  SDL_UnmapGPUTransferBuffer(device_ctx, transfer_buffer);
}

UniqueGPUTransferBuffer createGPUTransferBuffer(SDL_GPUDevice *device,
                                                std::uint32_t bufferSize)
{
  SDL_GPUTransferBufferCreateInfo bufferInfo{};
  bufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  bufferInfo.size = bufferSize;
  SDL_GPUTransferBuffer *transferBuffer =
      SDL_CreateGPUTransferBuffer(device, &bufferInfo);
  if (not transferBuffer)
  {
    std::string errorMsg{"SDL_CreateGPUTransferBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUTransferBuffer{transferBuffer,
                                 GPUTransferBufferDeleter{device}};
}

UniqueGPUMappedBuffer
mapGPUTransferBuffer(SDL_GPUDevice *device,
                     SDL_GPUTransferBuffer *transferBuffer, bool cycle)
{
  void *mappedBuffer = SDL_MapGPUTransferBuffer(device, transferBuffer, cycle);
  if (not mappedBuffer)
  {
    std::string errorMsg{"SDL_MapGPUTransferBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUMappedBuffer{
      mappedBuffer, GPUTransferBufferUnmapper{device, transferBuffer}};
}

struct GPUBufferDeleter
{
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUBuffer *buffer) const;
};
using UniqueGPUBuffer = std::unique_ptr<SDL_GPUBuffer, GPUBufferDeleter>;
void GPUBufferDeleter::operator()(SDL_GPUBuffer *buffer) const
{
  SDL_ReleaseGPUBuffer(device_ctx, buffer);
}

UniqueGPUBuffer createGPUBuffer(SDL_GPUDevice *device,
                                const SDL_GPUBufferCreateInfo *createInfo)
{
  SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, createInfo);
  if (not buffer)
  {
    std::string errorMsg{"SDL_CreateGPUBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUBuffer{buffer, GPUBufferDeleter{device}};
}

struct GPUGraphicsPipelineDeleter
{
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUGraphicsPipeline *pipeline) const;
};
using UniqueGPUGraphicsPipeline =
    std::unique_ptr<SDL_GPUGraphicsPipeline, GPUGraphicsPipelineDeleter>;
void GPUGraphicsPipelineDeleter::operator()(
    SDL_GPUGraphicsPipeline *pipeline) const
{
  SDL_ReleaseGPUGraphicsPipeline(device_ctx, pipeline);
}

UniqueGPUGraphicsPipeline
createGPUGraphicsPipeline(SDL_GPUDevice *device,
                          SDL_GPUGraphicsPipelineCreateInfo *pipelineInfo)
{
  SDL_GPUGraphicsPipeline *pipeline =
      SDL_CreateGPUGraphicsPipeline(device, pipelineInfo);
  if (not pipeline)
  {
    std::string errorMsg{"SDL_CreateGPUGraphicsPipeline failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUGraphicsPipeline{pipeline,
                                   GPUGraphicsPipelineDeleter{device}};
}

UniqueGPUGraphicsPipeline
createGlyphGPUGraphicsPipeline(SDL_GPUDevice *device,
                               SDL_GPUTextureFormat targetFormat)
{
  SDL_GPUShaderCreateInfo vertexInfo{};
  vertexInfo.code = vertexRectInstance.data();
  vertexInfo.code_size = vertexRectInstance.size();
  vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vertexInfo.num_uniform_buffers = 1;
  UniqueGPUShader vertexShader = createGPUShader(device, vertexInfo);

  SDL_GPUShaderCreateInfo fragmentInfo{};
  fragmentInfo.code = fragmentTint.data();
  fragmentInfo.code_size = fragmentTint.size();
  fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fragmentInfo.num_samplers = 1;
  fragmentInfo.num_uniform_buffers = 1;
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
      offsetof(RectInstanceData, rect_position);
  std::get<1>(vertexAttributes).location = 1;
  std::get<1>(vertexAttributes).buffer_slot = 0;
  std::get<1>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<1>(vertexAttributes).offset = offsetof(RectInstanceData, rect_size);
  std::get<2>(vertexAttributes).location = 2;
  std::get<2>(vertexAttributes).buffer_slot = 0;
  std::get<2>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  std::get<2>(vertexAttributes).offset = offsetof(RectInstanceData, uv_rect);

  SDL_GPUVertexBufferDescription vertexBufferDescriptions{};
  vertexBufferDescriptions.slot = 0;
  vertexBufferDescriptions.pitch = sizeof(RectInstanceData);
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
createSelectionGPUGraphicsPipeline(SDL_GPUDevice *device)
{
  SDL_GPUShaderCreateInfo vertexInfo{};
  vertexInfo.code = vertexRectInstance.data();
  vertexInfo.code_size = vertexRectInstance.size();
  vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vertexInfo.num_uniform_buffers = 1;
  UniqueGPUShader vertexShader = createGPUShader(device, vertexInfo);

  SDL_GPUShaderCreateInfo fragmentInfo{};
  fragmentInfo.code = fragmentSelection.data();
  fragmentInfo.code_size = fragmentSelection.size();
  fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fragmentInfo.num_uniform_buffers = 1;
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
  std::get<0>(vertexAttributes).offset =
      offsetof(RectInstanceData, rect_position);
  std::get<1>(vertexAttributes).location = 1;
  std::get<1>(vertexAttributes).buffer_slot = 0;
  std::get<1>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<1>(vertexAttributes).offset = offsetof(RectInstanceData, rect_size);
  std::get<2>(vertexAttributes).location = 2;
  std::get<2>(vertexAttributes).buffer_slot = 0;
  std::get<2>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  std::get<2>(vertexAttributes).offset = offsetof(RectInstanceData, uv_rect);

  SDL_GPUVertexBufferDescription vertexBufferDescriptions{};
  vertexBufferDescriptions.slot = 0;
  vertexBufferDescriptions.pitch = sizeof(RectInstanceData);
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

struct GPUComputePipelineDeleter
{
  SDL_GPUDevice *device_ctx;
  void operator()(SDL_GPUComputePipeline *pipeline) const;
};
using UniqueGPUComputePipeline =
    std::unique_ptr<SDL_GPUComputePipeline, GPUComputePipelineDeleter>;
void GPUComputePipelineDeleter::operator()(
    SDL_GPUComputePipeline *pipeline) const
{
  SDL_ReleaseGPUComputePipeline(device_ctx, pipeline);
}

UniqueGPUComputePipeline
createGPUComputePipeline(SDL_GPUDevice *device,
                         SDL_GPUComputePipelineCreateInfo *pipelineInfo)
{
  SDL_GPUComputePipeline *pipeline =
      SDL_CreateGPUComputePipeline(device, pipelineInfo);
  if (not pipeline)
  {
    std::string errorMsg{"SDL_CreateGPUComputePipeline failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUComputePipeline{pipeline, GPUComputePipelineDeleter{device}};
}

UniqueGPUComputePipeline createDifferenceBlendGPUPipeline(SDL_GPUDevice *device)
{
  SDL_GPUComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.code = computeDifferenceBlend.data();
  pipelineInfo.code_size = computeDifferenceBlend.size();
  pipelineInfo.entrypoint = "main";
  pipelineInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  pipelineInfo.num_readonly_storage_textures = 2;
  pipelineInfo.num_readwrite_storage_textures = 1;
  pipelineInfo.threadcount_x = 8;
  pipelineInfo.threadcount_y = 8;
  pipelineInfo.threadcount_z = 1;
  return createGPUComputePipeline(device, &pipelineInfo);
}

struct GPUCommandBufferSubmitter
{
  void operator()(SDL_GPUCommandBuffer *commandBuffer) const;
};
using EngagedGPUCommandBuffer =
    std::unique_ptr<SDL_GPUCommandBuffer, GPUCommandBufferSubmitter>;
void GPUCommandBufferSubmitter::operator()(
    SDL_GPUCommandBuffer *commandBuffer) const
{
  if (not SDL_SubmitGPUCommandBuffer(commandBuffer))
    SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
}

struct GPUCommandBufferCanceller
{
  void operator()(SDL_GPUCommandBuffer *commandBuffer) const;
};
using DismissedGPUCommandBuffer =
    std::unique_ptr<SDL_GPUCommandBuffer, GPUCommandBufferCanceller>;
void GPUCommandBufferCanceller::operator()(
    SDL_GPUCommandBuffer *commandBuffer) const
{
  if (not SDL_CancelGPUCommandBuffer(commandBuffer))
    SDL_Log("SDL_CancelGPUCommandBuffer failed: %s", SDL_GetError());
}

EngagedGPUCommandBuffer acquireGPUCommandBuffer(SDL_GPUDevice *device)
{
  SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
  if (not commandBuffer)
  {
    std::string errorMsg{"SDL_AcquireGPUCommandBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return EngagedGPUCommandBuffer{commandBuffer};
}

struct GPURenderPassDeleter
{
  void operator()(SDL_GPURenderPass *renderPass) const;
};
using UniqueGPURenderPass =
    std::unique_ptr<SDL_GPURenderPass, GPURenderPassDeleter>;
void GPURenderPassDeleter::operator()(SDL_GPURenderPass *renderPass) const
{
  SDL_EndGPURenderPass(renderPass);
}

UniqueGPURenderPass beginGPURenderPass(SDL_GPUCommandBuffer *commandBuffer,
                                       const SDL_GPUColorTargetInfo *targetInfo)
{
  SDL_GPURenderPass *renderPass =
      SDL_BeginGPURenderPass(commandBuffer, targetInfo, 1, nullptr);
  return UniqueGPURenderPass{renderPass};
}

SDL_GPUTexture *waitAndAcquireGPUSwapchainTexture(
    SDL_GPUCommandBuffer *commandBuffer, SDL_Window *window,
    std::uint32_t *texture_width, std::uint32_t *texture_height)
{
  SDL_GPUTexture *texture{};
  if (not SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &texture,
                                                texture_width, texture_height))
  {
    std::string errorMsg{"SDL_WaitAndAcquireGPUSwapchainTexture failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return texture;
}

struct GPUCopyPassDeleter
{
  void operator()(SDL_GPUCopyPass *copyPass) const;
};
using UniqueGPUCopyPass = std::unique_ptr<SDL_GPUCopyPass, GPUCopyPassDeleter>;
void GPUCopyPassDeleter::operator()(SDL_GPUCopyPass *copyPass) const
{
  SDL_EndGPUCopyPass(copyPass);
}

struct GPUComputePassDeleter
{
  void operator()(SDL_GPUComputePass *computePass) const;
};
using UniqueGPUComputePass =
    std::unique_ptr<SDL_GPUComputePass, GPUComputePassDeleter>;
void GPUComputePassDeleter::operator()(SDL_GPUComputePass *computePass) const
{
  SDL_EndGPUComputePass(computePass);
}

UniqueGPUComputePass
beginGPUComputePass(SDL_GPUCommandBuffer *commandBuffer,
                    const SDL_GPUStorageTextureReadWriteBinding *textureBinding)
{
  SDL_GPUComputePass *computePass =
      SDL_BeginGPUComputePass(commandBuffer, textureBinding, 1, nullptr, 0);
  return UniqueGPUComputePass{computePass};
}

struct UTextDeleter
{
  void operator()(UText *utextPtr) const { utext_close(utextPtr); }
};
using UniqueUText = std::unique_ptr<UText, UTextDeleter>;

UniqueUText utextOpenUTF8(std::string_view strView)
{
  std::int64_t textLen = static_cast<std::int64_t>(strView.size());
  UErrorCode status{U_ZERO_ERROR};
  UText *textPtr = utext_openUTF8(nullptr, strView.data(), textLen, &status);
  if (U_FAILURE(status))
  {
    std::string errorMsg{"utext_openUTF8 failed: "};
    throw std::runtime_error{errorMsg + u_errorName(status)};
  }
  return UniqueUText{textPtr};
}

struct CodepointInfo
{
  std::uint32_t byte_offset;
  UChar32 codepoint;
};
std::generator<CodepointInfo> emitCodepointInfo(std::string_view strView)
{
  UniqueUText uText = utextOpenUTF8(strView);
uTextLoop:
  std::int64_t byteOffset = utext_getNativeIndex(uText.get());
  UChar32 codepoint = utext_next32(uText.get());
  if (codepoint == U_SENTINEL)
    co_return;
  CodepointInfo codepointInfo{};
  codepointInfo.byte_offset = static_cast<std::uint32_t>(byteOffset);
  codepointInfo.codepoint = codepoint;
  co_yield codepointInfo;
  goto uTextLoop;
}

struct GlyphAtlasItem
{
  UniqueFreetypeGlyph ftglyph;
  std::array<std::uint32_t, 3> atlas_position;
  double lsb_delta, rsb_delta;
};

using GlyphAtlasLabel = std::pair<hb_codepoint_t, int>;
using GlyphAtlasCache = std::flat_map<GlyphAtlasLabel, GlyphAtlasItem>;

struct GlyphGeometry
{
  std::optional<GlyphAtlasLabel> atlas_label;
  double w, h;
  std::array<double, 4> bbox;
  std::array<double, 4> uv;
};

struct GlyphClusterSection
{
  std::uint32_t cluster;
  std::span<const CodepointInfo> codepoint_infos;
  std::span<const GlyphGeometry> geometries;
};

struct GlyphAtlasCopier
{
  UniqueGPUMappedBuffer mapped_atlas;
  const std::array<std::uint32_t, 3> *atlas_position;
  const FT_Bitmap *bitmap;
  void copy_bitmap(const GlyphAtlasItem &cacheItem);
  void copy_bitmap_row(std::uint32_t row);
};

struct GlyphCaretStep
{
  double pen_position, pen_advance;
  double horz_offset, vert_offset, horz_position;
  int subpixel_phase;
};

struct AppState;
struct GlyphSequencer
{
  AppState *app;
  UniqueHarfbuzzBuffer harfbuzz_buffer;

  double translate_x = 0, translate_y = 0;
  double x_min = 0, x_max = 0;
  double y_min = 0, y_max = 0;
  double measured_w() const noexcept { return x_max - x_min; }
  double measured_h() const noexcept { return y_max - y_min; }
  std::array<double, 4> bounding_box() const noexcept;

  std::vector<GlyphGeometry> glyph_geometries;
  std::vector<GlyphCaretStep> glyph_steps;
  std::vector<GlyphClusterSection> cluster_sections;
  std::vector<CodepointInfo> codepoint_infos;

  void populate_glyph_geometries();
  std::generator<GlyphCaretStep> emit_glyph_steps();
  std::vector<GlyphClusterSection> make_cluster_sections();

  UniqueGPUBuffer instance_buffer;
  UniqueGPUTransferBuffer instance_transfer;
  std::uint32_t instance_capacity = 0;

  void verify_instance_capacity();
  void transfer_instance_data();
  void queue_vertex_copypass();

  void instantiate(SDL_Window *window);
  bool verify_atlas_cache();

  bool text_reshape(std::string_view);
  void text_reflow();
  std::array<double, 4> text_measure(std::pair<std::uint32_t, double>,
                                     std::pair<std::uint32_t, double>,
                                     bool) const;
  std::uint32_t find_codepoint_offset(double screenX) const;
  std::pair<std::uint32_t, double>
  find_cluster_offset(std::uint32_t textOffset) const;
};

struct CopyGlyphAtlas
{
};
struct CopyGlyphVertices
{
  GlyphSequencer *sequencer;
};
struct CopySelectionAreas
{
  std::uint32_t num_selections;
};
using CopypassTask =
    std::variant<CopyGlyphAtlas, CopyGlyphVertices, CopySelectionAreas>;
struct CopypassTaskVisitor
{
  void operator()(CopyGlyphAtlas task) const;
  void operator()(CopyGlyphVertices task) const;
  void operator()(CopySelectionAreas task) const;
  AppState *app;
  SDL_GPUCopyPass *copypass;
};

struct GPUCommandSequencer
{
  AppState *app;
  EngagedGPUCommandBuffer command_buffer;
  void issue_copy_pass();
  void issue_render_pass();
};

struct GPURenderSequencer
{
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

struct SelectionDragState
{
  std::uint32_t selection_idx, sequencer_idx;
  std::array<double, 2> mouse_segment;
};

struct SelectionEventHandler
{
  AppState *app;
  std::optional<SelectionDragState> drag_state;

  std::uint32_t find_unoccupied_slot();
  void copy_drag_state();

  void handle_mouse_button_down(const SDL_MouseButtonEvent &event);
  void handle_mouse_button_up(const SDL_MouseButtonEvent &event);
  void handle_mouse_motion(const SDL_MouseMotionEvent &event);
};

struct AppState
{
  UniqueHarfbuzzFont harfbuzz_font;

  UniqueGPUDevice device_ctx;
  UniqueWindow main_window;
  UniqueWindowForGPUDevice main_window_for_gpu_device;

  std::string caption_string = "Hello figure kerning Text";

  static constexpr std::uint32_t ATLAS_SIDE = 1024;
  static constexpr std::uint32_t ATLAS_PADDING = 1;
  UniqueGPUTransferBuffer atlas_transfer;
  UniqueGPUTexture atlas_texture;
  GlyphAtlasCache atlas_cache;
  std::vector<GlyphSequencer> glyph_sequencers;

  UniqueFreetypeGlyph freetype_load_glyph(hb_codepoint_t glyphid, int phase);
  const FT_Glyph_Metrics &freetype_codepoint_metrics(FT_ULong codepoint);

  UniqueGPUTexture offscreen_texture;
  UniqueGPUTexture selection_texture;
  UniqueGPUTexture processed_texture;
  std::uint32_t offscreen_w = 0, offscreen_h = 0;
  double haitch_height = 0;

  SelectionEventHandler selection_handler{this};
  std::vector<std::optional<std::array<double, 4>>> selection_slots;
  bool selection_areas_flag = false;

  UniqueGPUSampler sampler;
  UniqueGPUGraphicsPipeline swapchain_pipeline;
  UniqueGPUGraphicsPipeline offscreen_pipeline;
  UniqueGPUGraphicsPipeline selection_pipeline;
  UniqueGPUComputePipeline difference_blend_pipeline;

  UniqueGPUBuffer selection_instances;
  UniqueGPUTransferBuffer selection_transfer;
  std::uint32_t selection_instances_capacity = 0;

  void verify_selection_capacity(std::uint32_t numSelections);
  void transfer_selection_areas();
  void selection_reflow();

  std::vector<CopypassTask> copypass_tasks;

  SDL_AppResult iterate() noexcept;
  SDL_AppResult handle_event(SDL_Event *event) noexcept;

  void instantiate(FT_Library library);
  void pack_atlas();
};

void AppState::instantiate(FT_Library library)
{
  device_ctx = createGPUDevice();
  main_window = createWindow("Hello GPU", static_cast<int>(INITIAL_WINDOW_W),
                             static_cast<int>(INITIAL_WINDOW_H));
  setWindowMinimumSize(main_window.get(), static_cast<int>(INITIAL_WINDOW_W),
                       static_cast<int>(INITIAL_WINDOW_H));
  main_window_for_gpu_device =
      claimWindowForGPUDevice(device_ctx.get(), main_window.get());
  harfbuzz_font = createHarfbuzzFont(library, "assets/DejaVuSans.ttf", 10 * 64,
                                     getWindowDisplayScale(main_window.get()));
  hb_ft_font_set_load_flags(harfbuzz_font.get(),
                            FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
  selection_pipeline = createSelectionGPUGraphicsPipeline(device_ctx.get());
  difference_blend_pipeline =
      createDifferenceBlendGPUPipeline(device_ctx.get());
  GPUTextureCreateInfo textureInfo{.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                                   .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                   .texture_w = ATLAS_SIDE,
                                   .texture_h = ATLAS_SIDE};
  atlas_texture = createGPUTexture(device_ctx.get(), textureInfo);
  atlas_transfer =
      createGPUTransferBuffer(device_ctx.get(), ATLAS_SIDE * ATLAS_SIDE);
  sampler = createCommonGPUSampler(device_ctx.get());
  swapchain_pipeline = createGlyphGPUGraphicsPipeline(
      device_ctx.get(),
      SDL_GetGPUSwapchainTextureFormat(device_ctx.get(), main_window.get()));
  offscreen_pipeline = createGlyphGPUGraphicsPipeline(
      device_ctx.get(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
  const FT_Glyph_Metrics &haitch_metrics = freetype_codepoint_metrics(72);
  haitch_height = static_cast<double>(haitch_metrics.height) / 64.0;
  glyph_sequencers.emplace_back(this).instantiate(main_window.get());
}

void GPUCommandSequencer::issue_copy_pass()
{
  if (app->copypass_tasks.empty())
    return;
  UniqueGPUCopyPass copypass{SDL_BeginGPUCopyPass(command_buffer.get())};
  for (const CopypassTask &copypassTask : app->copypass_tasks)
    copypassTask.visit(CopypassTaskVisitor{app, copypass.get()});
  app->copypass_tasks.clear();
}

void CopypassTaskVisitor::operator()(CopyGlyphVertices task) const
{
  SDL_GPUTransferBufferLocation src{
      .transfer_buffer = task.sequencer->instance_transfer.get(), .offset = 0};
  std::uint32_t bufferSize{static_cast<std::uint32_t>(
      hb_buffer_get_length(task.sequencer->harfbuzz_buffer.get()) *
      sizeof(RectInstanceData))};
  SDL_GPUBufferRegion dst{.buffer = task.sequencer->instance_buffer.get(),
                          .offset = 0,
                          .size = bufferSize};
  SDL_UploadToGPUBuffer(copypass, &src, &dst, false);
}

void GPUCommandSequencer::issue_render_pass()
{
  GPURenderSequencer render_sequencer{app, this};
  render_sequencer.swapchain_texture = waitAndAcquireGPUSwapchainTexture(
      command_buffer.get(), app->main_window.get(),
      &render_sequencer.swapchain_w, &render_sequencer.swapchain_h);
  if (not render_sequencer.swapchain_texture)
    return;
  auto numSelections =
      std::ranges::distance(app->selection_slots | std::views::join);
  if (numSelections == 0)
  {
    SDL_GPUGraphicsPipeline *scenePipeline = app->swapchain_pipeline.get();
    render_sequencer.draw_scene(render_sequencer.swapchain_texture,
                                scenePipeline);
  }
  else
  {
    render_sequencer.verify_storage_textures();
    SDL_GPUGraphicsPipeline *scenePipeline = app->offscreen_pipeline.get();
    render_sequencer.draw_scene(app->offscreen_texture.get(), scenePipeline);
    render_sequencer.selected_areas_draw();
    render_sequencer.selected_areas_blend();
    render_sequencer.draw_processed_image();
  }
}

void GPURenderSequencer::draw_processed_image()
{
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
                                    SDL_GPUGraphicsPipeline *pipeline)
{
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
  SDL_GPUTextureSamplerBinding samplerBinding{};
  samplerBinding.texture = app->atlas_texture.get();
  samplerBinding.sampler = app->sampler.get();
  SDL_BindGPUFragmentSamplers(renderPass.get(), 0, &samplerBinding, 1);

  struct alignas(16) BlitUniform
  {
    float tint[4];
  };
  BlitUniform blit{.tint = {1.0f, 1.0f, 1.0f, 1.0f}};
  SDL_PushGPUFragmentUniformData(cmd, 0, &blit, sizeof(BlitUniform));

  struct alignas(16) RectUniform
  {
    float viewport[2];
    float translate[2];
  };
  for (const GlyphSequencer &sequencer : app->glyph_sequencers)
  {
    SDL_GPUBufferBinding instanceBinding{sequencer.instance_buffer.get(), 0};
    SDL_BindGPUVertexBuffers(renderPass.get(), 0, &instanceBinding, 1);

    RectUniform rectUni{};
    rectUni.translate[0] =
        static_cast<float>(std::trunc(sequencer.translate_x));
    rectUni.translate[1] =
        static_cast<float>(std::trunc(sequencer.translate_y));
    rectUni.viewport[0] = static_cast<float>(swapchain_w);
    rectUni.viewport[1] = static_cast<float>(swapchain_h);
    SDL_PushGPUVertexUniformData(cmd, 0, &rectUni, sizeof(RectUniform));

    std::uint32_t numInstances =
        hb_buffer_get_length(sequencer.harfbuzz_buffer.get());
    SDL_DrawGPUPrimitives(renderPass.get(), 4, numInstances, 0, 0);
  }
}

void GPURenderSequencer::verify_storage_textures()
{
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

void GPURenderSequencer::selected_areas_draw()
{
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

  struct alignas(16) RectUniform
  {
    float viewport[2];
    float translate[2];
  };
  RectUniform rectUni{};
  rectUni.viewport[0] = static_cast<float>(swapchain_w);
  rectUni.viewport[1] = static_cast<float>(swapchain_h);
  std::array<float, 2> tx{0, static_cast<float>(swapchain_h)};
  std::memcpy(&rectUni.translate, tx.data(), sizeof(RectUniform::translate));
  SDL_PushGPUVertexUniformData(cmd, 0, &rectUni, sizeof(RectUniform));

  struct alignas(16) BoxUniform
  {
    float blend_color[4];
    float radius;
  };
  BoxUniform boxUniform{.blend_color = {1.0f, 1.0f, 1.0f, 1.0f},
                        .radius = 0.2f};
  SDL_PushGPUFragmentUniformData(cmd, 0, &boxUniform, sizeof(BoxUniform));

  std::uint32_t numInstances = static_cast<std::uint32_t>(
      std::ranges::distance(app->selection_slots | std::views::join));
  SDL_DrawGPUPrimitives(renderPass.get(), 4, numInstances, 0, 0);
}

void GPURenderSequencer::selected_areas_blend()
{
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

SDL_AppResult AppState::iterate() noexcept
{
  try
  {
    auto numSelections =
        std::ranges::distance(selection_slots | std::views::join);
    if (selection_areas_flag && numSelections > 0)
    {
      selection_reflow();
      selection_areas_flag = false;
    }
    GPUCommandSequencer command_sequencer{this};
    command_sequencer.command_buffer =
        acquireGPUCommandBuffer(device_ctx.get());
    command_sequencer.issue_copy_pass();
    command_sequencer.issue_render_pass();
    return SDL_APP_CONTINUE;
  }
  catch (const std::runtime_error &e)
  {
    SDL_Log("[AppIterate] %s", e.what());
    return SDL_APP_FAILURE;
  }
}

std::array<double, 4> GlyphSequencer::bounding_box() const noexcept
{
  static constexpr double PADDING_X = 1;
  static constexpr double PADDING_Y = 4;
  std::array<double, 4> boundingBox{};
  std::get<0>(boundingBox) = translate_x + x_min - PADDING_X;
  std::get<1>(boundingBox) = translate_y + y_max - PADDING_Y;
  std::get<2>(boundingBox) = measured_w() + PADDING_X * 2;
  std::get<3>(boundingBox) = measured_h() + PADDING_Y * 2;
  return boundingBox;
}

std::uint32_t SelectionEventHandler::find_unoccupied_slot()
{
  auto selectionPredicate = [](const std::optional<std::array<double, 4>> &slot)
  { return not slot.has_value(); };
  auto selectionIt =
      std::ranges::find_if(app->selection_slots, selectionPredicate);
  if (selectionIt == app->selection_slots.end())
    selectionIt =
        app->selection_slots.insert(app->selection_slots.end(), std::nullopt);
  selectionIt->emplace();
  return static_cast<std::uint32_t>(
      std::distance(app->selection_slots.begin(), selectionIt));
}

void SelectionEventHandler::copy_drag_state()
{
  auto &[selectionX, selectionY, selectionW, selectionH] =
      *app->selection_slots[drag_state->selection_idx];
  const auto &[mouseA, mouseB] = drag_state->mouse_segment;
  selectionX = std::min(mouseA, mouseB);
  constexpr double MIN_SELECTION_W = 2.0;
  selectionW = std::max(std::abs(mouseA - mouseB), MIN_SELECTION_W);
  auto [captionX, captionY, captionW, captionH] =
      app->glyph_sequencers[drag_state->sequencer_idx].bounding_box();
  selectionY = captionY;
  selectionH = captionH;
  app->selection_areas_flag = true;
}

bool isPointInside(const std::array<double, 2> &point,
                   const std::array<double, 4> &rectangle)
{
  const auto &[pointX, pointY] = point;
  const auto &[rectangleX, rectangleY, rectangleW, rectangleH] = rectangle;
  return (pointX >= rectangleX) && (pointX <= rectangleX + rectangleW) &&
         (pointY >= rectangleY) && (pointY <= rectangleY + rectangleH);
}

void SelectionEventHandler::handle_mouse_button_down(
    const SDL_MouseButtonEvent &event)
{
  for (std::optional<std::array<double, 4>> &selection : app->selection_slots)
    selection.reset();
  for (const GlyphSequencer &sequencer : app->glyph_sequencers)
    if (isPointInside({event.x, event.y}, sequencer.bounding_box()))
    {
      drag_state.emplace();
      drag_state->sequencer_idx =
          static_cast<std::uint32_t>(&sequencer - app->glyph_sequencers.data());
      drag_state->selection_idx = find_unoccupied_slot();
      std::get<0>(drag_state->mouse_segment) = event.x;
      std::get<1>(drag_state->mouse_segment) = event.x;
      copy_drag_state();
      break;
    }
}

std::uint32_t GlyphSequencer::find_codepoint_offset(double screenX) const
{
  std::size_t captionOffset{app->caption_string.size()};
  for (const GlyphClusterSection &clusterSection : cluster_sections)
  {
    double clusterL = std::get<0>(clusterSection.geometries.front().bbox);
    double clusterR = std::get<1>(clusterSection.geometries.back().bbox);
    double numCodepoints =
        static_cast<double>(clusterSection.codepoint_infos.size());
    double clusterHorz = std::max(screenX, clusterL) - clusterL;
    double approxCodepointIdx =
        std::round(clusterHorz / (clusterR - clusterL) * numCodepoints);
    if (approxCodepointIdx >= numCodepoints)
      continue;
    std::uint32_t codepointIdx = static_cast<std::uint32_t>(approxCodepointIdx);
    const CodepointInfo &codepointInfo =
        clusterSection.codepoint_infos.at(codepointIdx);
    captionOffset = codepointInfo.byte_offset;
    break;
  }
  return static_cast<std::uint32_t>(captionOffset);
}

std::pair<std::uint32_t, double>
GlyphSequencer::find_cluster_offset(std::uint32_t textOffset) const
{
  std::pair<std::uint32_t, double> clusterOffset{};
  for (const auto &[enumerationIdx, clusterSection] :
       cluster_sections | std::views::enumerate)
  {
    auto findPred = [textOffset](std::uint32_t offset)
    { return offset == textOffset; };
    auto codepointIt = std::ranges::find_if(
        clusterSection.codepoint_infos, findPred, &CodepointInfo::byte_offset);
    const std::span<const CodepointInfo> &codepointInfos =
        clusterSection.codepoint_infos;
    auto &[clusterIndex, normalT] = clusterOffset;
    clusterIndex = static_cast<std::uint32_t>(enumerationIdx);
    normalT = static_cast<double>(codepointIt - codepointInfos.begin()) /
              static_cast<double>(codepointInfos.size());
    if (codepointIt != codepointInfos.end())
      break;
  }
  return clusterOffset;
}

void SelectionEventHandler::handle_mouse_button_up(
    const SDL_MouseButtonEvent &event)
{
  if (not drag_state.has_value())
    return;
  const GlyphSequencer &sequencer =
      app->glyph_sequencers[drag_state->sequencer_idx];
  auto [captionX, captionY, captionW, captionH] = sequencer.bounding_box();
  std::array<double, 2> screenX{drag_state->mouse_segment};
  std::ranges::sort(screenX);

  double startX = std::get<0>(screenX) - captionX;
  std::uint32_t codepointStart = sequencer.find_codepoint_offset(startX);
  std::pair<std::uint32_t, double> clusterStart =
      sequencer.find_cluster_offset(codepointStart);

  double finalX = std::get<1>(screenX) - captionX;
  std::uint32_t codepointFinal = sequencer.find_codepoint_offset(finalX);
  std::pair<std::uint32_t, double> clusterFinal =
      sequencer.find_cluster_offset(codepointFinal);

  auto [measureL, measureR, measureT, measureB] =
      sequencer.text_measure(clusterStart, clusterFinal, true);
  double measureW = measureR - measureL;
  double measureH = measureB - measureT;

  std::optional<std::array<double, 4>> &selection =
      app->selection_slots[drag_state->selection_idx];
  if (measureL == measureR)
    selection.reset();
  else
  {
    static constexpr double PADDING_X = 1;
    static constexpr double PADDING_Y = 4;
    auto &[selectionX, selectionY, selectionW, selectionH] = *selection;
    selectionX = sequencer.translate_x + measureL - PADDING_X;
    selectionY =
        sequencer.translate_y + sequencer.measured_h() + measureT - PADDING_Y;
    selectionW = measureW + PADDING_X * 2;
    selectionH = measureH + PADDING_Y * 2;
  }

  app->selection_areas_flag = true;
  drag_state.reset();
}

void SelectionEventHandler::handle_mouse_motion(
    const SDL_MouseMotionEvent &event)
{
  if (not drag_state.has_value())
    return;
  auto [captionX, captionY, captionW, captionH] = app->caption_bbox();
  float captionMin = static_cast<float>(captionX);
  float captionMax = static_cast<float>(captionX + captionW);
  std::get<1>(drag_state->mouse_segment) =
      std::clamp(event.x, captionMin, captionMax);
  copy_drag_state();
}

SDL_AppResult AppState::handle_event(SDL_Event *event) noexcept
{
  switch (event->type)
  {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    selection_handler.handle_mouse_button_down(event->button);
    return SDL_APP_CONTINUE;

  case SDL_EVENT_MOUSE_MOTION:
    selection_handler.handle_mouse_motion(event->motion);
    return SDL_APP_CONTINUE;

  case SDL_EVENT_MOUSE_BUTTON_UP:
    selection_handler.handle_mouse_button_up(event->button);
    return SDL_APP_CONTINUE;

  case SDL_EVENT_WINDOW_RESIZED:
    caption_x = static_cast<double>(event->window.data1) * 0.5 -
                caption_sequencer.measured_w() * 0.5;
    caption_y = static_cast<double>(event->window.data2) * 0.5 -
                caption_sequencer.measured_h() * 0.5;
    return SDL_APP_CONTINUE;

  case SDL_EVENT_QUIT:
    return SDL_APP_SUCCESS;

  default:
    return SDL_APP_CONTINUE;
  }
}

void GlyphSequencer::instantiate(SDL_Window *window)
{
  harfbuzz_buffer = UniqueHarfbuzzBuffer{hb_buffer_create()};
}

bool GlyphSequencer::verify_atlas_cache()
{
  std::span<hb_glyph_info_t> glyphInfos{
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr),
      hb_buffer_get_length(harfbuzz_buffer.get())};
  bool everInserted{};
  for (const auto &[step, glyphInfo] : std::views::zip(glyph_steps, glyphInfos))
  {
    const GlyphAtlasLabel cacheLabel{glyphInfo.codepoint, step.subpixel_phase};
    if (app->atlas_cache.contains(cacheLabel))
      continue;
    UniqueFreetypeGlyph ftglyph =
        app->freetype_load_glyph(glyphInfo.codepoint, step.subpixel_phase);
    const FT_Bitmap &bitmap = asBitmapGlyph(ftglyph.get())->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0)
      continue;
    FT_Face freetypeFace = hb_ft_font_get_ft_face(app->harfbuzz_font.get());
    GlyphAtlasItem &atlasItem = app->atlas_cache[cacheLabel];
    atlasItem.lsb_delta =
        static_cast<double>(freetypeFace->glyph->lsb_delta) / 64.0;
    atlasItem.rsb_delta =
        static_cast<double>(freetypeFace->glyph->rsb_delta) / 64.0;
    atlasItem.ftglyph = std::move(ftglyph);
    everInserted = true;
  }
  return everInserted;
}

void AppState::pack_atlas()
{
  struct AtlasColumnItem
  {
    GlyphAtlasLabel atlas_label;
    std::uint32_t atlas_offset;
  };
  auto atlasLabelProj = [](const GlyphAtlasLabel &atlasLabel)
  { return AtlasColumnItem{.atlas_label = atlasLabel}; };
  std::vector<AtlasColumnItem> atlasColumnItems =
      atlas_cache.keys() | std::views::transform(atlasLabelProj) |
      std::ranges::to<std::vector<AtlasColumnItem>>();
  auto atlasGlyphH = [this](const AtlasColumnItem &columnItem)
  {
    const GlyphAtlasItem &atlasItem = atlas_cache.at(columnItem.atlas_label);
    return asBitmapGlyph(atlasItem.ftglyph.get())->bitmap.rows;
  };
  std::ranges::sort(atlasColumnItems, std::ranges::greater{}, atlasGlyphH);
  std::uint32_t accumulatedX{};
  for (AtlasColumnItem &columnItem : atlasColumnItems)
  {
    FT_BitmapGlyph bitmapGlyph =
        asBitmapGlyph(atlas_cache.at(columnItem.atlas_label).ftglyph.get());
    std::uint32_t bitmapW = bitmapGlyph->bitmap.width;
    if (bitmapW > ATLAS_SIDE)
      continue;
    columnItem.atlas_offset =
        accumulatedX + bitmapW > ATLAS_SIDE ? 0 : accumulatedX;
    accumulatedX = columnItem.atlas_offset + bitmapW + ATLAS_PADDING;
  }
  struct GlyphAtlasShelf
  {
    std::vector<AtlasColumnItem> children;
    std::uint32_t atlas_offset;
  };
  auto shelfPredicate =
      [](const AtlasColumnItem &viewA, const AtlasColumnItem &viewB)
  { return viewA.atlas_offset < viewB.atlas_offset; };
  auto shelfProject = [](auto shelfItems)
  {
    GlyphAtlasShelf atlasShelf{};
    atlasShelf.children = {std::from_range, shelfItems};
    return atlasShelf;
  };
  std::vector<GlyphAtlasShelf> atlasShelves =
      atlasColumnItems | std::views::chunk_by(shelfPredicate) |
      std::views::transform(shelfProject) |
      std::ranges::to<std::vector<GlyphAtlasShelf>>();
  std::uint32_t accumulatedY{};
  for (GlyphAtlasShelf &atlasShelf : atlasShelves)
  {
    const GlyphAtlasItem &atlasItem =
        atlas_cache.at(atlasShelf.children.front().atlas_label);
    std::uint32_t atlasShelfH =
        asBitmapGlyph(atlasItem.ftglyph.get())->bitmap.rows;
    if (atlasShelfH > ATLAS_SIDE)
      continue;
    atlasShelf.atlas_offset =
        accumulatedY + atlasShelfH > ATLAS_SIDE ? 0 : accumulatedY;
    accumulatedY = atlasShelf.atlas_offset + atlasShelfH + ATLAS_PADDING;
  }
  auto pagePredicate =
      [](const GlyphAtlasShelf &viewA, const GlyphAtlasShelf &viewB)
  { return viewA.atlas_offset < viewB.atlas_offset; };
  auto atlasPages = atlasShelves | std::views::chunk_by(pagePredicate) |
                    std::views::enumerate;
  for (const auto &[pageIdx, pageShelves] : atlasPages)
    for (const GlyphAtlasShelf &atlasShelf : pageShelves)
      for (const AtlasColumnItem &shelfChild : atlasShelf.children)
      {
        GlyphAtlasItem &atlasItem = atlas_cache.at(shelfChild.atlas_label);
        auto &[atlasX, atlasY, atlasPage] = atlasItem.atlas_position;
        atlasX = shelfChild.atlas_offset;
        atlasY = atlasShelf.atlas_offset;
        atlasPage = static_cast<std::uint32_t>(pageIdx);
      }
  GlyphAtlasCopier atlasCopier{};
  atlasCopier.mapped_atlas =
      mapGPUTransferBuffer(device_ctx.get(), atlas_transfer.get(), true);
  std::memset(atlasCopier.mapped_atlas.get(), 0, ATLAS_SIDE * ATLAS_SIDE);
  for (const auto &[cacheLabel, cacheItem] : atlas_cache)
    atlasCopier.copy_bitmap(cacheItem);
}

void GlyphAtlasCopier::copy_bitmap(const GlyphAtlasItem &cacheItem)
{
  atlas_position = &cacheItem.atlas_position;
  bitmap = &asBitmapGlyph(cacheItem.ftglyph.get())->bitmap;
  for (std::uint32_t row = 0; row < bitmap->rows; ++row)
    copy_bitmap_row(row);
}

void GlyphAtlasCopier::copy_bitmap_row(std::uint32_t row)
{
  const auto &[atlasX, atlasY, atlasPage] = *atlas_position;
  unsigned char *source = bitmap->buffer + row * bitmap->pitch;
  std::uint32_t atlasVertOffset = atlasY + row;
  std::uint32_t atlasFullOffset =
      atlasVertOffset * AppState::ATLAS_SIDE + atlasX;
  unsigned char *transferBegin =
      static_cast<unsigned char *>(mapped_atlas.get());
  std::memcpy(transferBegin + atlasFullOffset, source, bitmap->width);
}

void GlyphSequencer::verify_instance_capacity()
{
  std::uint32_t bufferSize = static_cast<std::uint32_t>(
      hb_buffer_get_length(harfbuzz_buffer.get()) * sizeof(RectInstanceData));
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

void GlyphSequencer::transfer_instance_data()
{
  unsigned int bufferIdx{};
  UniqueGPUMappedBuffer mappedBuffer = mapGPUTransferBuffer(
      app->device_ctx.get(), instance_transfer.get(), true);
  RectInstanceData *const bufferBegin =
      static_cast<RectInstanceData *>(mappedBuffer.get());
  for (const GlyphGeometry &geometry : glyph_geometries)
  {
    if (not geometry.atlas_label)
      continue;
    RectInstanceData vertexInstance{};
    vertexInstance.rect_position[0] =
        static_cast<float>(std::get<0>(geometry.bbox));
    vertexInstance.rect_position[1] =
        -1.0f * static_cast<float>(std::get<3>(geometry.bbox));
    vertexInstance.rect_size[0] = static_cast<float>(geometry.w);
    vertexInstance.rect_size[1] = static_cast<float>(geometry.h);
    vertexInstance.uv_rect[0] = static_cast<float>(std::get<0>(geometry.uv));
    vertexInstance.uv_rect[1] = static_cast<float>(std::get<1>(geometry.uv));
    vertexInstance.uv_rect[2] = static_cast<float>(std::get<2>(geometry.uv));
    vertexInstance.uv_rect[3] = static_cast<float>(std::get<3>(geometry.uv));
    std::memcpy(bufferBegin + bufferIdx, &vertexInstance,
                sizeof(RectInstanceData));
    ++bufferIdx;
  }
}

void GlyphSequencer::queue_vertex_copypass()
{
  verify_instance_capacity();
  transfer_instance_data();
  app->copypass_tasks.push_back(CopyGlyphVertices{this});
}

constexpr double MAX_SAFE_INTEGER = std::ldexp(1.0, 53) - 1;
constexpr double MIN_SAFE_INTEGER = -1.0 * MAX_SAFE_INTEGER;

std::array<double, 4>
GlyphSequencer::text_measure(std::pair<std::uint32_t, double> start,
                             std::pair<std::uint32_t, double> final,
                             bool finalBound) const
{
  std::array<double, 4> textMetrics{};
  auto &[textL, textR, textT, textB] = textMetrics;
  textL = MAX_SAFE_INTEGER, textR = MIN_SAFE_INTEGER;
  textT = MAX_SAFE_INTEGER, textB = MIN_SAFE_INTEGER;
  const auto [clusterStart, codepointStart] = start;
  const auto [clusterFinal, codepointFinal] = final;
  std::uint32_t sectionsLen =
      static_cast<std::uint32_t>(cluster_sections.size());
  std::uint32_t clusterEnd = finalBound ? clusterFinal + 1 : sectionsLen;
  std::span queriedSections{cluster_sections.begin() + clusterStart,
                            cluster_sections.begin() + clusterEnd};
  for (const auto &[enumerationIdx, clusterSection] :
       queriedSections | std::views::enumerate)
  {
    std::uint32_t clusterIdx =
        clusterStart + static_cast<std::uint32_t>(enumerationIdx);
    auto geometriesFirst = clusterSection.geometries.cbegin();
    auto geometriesFinal = clusterSection.geometries.crbegin();
    double clusterL = std::get<0>(geometriesFirst->bbox);
    double clusterR = std::get<0>(geometriesFinal->bbox) + geometriesFinal->w;
    if (geometriesFirst->atlas_label.has_value())
    {
      const GlyphAtlasItem &atlasItem =
          app->atlas_cache.at(*geometriesFirst->atlas_label);
      FT_BitmapGlyph bitmapGlyph = asBitmapGlyph(atlasItem.ftglyph.get());
      clusterL -= static_cast<double>(bitmapGlyph->left);
    }
    std::ranges::transform_view geometriesTop{
        clusterSection.geometries, [](const GlyphGeometry &geometry)
        { return std::get<2>(geometry.bbox); }};
    double clusterT = std::ranges::min(geometriesTop);
    std::ranges::transform_view geometriesBottom{
        clusterSection.geometries, [](const GlyphGeometry &geometry)
        { return std::get<3>(geometry.bbox); }};
    double clusterB = std::ranges::max(geometriesBottom);
    double normalStart = clusterIdx == clusterStart ? codepointStart : 0;
    double normalFinal =
        finalBound && clusterIdx == clusterFinal ? codepointFinal : 1;
    textL = std::min(textL, std::lerp(clusterL, clusterR, normalStart));
    textR = std::max(textR, std::lerp(clusterL, clusterR, normalFinal));
    if (normalFinal == 0)
      break;
    textT = std::min(textT, clusterT);
    textB = std::max(textB, clusterB);
  }
  return textMetrics;
}

std::generator<GlyphCaretStep> GlyphSequencer::emit_glyph_steps()
{
  unsigned int bufferLen = hb_buffer_get_length(harfbuzz_buffer.get());
  std::span<const hb_glyph_position_t> positions{
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), nullptr), bufferLen};
  GlyphCaretStep step{};
  auto getPositionInPixels = [](hb_position_t pos) -> double
  {
    constexpr double GLYPH_POSITION_UNIT = 1.0 / 64.0;
    return static_cast<double>(pos) * GLYPH_POSITION_UNIT;
  };
  for (const hb_glyph_position_t &position : positions)
  {
    step.horz_offset = getPositionInPixels(position.x_offset);
    step.horz_position = step.pen_position + step.horz_offset;
    constexpr int SUBPIXEL_PHASE_COUNT = 4;
    double phase = (step.horz_position - std::trunc(step.horz_position)) *
                   static_cast<double>(SUBPIXEL_PHASE_COUNT);
    step.subpixel_phase = static_cast<int>(std::round(phase));
    step.vert_offset = getPositionInPixels(position.y_offset);
    step.pen_advance = getPositionInPixels(position.x_advance);
    co_yield step;
    step.pen_position += step.pen_advance;
  }
}

void GlyphSequencer::populate_glyph_geometries()
{
  std::span<const hb_glyph_info_t> glyphInfos{
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr),
      hb_buffer_get_length(harfbuzz_buffer.get())};
  for (const auto &[step, glyphInfo] : std::views::zip(glyph_steps, glyphInfos))
  {
    const GlyphAtlasLabel atlasLabel{glyphInfo.codepoint, step.subpixel_phase};
    if (not app->atlas_cache.contains(atlasLabel))
    {
      GlyphGeometry geometry{};
      bool behind = glyph_geometries.size() > 0 &&
                    glyph_geometries.back().atlas_label.has_value();
      geometry.w = step.pen_advance;
      geometry.h = behind ? glyph_geometries.back().h : app->haitch_height;

      std::get<0>(geometry.bbox) = step.pen_position;
      std::get<1>(geometry.bbox) = step.pen_position + geometry.w;
      std::get<2>(geometry.bbox) = -1.0 * geometry.h;

      glyph_geometries.push_back(geometry);
      continue;
    }
    GlyphGeometry &geometry = glyph_geometries.emplace_back();
    geometry.atlas_label = atlasLabel;

    FT_BitmapGlyph bitmapGlyph =
        asBitmapGlyph(app->atlas_cache.at(atlasLabel).ftglyph.get());
    geometry.w = static_cast<double>(bitmapGlyph->bitmap.width);
    geometry.h = static_cast<double>(bitmapGlyph->bitmap.rows);

    std::get<0>(geometry.bbox) =
        std::trunc(step.horz_position) + static_cast<double>(bitmapGlyph->left);
    std::get<1>(geometry.bbox) = step.pen_advance + std::get<0>(geometry.bbox);
    std::get<2>(geometry.bbox) =
        step.vert_offset - static_cast<double>(bitmapGlyph->top);
    std::get<3>(geometry.bbox) = std::get<2>(geometry.bbox) + geometry.h;

    const auto &[atlasX, atlasY, atlasPage] =
        app->atlas_cache.at(atlasLabel).atlas_position;
    constexpr double SIDE_DIVISOR = static_cast<double>(AppState::ATLAS_SIDE);
    double atlasU = static_cast<double>(atlasX) / SIDE_DIVISOR;
    double atlasV = static_cast<double>(atlasY) / SIDE_DIVISOR;
    std::get<0>(geometry.uv) = atlasU;
    std::get<1>(geometry.uv) = atlasV;
    std::get<2>(geometry.uv) = atlasU + geometry.w / SIDE_DIVISOR;
    std::get<3>(geometry.uv) = atlasV + geometry.h / SIDE_DIVISOR;
  }
}

std::vector<GlyphClusterSection> GlyphSequencer::make_cluster_sections()
{
  const hb_glyph_info_t *glyphInfos =
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr);
  using BufferPair = std::pair<std::uint32_t, const GlyphGeometry *>;
  std::vector<BufferPair> bufferPairs{};
  for (std::size_t i{}; i < glyph_geometries.size(); ++i)
    bufferPairs.emplace_back(glyphInfos[i].cluster,
                             glyph_geometries.data() + i);

  std::vector<std::span<BufferPair>> clusterSpans{};
  auto clusterPredicate = [](const BufferPair &infoA, const BufferPair &infoB)
  { return infoA.first == infoB.first; };
  for (const auto &clusterChunk :
       bufferPairs | std::views::chunk_by(clusterPredicate))
    clusterSpans.emplace_back(clusterChunk);

  auto projectAdjacent = [this](const auto &clusterAdjacent)
  {
    const auto &[spanA, spanB] = clusterAdjacent;
    auto codepointItA = std::ranges::lower_bound(
        codepoint_infos, spanA.front().first, std::ranges::less{},
        &CodepointInfo::byte_offset);
    auto codepointItB = std::ranges::lower_bound(
        codepoint_infos, spanB.front().first, std::ranges::less{},
        &CodepointInfo::byte_offset);
    GlyphClusterSection clusterSection{};
    clusterSection.cluster = spanA.front().first;
    clusterSection.codepoint_infos = {codepointItA, codepointItB};
    clusterSection.geometries = {spanA.front().second, spanA.size()};
    return clusterSection;
  };
  auto sectionsHead = clusterSpans | std::views::pairwise |
                      std::views::transform(projectAdjacent);

  auto projectTrailing = [this](const std::span<BufferPair> &clusterSpan)
  {
    auto codepointIt = std::ranges::lower_bound(
        codepoint_infos, clusterSpan.front().first, std::ranges::less{},
        &CodepointInfo::byte_offset);
    GlyphClusterSection clusterSection{};
    clusterSection.cluster = clusterSpan.front().first;
    clusterSection.codepoint_infos = {codepointIt, codepoint_infos.end()};
    clusterSection.geometries = {clusterSpan.front().second,
                                 clusterSpan.size()};
    return clusterSection;
  };
  auto sectionsTail = clusterSpans | std::views::reverse | std::views::take(1) |
                      std::views::transform(projectTrailing);

  std::ranges::concat_view sectionsAll{sectionsHead, sectionsTail};
  return {std::from_range, sectionsAll};
}

void CopypassTaskVisitor::operator()(CopyGlyphAtlas) const
{
  SDL_GPUTextureTransferInfo source{};
  source.transfer_buffer = app->atlas_transfer.get();
  source.pixels_per_row = AppState::ATLAS_SIDE;
  source.rows_per_layer = AppState::ATLAS_SIDE;
  SDL_GPUTextureRegion destination{};
  destination.texture = app->atlas_texture.get();
  destination.w = AppState::ATLAS_SIDE;
  destination.h = AppState::ATLAS_SIDE;
  destination.d = 1;
  SDL_UploadToGPUTexture(copypass, &source, &destination, false);
}

UniqueFreetypeGlyph AppState::freetype_load_glyph(hb_codepoint_t glyphid,
                                                  int phase)
{
  FT_Error errcode{};
  FT_Face face = hb_ft_font_get_ft_face(harfbuzz_font.get());
  errcode =
      FT_Load_Glyph(face, glyphid, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
  if (errcode)
  {
    std::string errorMsg{"FT_Load_Glyph failed for glyph: "};
    throw std::runtime_error{errorMsg + std::to_string(glyphid)};
  }
  FT_Glyph glyph{};
  errcode = FT_Get_Glyph(face->glyph, &glyph);
  if (errcode)
  {
    std::string errorMsg{"FT_Get_Glyph failed for glyph: "};
    throw std::runtime_error{errorMsg + std::to_string(glyphid)};
  }
  const FT_Vector origin{static_cast<FT_Pos>(phase) * 16, 0};
  errcode = FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, &origin, 1);
  if (errcode)
  {
    std::string errorMsg{"FT_Glyph_To_Bitmap failed for glyph: "};
    throw std::runtime_error{errorMsg + std::to_string(glyphid)};
  }
  return UniqueFreetypeGlyph{glyph};
}

const FT_Glyph_Metrics &AppState::freetype_codepoint_metrics(FT_ULong codepoint)
{
  FT_Error errcode{};
  FT_Face face = hb_ft_font_get_ft_face(harfbuzz_font.get());
  errcode = FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT | FT_LOAD_NO_SCALE);
  if (errcode)
  {
    std::string errorMsg{"FT_Load_Char failed for glyph: "};
    throw std::runtime_error{errorMsg + std::to_string(codepoint)};
  }
  return face->glyph->metrics;
}

bool GlyphSequencer::text_reshape(std::string_view textView)
{
  hb_buffer_reset(harfbuzz_buffer.get());
  hb_buffer_add_utf8(harfbuzz_buffer.get(), textView.data(),
                     static_cast<int>(textView.size()), 0,
                     static_cast<int>(textView.size()));
  hb_buffer_guess_segment_properties(harfbuzz_buffer.get());
  hb_shape(app->harfbuzz_font.get(), harfbuzz_buffer.get(), nullptr, 0);
  codepoint_infos = {std::from_range, emitCodepointInfo(textView)};
  glyph_steps = {std::from_range, emit_glyph_steps()};
  return verify_atlas_cache();
}

void GlyphSequencer::text_reflow()
{
  glyph_geometries.clear();
  populate_glyph_geometries();
  cluster_sections = make_cluster_sections();
  std::tie(x_min, x_max, y_min, y_max) = text_measure({}, {}, false);
  queue_vertex_copypass();
}

void CopypassTaskVisitor::operator()(CopySelectionAreas task) const
{
  SDL_GPUTransferBufferLocation source{
      .transfer_buffer = app->selection_transfer.get(), .offset = 0};
  SDL_GPUBufferRegion destination{};
  destination.buffer = app->selection_instances.get();
  destination.size = task.num_selections * sizeof(RectInstanceData);
  SDL_UploadToGPUBuffer(copypass, &source, &destination, false);
}

void AppState::selection_reflow()
{
  std::uint32_t numSelections = static_cast<std::uint32_t>(
      std::ranges::distance(selection_slots | std::views::join));
  verify_selection_capacity(numSelections);
  transfer_selection_areas();
  copypass_tasks.push_back(CopySelectionAreas{numSelections});
}

void AppState::verify_selection_capacity(std::uint32_t numSelections)
{
  std::uint32_t instancesSize = numSelections * sizeof(RectInstanceData);
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

void AppState::transfer_selection_areas()
{
  UniqueGPUMappedBuffer selectionMapped =
      mapGPUTransferBuffer(device_ctx.get(), selection_transfer.get(), true);
  auto selectionAreas =
      selection_slots | std::views::join | std::views::enumerate;
  for (const auto &[idx, selectionArea] : selectionAreas)
  {
    auto [rectX, rectY, rectW, rectH] = selectionArea;
    RectInstanceData rectInstance{};
    rectInstance.rect_position[0] = static_cast<float>(rectX);
    rectInstance.rect_position[1] = -1.0f * static_cast<float>(rectY + rectH);
    rectInstance.rect_size[0] = static_cast<float>(rectW);
    rectInstance.rect_size[1] = static_cast<float>(rectH);
    rectInstance.uv_rect[2] = 1.0f;
    rectInstance.uv_rect[3] = 1.0f;
    std::memcpy(static_cast<RectInstanceData *>(selectionMapped.get()) + idx,
                &rectInstance, sizeof(RectInstanceData));
  }
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
  if (not SDL_InitSubSystem(SDL_INIT_VIDEO))
  {
    SDL_Log("SDL_Init_Subsystem failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  if (FT_Init_FreeType(&globalFTLibrary))
  {
    SDL_Log("FT_Init_FreeType failed!");
    return SDL_APP_FAILURE;
  }
  try
  {
    AppState *app = new AppState{};
    *appstate = app;
    app->instantiate(globalFTLibrary);
    bool atlasInserted{};
    for (GlyphSequencer &sequencer : app->glyph_sequencers)
      atlasInserted |= sequencer.text_reshape(app->caption_string);
    if (atlasInserted)
    {
      app->pack_atlas();
      app->copypass_tasks.push_back(CopyGlyphAtlas{});
    }
    for (GlyphSequencer &sequencer : app->glyph_sequencers)
      sequencer.text_reflow();
    auto [windowW, windowH] = getWindowSizeInPixels(app->main_window.get());
    app->caption_x = static_cast<double>(windowW) * 0.5 -
                     app->caption_sequencer.measured_w() * 0.5;
    app->caption_y = static_cast<double>(windowH) * 0.5 -
                     app->caption_sequencer.measured_h() * 0.5;
    return SDL_APP_CONTINUE;
  }
  catch (const std::runtime_error &e)
  {
    SDL_Log("[AppInit] %s", e.what());
    return SDL_APP_FAILURE;
  }
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
  AppState *app = static_cast<AppState *>(appstate);
  delete app;
  if (globalFTLibrary)
  {
    FT_Done_FreeType(globalFTLibrary);
    globalFTLibrary = nullptr;
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
  AppState *app = static_cast<AppState *>(appstate);
  return app->iterate();
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
  AppState *app = static_cast<AppState *>(appstate);
  return app->handle_event(event);
}
