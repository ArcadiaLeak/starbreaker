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

struct alignas(16) RectImageUniform
{
  float uv_rect[4];
  float rect_size[2];
  float rect_position[2];
  float viewport[2];
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

FT_BitmapGlyph AsBitmapGlyph(FT_Glyph freetypeGlyph)
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

UniqueHarfbuzzFont CreateHarfbuzzFont(FT_Library library, const char *filepath,
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

UniqueGPUDevice CreateGPUDevice()
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

UniqueWindow CreateWindow(const char *title, int width, int height)
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

UniqueWindowForGPUDevice ClaimWindowForGPUDevice(SDL_GPUDevice *device,
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

void SetWindowMinimumSize(SDL_Window *window, int minW, int minH)
{
  if (not SDL_SetWindowMinimumSize(window, minW, minH))
  {
    std::string errorMsg{"SDL_SetWindowMinimumSize failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
}

std::array<int, 2> GetWindowSizeInPixels(SDL_Window *window)
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

UniqueFileContent LoadFile(const char *filepath, size_t *datasize)
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

const std::array blit_shader = std::to_array<std::uint8_t>({
#embed "blit.spv"
});
const std::array rect_image_shader = std::to_array<std::uint8_t>({
#embed "rect_image.spv"
});
const std::array rect_instance_shader = std::to_array<std::uint8_t>({
#embed "rect_instance.spv"
});
const std::array selection_box_shader = std::to_array<std::uint8_t>({
#embed "selection_box.spv"
});
const std::array solid_shader = std::to_array<std::uint8_t>({
#embed "solid.spv"
});
const std::array tint_shader = std::to_array<std::uint8_t>({
#embed "coverage_tint.spv"
});
const std::array difference_blend_shader = std::to_array<std::uint8_t>({
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

UniqueGPUSampler CreateCommonGPUSampler(SDL_GPUDevice *device)
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
UniqueGPUTexture CreateGPUTexture(SDL_GPUDevice *device,
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

UniqueGPUTransferBuffer CreateGPUTransferBuffer(SDL_GPUDevice *device,
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
CreateGPUGraphicsPipeline(SDL_GPUDevice *device,
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

struct RectInstanceVertexInfo
{
  std::array<SDL_GPUVertexAttribute, 3> vertex_attributes;
  SDL_GPUVertexBufferDescription vertex_buffer_descriptions;
  SDL_GPUVertexInputState vertex_input_state;
  RectInstanceVertexInfo();
};

RectInstanceVertexInfo::RectInstanceVertexInfo()
{
  std::get<0>(vertex_attributes).location = 0;
  std::get<0>(vertex_attributes).buffer_slot = 0;
  std::get<0>(vertex_attributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<0>(vertex_attributes).offset =
      offsetof(RectInstanceData, rect_position);
  std::get<1>(vertex_attributes).location = 1;
  std::get<1>(vertex_attributes).buffer_slot = 0;
  std::get<1>(vertex_attributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<1>(vertex_attributes).offset = offsetof(RectInstanceData, rect_size);
  std::get<2>(vertex_attributes).location = 2;
  std::get<2>(vertex_attributes).buffer_slot = 0;
  std::get<2>(vertex_attributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  std::get<2>(vertex_attributes).offset = offsetof(RectInstanceData, uv_rect);

  vertex_buffer_descriptions.slot = 0;
  vertex_buffer_descriptions.pitch = sizeof(RectInstanceData);
  vertex_buffer_descriptions.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  vertex_buffer_descriptions.instance_step_rate = 0;

  vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_descriptions;
  vertex_input_state.num_vertex_buffers = 1;
  vertex_input_state.vertex_attributes = vertex_attributes.data();
  vertex_input_state.num_vertex_attributes =
      static_cast<std::uint32_t>(vertex_attributes.size());
}

UniqueGPUGraphicsPipeline
CreateGlyphGPUGraphicsPipeline(SDL_GPUDevice *device,
                               SDL_GPUTextureFormat target_format)
{
  SDL_GPUShaderCreateInfo vertex_shader_info{};
  vertex_shader_info.code = rect_instance_shader.data();
  vertex_shader_info.code_size = rect_instance_shader.size();
  vertex_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vertex_shader_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vertex_shader_info.num_uniform_buffers = 1;
  UniqueGPUShader vertex_shader = createGPUShader(device, vertex_shader_info);

  SDL_GPUShaderCreateInfo fragment_shader_info{};
  fragment_shader_info.code = tint_shader.data();
  fragment_shader_info.code_size = tint_shader.size();
  fragment_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fragment_shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fragment_shader_info.num_samplers = 1;
  fragment_shader_info.num_uniform_buffers = 1;
  UniqueGPUShader fragment_shader =
      createGPUShader(device, fragment_shader_info);

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = target_format;
  color_target.blend_state.src_color_blendfactor =
      SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  color_target.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  color_target.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo pipeline_target_info{};
  pipeline_target_info.color_target_descriptions = &color_target;
  pipeline_target_info.num_color_targets = 1;

  SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.vertex_shader = vertex_shader.get();
  pipeline_info.fragment_shader = fragment_shader.get();
  RectInstanceVertexInfo rect_instance_vertex_info{};
  pipeline_info.vertex_input_state =
      rect_instance_vertex_info.vertex_input_state;
  pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipeline_info.target_info = pipeline_target_info;

  return CreateGPUGraphicsPipeline(device, &pipeline_info);
}

UniqueGPUGraphicsPipeline
CreateSelectionGPUGraphicsPipeline(SDL_GPUDevice *device)
{
  SDL_GPUShaderCreateInfo vertex_shader_info{};
  vertex_shader_info.code = rect_instance_shader.data();
  vertex_shader_info.code_size = rect_instance_shader.size();
  vertex_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vertex_shader_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vertex_shader_info.num_uniform_buffers = 1;
  UniqueGPUShader vertex_shader = createGPUShader(device, vertex_shader_info);

  SDL_GPUShaderCreateInfo fragment_shader_info{};
  fragment_shader_info.code = selection_box_shader.data();
  fragment_shader_info.code_size = selection_box_shader.size();
  fragment_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fragment_shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fragment_shader_info.num_uniform_buffers = 1;
  UniqueGPUShader fragment_shader =
      createGPUShader(device, fragment_shader_info);

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  color_target.blend_state.src_color_blendfactor =
      SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  color_target.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  color_target.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo pipeline_target_info{};
  pipeline_target_info.color_target_descriptions = &color_target;
  pipeline_target_info.num_color_targets = 1;

  SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.vertex_shader = vertex_shader.get();
  pipeline_info.fragment_shader = fragment_shader.get();
  RectInstanceVertexInfo rect_instance_vertex_info{};
  pipeline_info.vertex_input_state =
      rect_instance_vertex_info.vertex_input_state;
  pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipeline_info.target_info = pipeline_target_info;

  return CreateGPUGraphicsPipeline(device, &pipeline_info);
}

UniqueGPUGraphicsPipeline
CreateSolidRectGPUGraphicsPipeline(SDL_GPUDevice *device,
                                   SDL_GPUTextureFormat target_format)
{
  SDL_GPUShaderCreateInfo vertex_shader_info{};
  vertex_shader_info.code = rect_image_shader.data();
  vertex_shader_info.code_size = rect_image_shader.size();
  vertex_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vertex_shader_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vertex_shader_info.num_uniform_buffers = 1;
  UniqueGPUShader vertex_shader = createGPUShader(device, vertex_shader_info);

  SDL_GPUShaderCreateInfo fragment_shader_info{};
  fragment_shader_info.code = solid_shader.data();
  fragment_shader_info.code_size = solid_shader.size();
  fragment_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fragment_shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fragment_shader_info.num_uniform_buffers = 1;
  UniqueGPUShader fragment_shader =
      createGPUShader(device, fragment_shader_info);

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = target_format;
  color_target.blend_state.src_color_blendfactor =
      SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  color_target.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  color_target.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo pipeline_target_info{};
  pipeline_target_info.color_target_descriptions = &color_target;
  pipeline_target_info.num_color_targets = 1;

  SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.vertex_shader = vertex_shader.get();
  pipeline_info.fragment_shader = fragment_shader.get();
  pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipeline_info.target_info = pipeline_target_info;

  return CreateGPUGraphicsPipeline(device, &pipeline_info);
}

UniqueGPUGraphicsPipeline
CreateTexturedRectGPUGraphicsPipeline(SDL_GPUDevice *device,
                                      SDL_GPUTextureFormat target_format)
{
  SDL_GPUShaderCreateInfo vertex_shader_info{};
  vertex_shader_info.code = rect_image_shader.data();
  vertex_shader_info.code_size = rect_image_shader.size();
  vertex_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vertex_shader_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vertex_shader_info.num_uniform_buffers = 1;
  UniqueGPUShader vertex_shader = createGPUShader(device, vertex_shader_info);

  SDL_GPUShaderCreateInfo fragment_shader_info{};
  fragment_shader_info.code = blit_shader.data();
  fragment_shader_info.code_size = blit_shader.size();
  fragment_shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  fragment_shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fragment_shader_info.num_samplers = 1;
  UniqueGPUShader fragment_shader =
      createGPUShader(device, fragment_shader_info);

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = target_format;
  color_target.blend_state.src_color_blendfactor =
      SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  color_target.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  color_target.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo pipeline_target_info{};
  pipeline_target_info.color_target_descriptions = &color_target;
  pipeline_target_info.num_color_targets = 1;

  SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.vertex_shader = vertex_shader.get();
  pipeline_info.fragment_shader = fragment_shader.get();
  pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipeline_info.target_info = pipeline_target_info;

  return CreateGPUGraphicsPipeline(device, &pipeline_info);
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

UniqueGPUComputePipeline CreateDifferenceBlendGPUPipeline(SDL_GPUDevice *device)
{
  SDL_GPUComputePipelineCreateInfo pipeline_info{};
  pipeline_info.code = difference_blend_shader.data();
  pipeline_info.code_size = difference_blend_shader.size();
  pipeline_info.entrypoint = "main";
  pipeline_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  pipeline_info.num_readonly_storage_textures = 2;
  pipeline_info.num_readwrite_storage_textures = 1;
  pipeline_info.threadcount_x = 8;
  pipeline_info.threadcount_y = 8;
  pipeline_info.threadcount_z = 1;
  return createGPUComputePipeline(device, &pipeline_info);
}

struct GPUCommandBufferSubmitter
{
  void operator()(SDL_GPUCommandBuffer *command_buf) const;
};
using EngagedGPUCommandBuffer =
    std::unique_ptr<SDL_GPUCommandBuffer, GPUCommandBufferSubmitter>;
void GPUCommandBufferSubmitter::operator()(
    SDL_GPUCommandBuffer *command_buf) const
{
  if (not SDL_SubmitGPUCommandBuffer(command_buf))
    SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
}

struct GPUCommandBufferCanceller
{
  void operator()(SDL_GPUCommandBuffer *command_buf) const;
};
using DismissedGPUCommandBuffer =
    std::unique_ptr<SDL_GPUCommandBuffer, GPUCommandBufferCanceller>;
void GPUCommandBufferCanceller::operator()(
    SDL_GPUCommandBuffer *command_buf) const
{
  if (not SDL_CancelGPUCommandBuffer(command_buf))
    SDL_Log("SDL_CancelGPUCommandBuffer failed: %s", SDL_GetError());
}

EngagedGPUCommandBuffer acquireGPUCommandBuffer(SDL_GPUDevice *device)
{
  SDL_GPUCommandBuffer *command_buf = SDL_AcquireGPUCommandBuffer(device);
  if (not command_buf)
  {
    std::string errorMsg{"SDL_AcquireGPUCommandBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return EngagedGPUCommandBuffer{command_buf};
}

struct GPURenderPassDeleter
{
  void operator()(SDL_GPURenderPass *render_pass) const;
};
using UniqueGPURenderPass =
    std::unique_ptr<SDL_GPURenderPass, GPURenderPassDeleter>;
void GPURenderPassDeleter::operator()(SDL_GPURenderPass *render_pass) const
{
  SDL_EndGPURenderPass(render_pass);
}

UniqueGPURenderPass
BeginGPURenderPass(SDL_GPUCommandBuffer *command_buf,
                   const SDL_GPUColorTargetInfo *target_info)
{
  SDL_GPURenderPass *render_pass =
      SDL_BeginGPURenderPass(command_buf, target_info, 1, nullptr);
  return UniqueGPURenderPass{render_pass};
}

SDL_GPUTexture *
WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer *command_buf,
                                  SDL_Window *window, std::uint32_t *texture_w,
                                  std::uint32_t *texture_h)
{
  SDL_GPUTexture *texture{};
  if (not SDL_WaitAndAcquireGPUSwapchainTexture(command_buf, window, &texture,
                                                texture_w, texture_h))
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
  void operator()(SDL_GPUComputePass *compute_pass) const;
};
using UniqueGPUComputePass =
    std::unique_ptr<SDL_GPUComputePass, GPUComputePassDeleter>;
void GPUComputePassDeleter::operator()(SDL_GPUComputePass *compute_pass) const
{
  SDL_EndGPUComputePass(compute_pass);
}

UniqueGPUComputePass
BeginGPUComputePass(SDL_GPUCommandBuffer *command_buf,
                    const SDL_GPUStorageTextureReadWriteBinding *textureBinding)
{
  SDL_GPUComputePass *compute_pass =
      SDL_BeginGPUComputePass(command_buf, textureBinding, 1, nullptr, 0);
  return UniqueGPUComputePass{compute_pass};
}

struct UTextDeleter
{
  void operator()(UText *utextPtr) const { utext_close(utextPtr); }
};
using UniqueUText = std::unique_ptr<UText, UTextDeleter>;

UniqueUText UTextOpenUTF8(std::string_view strView)
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
std::generator<CodepointInfo> EmitCodepointInfo(std::string_view strView)
{
  UniqueUText uText = UTextOpenUTF8(strView);
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
  void CopyBitmap(const GlyphAtlasItem &cacheItem);
  void CopyBitmapRow(std::uint32_t row);
};

struct GlyphCaretStep
{
  double pen_position, pen_advance;
  double horz_offset, vert_offset, horz_position;
  int subpixel_phase;
};

struct AppState;
struct GlyphShapeItem
{
  AppState *app;
  UniqueHarfbuzzBuffer harfbuzz_buffer;
  std::string shape_str;

  double translate_x = 0, translate_y = 0;
  double x_min = 0, x_max = 0;
  double y_min = 0, y_max = 0;
  double GetMeasuredW() const noexcept { return x_max - x_min; }
  double GetMeasuredH() const noexcept { return y_max - y_min; }
  std::array<double, 4> GetBoundingBox() const noexcept;

  std::vector<GlyphGeometry> glyph_geometries;
  std::vector<GlyphCaretStep> glyph_steps;
  std::vector<GlyphClusterSection> cluster_sections;
  std::vector<CodepointInfo> codepoint_infos;

  void PopulateGlyphGeometries();
  std::generator<GlyphCaretStep> EmitGlyphSteps();
  std::vector<GlyphClusterSection> MakeClusterSections();

  UniqueGPUBuffer instance_buffer;
  UniqueGPUTransferBuffer instance_transfer;
  std::uint32_t instance_capacity = 0;

  void VerifyInstanceCapacity();
  void TransferInstanceData();
  void QueueVertexCopypass();
  bool VerifyAtlasCache();

  bool TextReshape();
  void TextReflow();
  std::array<double, 4> TextMeasure(std::pair<std::uint32_t, double>,
                                    std::pair<std::uint32_t, double>,
                                    bool) const;
  std::uint32_t FindCodepointOffset(double screenX) const;
  std::pair<std::uint32_t, double>
  FindClusterOffset(std::uint32_t textOffset) const;
};

struct CopyGlyphAtlas
{
};
struct CopyGlyphVertices
{
  GlyphShapeItem *shape_item;
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

struct GPURenderSequencer
{
  AppState *app;
  SDL_GPUCommandBuffer *command_buffer;
  UniqueGPURenderPass render_pass;

  std::uint32_t swapchain_w, swapchain_h;
  SDL_GPUTexture *swapchain_texture;

  void DrawScene(SDL_GPUTexture *texture, SDL_GPUGraphicsPipeline *pipeline);
  void VerifyStorageTextures();
  void DrawSelectedAreas();
  void BlendSelectedAreas();
  void DrawProcessedImage();
  void DrawMarqueeTool();
};

struct SelectionDragState
{
  std::uint32_t selection_idx, shape_item_idx;
  std::array<double, 2> mouse_segment;
};

struct SelectionEventHandler
{
  AppState *app;
  std::optional<SelectionDragState> drag_state;

  std::uint32_t FindUnoccupiedSlot();
  void CopyDragState();

  void HandleMouseButtonLDown(const SDL_MouseButtonEvent &event);
  void HandleMouseButtonLUp(const SDL_MouseButtonEvent &event);
  void HandleMouseMotion(const SDL_MouseMotionEvent &event);
};

struct AppState
{
  UniqueHarfbuzzFont harfbuzz_font;

  UniqueGPUDevice device_ctx;
  UniqueWindow main_window;
  UniqueWindowForGPUDevice main_window_for_gpu_device;

  static constexpr std::uint32_t ATLAS_SIDE = 1024;
  static constexpr std::uint32_t ATLAS_PADDING = 1;
  UniqueGPUTransferBuffer atlas_transfer;
  UniqueGPUTexture atlas_texture;
  GlyphAtlasCache atlas_cache;

  const char *SHAPE_STR_ONE = "Hello figure kerning Text";
  GlyphShapeItem caption_one{.app = this, .shape_str = SHAPE_STR_ONE};
  const char *SHAPE_STR_TWO = "The quick brown fox jumps over the lazy dog.";
  GlyphShapeItem caption_two{.app = this, .shape_str = SHAPE_STR_TWO};
  std::vector<GlyphShapeItem *> shape_items{&caption_one, &caption_two};

  UniqueFreetypeGlyph GetFtGlyph(hb_codepoint_t glyphid, int phase);
  const FT_Glyph_Metrics &GetFtMetricsForCodepoint(FT_ULong codepoint);

  UniqueGPUTexture offscreen_texture;
  UniqueGPUTexture selection_texture;
  UniqueGPUTexture processed_texture;
  std::uint32_t offscreen_w = 0, offscreen_h = 0;
  double haitch_height = 0;

  SelectionEventHandler selection_handler{this};
  std::vector<std::optional<std::array<double, 4>>> selection_slots;
  bool selection_areas_flag = false;

  UniqueGPUSampler sampler;
  UniqueGPUGraphicsPipeline swapchain_glyph_pipeline;
  UniqueGPUGraphicsPipeline offscreen_glyph_pipeline;
  UniqueGPUGraphicsPipeline selection_pipeline;
  UniqueGPUGraphicsPipeline textured_rect_pipeline;
  UniqueGPUGraphicsPipeline solid_rect_pipeline;
  UniqueGPUComputePipeline difference_blend_pipeline;

  UniqueGPUBuffer selection_instances;
  UniqueGPUTransferBuffer selection_transfer;
  std::uint32_t selection_instances_capacity = 0;

  void VerifySelectionBuffersCapacity(std::uint32_t num_selections);
  void WriteSelectionAreasForTransfer();
  void ReflowSelections();

  std::vector<CopypassTask> copypass_tasks;

  SDL_AppResult Iterate() noexcept;
  SDL_AppResult HandleEvent(SDL_Event *event) noexcept;

  void Instantiate(FT_Library library);
  void RecordGPUCopies(SDL_GPUCommandBuffer *command_buffer);
  void RecordGPURender(SDL_GPUCommandBuffer *command_buffer);
  void PackAtlas();
};

void AppState::Instantiate(FT_Library library)
{
  device_ctx = CreateGPUDevice();
  main_window = CreateWindow("Hello GPU", static_cast<int>(INITIAL_WINDOW_W),
                             static_cast<int>(INITIAL_WINDOW_H));
  SetWindowMinimumSize(main_window.get(), static_cast<int>(INITIAL_WINDOW_W),
                       static_cast<int>(INITIAL_WINDOW_H));
  main_window_for_gpu_device =
      ClaimWindowForGPUDevice(device_ctx.get(), main_window.get());
  harfbuzz_font = CreateHarfbuzzFont(library, "assets/DejaVuSans.ttf", 10 * 64,
                                     getWindowDisplayScale(main_window.get()));
  hb_ft_font_set_load_flags(harfbuzz_font.get(),
                            FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);

  GPUTextureCreateInfo texture_info{.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                                    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                    .texture_w = ATLAS_SIDE,
                                    .texture_h = ATLAS_SIDE};
  atlas_texture = CreateGPUTexture(device_ctx.get(), texture_info);
  atlas_transfer =
      CreateGPUTransferBuffer(device_ctx.get(), ATLAS_SIDE * ATLAS_SIDE);
  sampler = CreateCommonGPUSampler(device_ctx.get());

  selection_pipeline = CreateSelectionGPUGraphicsPipeline(device_ctx.get());
  difference_blend_pipeline =
      CreateDifferenceBlendGPUPipeline(device_ctx.get());
  offscreen_glyph_pipeline = CreateGlyphGPUGraphicsPipeline(
      device_ctx.get(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
  SDL_GPUTextureFormat swapchain_texture_format =
      SDL_GetGPUSwapchainTextureFormat(device_ctx.get(), main_window.get());
  swapchain_glyph_pipeline = CreateGlyphGPUGraphicsPipeline(
      device_ctx.get(), swapchain_texture_format);
  textured_rect_pipeline = CreateTexturedRectGPUGraphicsPipeline(
      device_ctx.get(), swapchain_texture_format);
  solid_rect_pipeline = CreateSolidRectGPUGraphicsPipeline(
      device_ctx.get(), swapchain_texture_format);

  const FT_Glyph_Metrics &haitch_metrics = GetFtMetricsForCodepoint(72);
  haitch_height = static_cast<double>(haitch_metrics.height) / 64.0;
}

void AppState::RecordGPUCopies(SDL_GPUCommandBuffer *command_buffer)
{
  if (copypass_tasks.empty())
    return;
  UniqueGPUCopyPass copypass{SDL_BeginGPUCopyPass(command_buffer)};
  for (const CopypassTask &copypassTask : copypass_tasks)
    copypassTask.visit(CopypassTaskVisitor{this, copypass.get()});
  copypass_tasks.clear();
}

void CopypassTaskVisitor::operator()(CopyGlyphVertices task) const
{
  const GlyphShapeItem &si = *task.shape_item;
  SDL_GPUTransferBufferLocation src{
      .transfer_buffer = si.instance_transfer.get(), .offset = 0};
  std::uint32_t buffer_size = static_cast<std::uint32_t>(
      hb_buffer_get_length(si.harfbuzz_buffer.get()) *
      sizeof(RectInstanceData));
  SDL_GPUBufferRegion dst{
      .buffer = si.instance_buffer.get(), .offset = 0, .size = buffer_size};
  SDL_UploadToGPUBuffer(copypass, &src, &dst, false);
}

void AppState::RecordGPURender(SDL_GPUCommandBuffer *command_buffer)
{
  GPURenderSequencer render_sequencer{this, command_buffer};
  render_sequencer.swapchain_texture = WaitAndAcquireGPUSwapchainTexture(
      command_buffer, main_window.get(), &render_sequencer.swapchain_w,
      &render_sequencer.swapchain_h);
  if (not render_sequencer.swapchain_texture)
    return;
  auto num_selections =
      std::ranges::distance(selection_slots | std::views::join);
  if (num_selections == 0)
  {
    render_sequencer.DrawScene(render_sequencer.swapchain_texture,
                               swapchain_glyph_pipeline.get());
    render_sequencer.DrawMarqueeTool();
    render_sequencer.render_pass.reset();
  }
  else
  {
    render_sequencer.VerifyStorageTextures();
    render_sequencer.DrawScene(offscreen_texture.get(),
                               offscreen_glyph_pipeline.get());
    render_sequencer.render_pass.reset();
    render_sequencer.DrawSelectedAreas();
    render_sequencer.render_pass.reset();
    render_sequencer.BlendSelectedAreas();
    render_sequencer.DrawProcessedImage();
    render_sequencer.DrawMarqueeTool();
    render_sequencer.render_pass.reset();
  }
}

void GPURenderSequencer::DrawMarqueeTool()
{
  SDL_BindGPUGraphicsPipeline(render_pass.get(),
                              app->solid_rect_pipeline.get());
  RectImageUniform vertex_uniform{};
  vertex_uniform.uv_rect[2] = 1.0f;
  vertex_uniform.uv_rect[3] = 1.0f;
  vertex_uniform.rect_position[0] = 300.0f;
  vertex_uniform.rect_position[1] = 200.0f;
  vertex_uniform.rect_size[0] = 200.0f;
  vertex_uniform.rect_size[1] = 200.0f;
  vertex_uniform.viewport[0] = static_cast<float>(swapchain_w);
  vertex_uniform.viewport[1] = static_cast<float>(swapchain_h);
  SDL_PushGPUVertexUniformData(command_buffer, 0, &vertex_uniform,
                               sizeof(RectImageUniform));
  struct alignas(16) SolidUniform
  {
    float tint[4];
  };
  SolidUniform fragment_uniform{.tint = {1.0f, 1.0f, 1.0f, 0.5f}};
  SDL_PushGPUFragmentUniformData(command_buffer, 0, &fragment_uniform,
                                 sizeof(SolidUniform));
  SDL_DrawGPUPrimitives(render_pass.get(), 4, 1, 0, 0);
}

void GPURenderSequencer::DrawProcessedImage()
{
  SDL_GPUColorTargetInfo target_info{};
  target_info.texture = swapchain_texture;
  target_info.load_op = SDL_GPU_LOADOP_DONT_CARE;
  target_info.store_op = SDL_GPU_STOREOP_STORE;
  render_pass = BeginGPURenderPass(command_buffer, &target_info);

  SDL_BindGPUGraphicsPipeline(render_pass.get(),
                              app->textured_rect_pipeline.get());
  SDL_GPUTextureSamplerBinding sampler_binding{};
  sampler_binding.texture = app->processed_texture.get();
  sampler_binding.sampler = app->sampler.get();
  SDL_BindGPUFragmentSamplers(render_pass.get(), 0, &sampler_binding, 1);

  RectImageUniform vertex_uniform{};
  vertex_uniform.uv_rect[2] = 1.0f;
  vertex_uniform.uv_rect[3] = 1.0f;
  vertex_uniform.rect_size[0] = static_cast<float>(app->offscreen_w);
  vertex_uniform.rect_size[1] = static_cast<float>(app->offscreen_h);
  vertex_uniform.rect_position[1] =
      static_cast<float>(swapchain_h) - static_cast<float>(app->offscreen_h);
  vertex_uniform.viewport[0] = static_cast<float>(swapchain_w);
  vertex_uniform.viewport[1] = static_cast<float>(swapchain_h);
  SDL_PushGPUVertexUniformData(command_buffer, 0, &vertex_uniform,
                               sizeof(RectImageUniform));

  SDL_DrawGPUPrimitives(render_pass.get(), 4, 1, 0, 0);
}

void GPURenderSequencer::DrawScene(SDL_GPUTexture *scene_texture,
                                   SDL_GPUGraphicsPipeline *pipeline)
{
  SDL_GPUColorTargetInfo color_target{};
  color_target.texture = scene_texture;
  color_target.clear_color = {0.08f, 0.08f, 0.10f, 1.0f};
  color_target.load_op = SDL_GPU_LOADOP_CLEAR;
  color_target.store_op = SDL_GPU_STOREOP_STORE;
  render_pass = BeginGPURenderPass(command_buffer, &color_target);

  SDL_GPUViewport viewport{.x = 0,
                           .y = 0,
                           .w = static_cast<float>(swapchain_w),
                           .h = static_cast<float>(swapchain_h),
                           .min_depth = 0.0f,
                           .max_depth = 1.0f};
  SDL_SetGPUViewport(render_pass.get(), &viewport);
  SDL_Rect scissor{0, 0, static_cast<int>(swapchain_w),
                   static_cast<int>(swapchain_h)};
  SDL_SetGPUScissor(render_pass.get(), &scissor);

  SDL_BindGPUGraphicsPipeline(render_pass.get(), pipeline);
  SDL_GPUTextureSamplerBinding sampler_binding{};
  sampler_binding.texture = app->atlas_texture.get();
  sampler_binding.sampler = app->sampler.get();
  SDL_BindGPUFragmentSamplers(render_pass.get(), 0, &sampler_binding, 1);

  struct alignas(16) BlitUniform
  {
    float tint[4];
  };
  BlitUniform blit{.tint = {1.0f, 1.0f, 1.0f, 1.0f}};
  SDL_PushGPUFragmentUniformData(command_buffer, 0, &blit, sizeof(BlitUniform));

  struct alignas(16) RectUniform
  {
    float viewport[2];
    float translate[2];
  };
  for (const GlyphShapeItem *shape_item : app->shape_items)
  {
    SDL_GPUBufferBinding instance_binding{shape_item->instance_buffer.get(), 0};
    SDL_BindGPUVertexBuffers(render_pass.get(), 0, &instance_binding, 1);

    RectUniform rect_uni{};
    rect_uni.viewport[0] = static_cast<float>(swapchain_w);
    rect_uni.viewport[1] = static_cast<float>(swapchain_h);
    double vertical_translate = static_cast<double>(swapchain_h) -
                                shape_item->GetMeasuredH() -
                                shape_item->translate_y;
    rect_uni.translate[0] =
        static_cast<float>(std::trunc(shape_item->translate_x));
    rect_uni.translate[1] = static_cast<float>(std::trunc(vertical_translate));

    SDL_PushGPUVertexUniformData(command_buffer, 0, &rect_uni,
                                 sizeof(RectUniform));

    std::uint32_t num_instances =
        hb_buffer_get_length(shape_item->harfbuzz_buffer.get());
    SDL_DrawGPUPrimitives(render_pass.get(), 4, num_instances, 0, 0);
  }
}

void GPURenderSequencer::VerifyStorageTextures()
{
  bool offscreen_enough_w = app->offscreen_w >= swapchain_w;
  bool offscreen_enough_h = app->offscreen_h >= swapchain_h;
  if (offscreen_enough_w && offscreen_enough_h)
    return;
  if (not offscreen_enough_w)
    app->offscreen_w =
        std::max(1u << std::bit_width(swapchain_w), app->offscreen_w * 2u);
  if (not offscreen_enough_h)
    app->offscreen_h =
        std::max(1u << std::bit_width(swapchain_h), app->offscreen_h * 2u);
  GPUTextureCreateInfo offscreen_info{};
  offscreen_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  offscreen_info.texture_w = app->offscreen_w;
  offscreen_info.texture_h = app->offscreen_h;
  offscreen_info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  app->offscreen_texture =
      CreateGPUTexture(app->device_ctx.get(), offscreen_info);
  GPUTextureCreateInfo selection_info{};
  selection_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  selection_info.texture_w = app->offscreen_w;
  selection_info.texture_h = app->offscreen_h;
  selection_info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  app->selection_texture =
      CreateGPUTexture(app->device_ctx.get(), selection_info);
  GPUTextureCreateInfo processed_info{};
  processed_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  processed_info.texture_w = app->offscreen_w;
  processed_info.texture_h = app->offscreen_h;
  processed_info.usage =
      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
  app->processed_texture =
      CreateGPUTexture(app->device_ctx.get(), processed_info);
}

void GPURenderSequencer::DrawSelectedAreas()
{
  SDL_GPUColorTargetInfo color_target{};
  color_target.texture = app->selection_texture.get();
  color_target.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
  color_target.load_op = SDL_GPU_LOADOP_CLEAR;
  color_target.store_op = SDL_GPU_STOREOP_STORE;
  render_pass = BeginGPURenderPass(command_buffer, &color_target);

  SDL_GPUViewport viewport{.x = 0,
                           .y = 0,
                           .w = static_cast<float>(swapchain_w),
                           .h = static_cast<float>(swapchain_h),
                           .min_depth = 0.0f,
                           .max_depth = 1.0f};
  SDL_SetGPUViewport(render_pass.get(), &viewport);
  SDL_Rect scissor{0, 0, static_cast<int>(swapchain_w),
                   static_cast<int>(swapchain_h)};
  SDL_SetGPUScissor(render_pass.get(), &scissor);

  SDL_BindGPUGraphicsPipeline(render_pass.get(), app->selection_pipeline.get());
  SDL_GPUBufferBinding instance_binding{app->selection_instances.get(), 0};
  SDL_BindGPUVertexBuffers(render_pass.get(), 0, &instance_binding, 1);

  struct alignas(16) RectUniform
  {
    float viewport[2];
    float translate[2];
  };
  RectUniform rect_uni{};
  rect_uni.viewport[0] = static_cast<float>(swapchain_w);
  rect_uni.viewport[1] = static_cast<float>(swapchain_h);
  std::array<float, 2> tx{0, static_cast<float>(swapchain_h)};
  std::memcpy(&rect_uni.translate, tx.data(), sizeof(RectUniform::translate));
  SDL_PushGPUVertexUniformData(command_buffer, 0, &rect_uni,
                               sizeof(RectUniform));

  struct alignas(16) BoxUniform
  {
    float blend_color[4];
    float radius;
  };
  BoxUniform box_uniform{.blend_color = {1.0f, 1.0f, 1.0f, 1.0f},
                         .radius = 0.2f};
  SDL_PushGPUFragmentUniformData(command_buffer, 0, &box_uniform,
                                 sizeof(BoxUniform));

  std::uint32_t num_instances = static_cast<std::uint32_t>(
      std::ranges::distance(app->selection_slots | std::views::join));
  SDL_DrawGPUPrimitives(render_pass.get(), 4, num_instances, 0, 0);
}

void GPURenderSequencer::BlendSelectedAreas()
{
  SDL_GPUStorageTextureReadWriteBinding texture_rw{
      .texture = app->processed_texture.get()};
  UniqueGPUComputePass compute_pass =
      BeginGPUComputePass(command_buffer, &texture_rw);

  SDL_BindGPUComputePipeline(compute_pass.get(),
                             app->difference_blend_pipeline.get());
  std::array<SDL_GPUTexture *, 2> storage_textures{
      app->offscreen_texture.get(), app->selection_texture.get()};
  SDL_BindGPUComputeStorageTextures(
      compute_pass.get(), 0, storage_textures.data(), storage_textures.size());

  std::uint32_t ceiled_x{(swapchain_w + 7) / 8};
  std::uint32_t ceiled_y{(swapchain_h + 7) / 8};
  SDL_DispatchGPUCompute(compute_pass.get(), ceiled_x, ceiled_y, 1);
}

SDL_AppResult AppState::Iterate() noexcept
{
  try
  {
    auto num_selections =
        std::ranges::distance(selection_slots | std::views::join);
    if (selection_areas_flag && num_selections > 0)
    {
      ReflowSelections();
      selection_areas_flag = false;
    }
    EngagedGPUCommandBuffer command_buffer =
        acquireGPUCommandBuffer(device_ctx.get());
    RecordGPUCopies(command_buffer.get());
    RecordGPURender(command_buffer.get());
    return SDL_APP_CONTINUE;
  }
  catch (const std::runtime_error &e)
  {
    SDL_Log("[AppIterate] %s", e.what());
    return SDL_APP_FAILURE;
  }
}

std::array<double, 4> GlyphShapeItem::GetBoundingBox() const noexcept
{
  static constexpr double PADDING_X = 1;
  static constexpr double PADDING_Y = 4;
  std::array<double, 4> bounding_box{};
  std::get<0>(bounding_box) = translate_x + x_min - PADDING_X;
  std::get<1>(bounding_box) = translate_y + y_max - PADDING_Y;
  std::get<2>(bounding_box) = GetMeasuredW() + PADDING_X * 2;
  std::get<3>(bounding_box) = GetMeasuredH() + PADDING_Y * 2;
  return bounding_box;
}

std::uint32_t SelectionEventHandler::FindUnoccupiedSlot()
{
  auto CanOccupySlot = [](const std::optional<std::array<double, 4>> &slot)
  { return not slot.has_value(); };
  auto selection_it = std::ranges::find_if(app->selection_slots, CanOccupySlot);
  if (selection_it == app->selection_slots.end())
    selection_it =
        app->selection_slots.insert(app->selection_slots.end(), std::nullopt);
  selection_it->emplace();
  return static_cast<std::uint32_t>(
      std::distance(app->selection_slots.begin(), selection_it));
}

void SelectionEventHandler::CopyDragState()
{
  auto &[selectionX, selectionY, selectionW, selectionH] =
      *app->selection_slots[drag_state->selection_idx];
  const auto &[mouseA, mouseB] = drag_state->mouse_segment;
  selectionX = std::min(mouseA, mouseB);
  constexpr double MIN_SELECTION_W = 2.0;
  selectionW = std::max(std::abs(mouseA - mouseB), MIN_SELECTION_W);
  auto [captionX, captionY, captionW, captionH] =
      app->shape_items[drag_state->shape_item_idx]->GetBoundingBox();
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

void SelectionEventHandler::HandleMouseButtonLDown(
    const SDL_MouseButtonEvent &event)
{
  for (std::optional<std::array<double, 4>> &selection : app->selection_slots)
    selection.reset();
  for (const auto &[idx, shapeItem] : app->shape_items | std::views::enumerate)
    if (isPointInside({event.x, event.y}, shapeItem->GetBoundingBox()))
    {
      drag_state.emplace();
      drag_state->shape_item_idx = static_cast<std::uint32_t>(idx);
      drag_state->selection_idx = FindUnoccupiedSlot();
      std::get<0>(drag_state->mouse_segment) = event.x;
      std::get<1>(drag_state->mouse_segment) = event.x;
      CopyDragState();
      break;
    }
}

std::uint32_t GlyphShapeItem::FindCodepointOffset(double screenX) const
{
  std::size_t captionOffset{shape_str.size()};
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
GlyphShapeItem::FindClusterOffset(std::uint32_t textOffset) const
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

void SelectionEventHandler::HandleMouseButtonLUp(
    const SDL_MouseButtonEvent &event)
{
  if (not drag_state.has_value())
    return;
  const GlyphShapeItem *shapeItem =
      app->shape_items[drag_state->shape_item_idx];
  auto [captionX, captionY, captionW, captionH] = shapeItem->GetBoundingBox();
  std::array<double, 2> screenX{drag_state->mouse_segment};
  std::ranges::sort(screenX);

  double startX = std::get<0>(screenX) - captionX;
  std::uint32_t codepointStart = shapeItem->FindCodepointOffset(startX);
  std::pair<std::uint32_t, double> clusterStart =
      shapeItem->FindClusterOffset(codepointStart);

  double finalX = std::get<1>(screenX) - captionX;
  std::uint32_t codepointFinal = shapeItem->FindCodepointOffset(finalX);
  std::pair<std::uint32_t, double> clusterFinal =
      shapeItem->FindClusterOffset(codepointFinal);

  auto [measureL, measureR, measureT, measureB] =
      shapeItem->TextMeasure(clusterStart, clusterFinal, true);
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
    selectionX = shapeItem->translate_x + measureL - PADDING_X;
    selectionY = shapeItem->translate_y + shapeItem->GetMeasuredH() + measureT -
                 PADDING_Y;
    selectionW = measureW + PADDING_X * 2;
    selectionH = measureH + PADDING_Y * 2;
  }

  app->selection_areas_flag = true;
  drag_state.reset();
}

void SelectionEventHandler::HandleMouseMotion(const SDL_MouseMotionEvent &event)
{
  if (not drag_state.has_value())
    return;
  const GlyphShapeItem *shapeItem =
      app->shape_items[drag_state->shape_item_idx];
  auto [captionX, captionY, captionW, captionH] = shapeItem->GetBoundingBox();
  float captionMin = static_cast<float>(captionX);
  float captionMax = static_cast<float>(captionX + captionW);
  std::get<1>(drag_state->mouse_segment) =
      std::clamp(event.x, captionMin, captionMax);
  CopyDragState();
}

SDL_AppResult AppState::HandleEvent(SDL_Event *event) noexcept
{
  switch (event->type)
  {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (event->button.button == 1)
      selection_handler.HandleMouseButtonLDown(event->button);
    return SDL_APP_CONTINUE;

  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (event->button.button == 1)
      selection_handler.HandleMouseButtonLUp(event->button);
    return SDL_APP_CONTINUE;

  case SDL_EVENT_MOUSE_MOTION:
    selection_handler.HandleMouseMotion(event->motion);
    return SDL_APP_CONTINUE;

  case SDL_EVENT_WINDOW_RESIZED:
    caption_one.translate_x = 0.5 * static_cast<double>(event->window.data1) -
                              caption_one.GetMeasuredW() * 0.5;
    caption_one.translate_y =
        5.0 / 12.0 * static_cast<double>(event->window.data2) -
        caption_one.GetMeasuredH() * 0.5;
    caption_two.translate_x = 0.5 * static_cast<double>(event->window.data1) -
                              caption_two.GetMeasuredW() * 0.5;
    caption_two.translate_y =
        7.0 / 12.0 * static_cast<double>(event->window.data2) -
        caption_two.GetMeasuredH() * 0.5;
    return SDL_APP_CONTINUE;

  case SDL_EVENT_QUIT:
    return SDL_APP_SUCCESS;

  default:
    return SDL_APP_CONTINUE;
  }
}

bool GlyphShapeItem::VerifyAtlasCache()
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
        app->GetFtGlyph(glyphInfo.codepoint, step.subpixel_phase);
    const FT_Bitmap &bitmap = AsBitmapGlyph(ftglyph.get())->bitmap;
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

void AppState::PackAtlas()
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
    return AsBitmapGlyph(atlasItem.ftglyph.get())->bitmap.rows;
  };
  std::ranges::sort(atlasColumnItems, std::ranges::greater{}, atlasGlyphH);
  std::uint32_t accumulatedX{};
  for (AtlasColumnItem &columnItem : atlasColumnItems)
  {
    FT_BitmapGlyph bitmapGlyph =
        AsBitmapGlyph(atlas_cache.at(columnItem.atlas_label).ftglyph.get());
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
        AsBitmapGlyph(atlasItem.ftglyph.get())->bitmap.rows;
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
    atlasCopier.CopyBitmap(cacheItem);
}

void GlyphAtlasCopier::CopyBitmap(const GlyphAtlasItem &cacheItem)
{
  atlas_position = &cacheItem.atlas_position;
  bitmap = &AsBitmapGlyph(cacheItem.ftglyph.get())->bitmap;
  for (std::uint32_t row = 0; row < bitmap->rows; ++row)
    CopyBitmapRow(row);
}

void GlyphAtlasCopier::CopyBitmapRow(std::uint32_t row)
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

void GlyphShapeItem::VerifyInstanceCapacity()
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
      CreateGPUTransferBuffer(app->device_ctx.get(), instance_capacity);
}

void GlyphShapeItem::TransferInstanceData()
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

void GlyphShapeItem::QueueVertexCopypass()
{
  VerifyInstanceCapacity();
  TransferInstanceData();
  app->copypass_tasks.push_back(CopyGlyphVertices{this});
}

constexpr double MAX_SAFE_INTEGER = std::ldexp(1.0, 53) - 1;
constexpr double MIN_SAFE_INTEGER = -1.0 * MAX_SAFE_INTEGER;

std::array<double, 4>
GlyphShapeItem::TextMeasure(std::pair<std::uint32_t, double> start,
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
      FT_BitmapGlyph bitmapGlyph = AsBitmapGlyph(atlasItem.ftglyph.get());
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

std::generator<GlyphCaretStep> GlyphShapeItem::EmitGlyphSteps()
{
  unsigned int buffer_len = hb_buffer_get_length(harfbuzz_buffer.get());
  std::span<const hb_glyph_position_t> positions{
      hb_buffer_get_glyph_positions(harfbuzz_buffer.get(), nullptr),
      buffer_len};
  GlyphCaretStep step{};
  auto GetPositionInPixels = [](hb_position_t pos) -> double
  {
    constexpr double GLYPH_POSITION_UNIT = 1.0 / 64.0;
    return static_cast<double>(pos) * GLYPH_POSITION_UNIT;
  };
  for (const hb_glyph_position_t &position : positions)
  {
    step.horz_offset = GetPositionInPixels(position.x_offset);
    step.horz_position = step.pen_position + step.horz_offset;
    constexpr int SUBPIXEL_PHASE_COUNT = 4;
    double phase = (step.horz_position - std::trunc(step.horz_position)) *
                   static_cast<double>(SUBPIXEL_PHASE_COUNT);
    step.subpixel_phase = static_cast<int>(std::round(phase));
    step.vert_offset = GetPositionInPixels(position.y_offset);
    step.pen_advance = GetPositionInPixels(position.x_advance);
    co_yield step;
    step.pen_position += step.pen_advance;
  }
}

void GlyphShapeItem::PopulateGlyphGeometries()
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
        AsBitmapGlyph(app->atlas_cache.at(atlasLabel).ftglyph.get());
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

std::vector<GlyphClusterSection> GlyphShapeItem::MakeClusterSections()
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

UniqueFreetypeGlyph AppState::GetFtGlyph(hb_codepoint_t glyphid, int phase)
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

const FT_Glyph_Metrics &AppState::GetFtMetricsForCodepoint(FT_ULong codepoint)
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

bool GlyphShapeItem::TextReshape()
{
  if (not harfbuzz_buffer)
    harfbuzz_buffer = UniqueHarfbuzzBuffer{hb_buffer_create()};
  else
    hb_buffer_reset(harfbuzz_buffer.get());
  hb_buffer_add_utf8(harfbuzz_buffer.get(), shape_str.data(),
                     static_cast<int>(shape_str.size()), 0,
                     static_cast<int>(shape_str.size()));
  hb_buffer_guess_segment_properties(harfbuzz_buffer.get());
  hb_shape(app->harfbuzz_font.get(), harfbuzz_buffer.get(), nullptr, 0);
  codepoint_infos = {std::from_range, EmitCodepointInfo(shape_str)};
  glyph_steps = {std::from_range, EmitGlyphSteps()};
  return VerifyAtlasCache();
}

void GlyphShapeItem::TextReflow()
{
  glyph_geometries.clear();
  PopulateGlyphGeometries();
  cluster_sections = MakeClusterSections();
  std::tie(x_min, x_max, y_min, y_max) = TextMeasure({}, {}, false);
  QueueVertexCopypass();
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

void AppState::ReflowSelections()
{
  std::uint32_t num_selections = static_cast<std::uint32_t>(
      std::ranges::distance(selection_slots | std::views::join));
  VerifySelectionBuffersCapacity(num_selections);
  WriteSelectionAreasForTransfer();
  copypass_tasks.push_back(CopySelectionAreas{num_selections});
}

void AppState::VerifySelectionBuffersCapacity(std::uint32_t num_selections)
{
  std::uint32_t instances_size = num_selections * sizeof(RectInstanceData);
  if (instances_size <= selection_instances_capacity)
    return;
  selection_instances_capacity = std::max(1u << std::bit_width(instances_size),
                                          selection_instances_capacity * 2u);
  SDL_GPUBufferCreateInfo instances_info{.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
                                         .size = selection_instances_capacity};
  selection_instances = createGPUBuffer(device_ctx.get(), &instances_info);
  selection_transfer =
      CreateGPUTransferBuffer(device_ctx.get(), selection_instances_capacity);
}

void AppState::WriteSelectionAreasForTransfer()
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
    app->Instantiate(globalFTLibrary);
    bool atlasInserted{};
    for (GlyphShapeItem *shapeItem : app->shape_items)
      atlasInserted |= shapeItem->TextReshape();
    if (atlasInserted)
    {
      app->PackAtlas();
      app->copypass_tasks.push_back(CopyGlyphAtlas{});
    }
    for (GlyphShapeItem *shapeItem : app->shape_items)
      shapeItem->TextReflow();
    auto [windowW, windowH] = GetWindowSizeInPixels(app->main_window.get());
    app->caption_one.translate_x = 0.5 * static_cast<double>(windowW) -
                                   app->caption_one.GetMeasuredW() * 0.5;
    app->caption_one.translate_y = 5.0 / 12.0 * static_cast<double>(windowH) -
                                   app->caption_one.GetMeasuredH() * 0.5;
    app->caption_two.translate_x = 0.5 * static_cast<double>(windowW) -
                                   app->caption_two.GetMeasuredW() * 0.5;
    app->caption_two.translate_y = 7.0 / 12.0 * static_cast<double>(windowH) -
                                   app->caption_two.GetMeasuredH() * 0.5;
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
  return app->Iterate();
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
  AppState *app = static_cast<AppState *>(appstate);
  return app->HandleEvent(event);
}
