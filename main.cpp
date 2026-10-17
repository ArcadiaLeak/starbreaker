#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
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

struct GlyphInstanceData {
  float rect_pos[2];
  float rect_size[2];
  float uv_rect[4];
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

UniqueWindow createWindow() {
  SDL_Window *window =
      SDL_CreateWindow("Hello GPU", INITIAL_WINDOW_WIDTH, INITIAL_WINDOW_HEIGHT,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
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

void setWindowMinimumSize(SDL_Window *window) {
  if (not SDL_SetWindowMinimumSize(window, INITIAL_WINDOW_WIDTH,
                                   INITIAL_WINDOW_HEIGHT)) {
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

UniqueGPUSampler createCaptionGPUSampler(SDL_GPUDevice *device) {
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
  SDL_GPUTextureUsageFlags usage;
  std::uint32_t texture_width;
  std::uint32_t texture_height;
};
UniqueGPUTexture createGPUTexture(SDL_GPUDevice *device,
                                  GPUTextureCreateInfo createInfo) {
  SDL_GPUTextureCreateInfo textureInfo{};
  textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
  textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
  textureInfo.usage = createInfo.usage;
  textureInfo.width = createInfo.texture_width;
  textureInfo.height = createInfo.texture_height;
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

UniqueGPUBuffer createGPUVertexBuffer(SDL_GPUDevice *device,
                                      std::uint32_t bufferSize) {
  SDL_GPUBufferCreateInfo buf_info = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
                                      .size = bufferSize};
  SDL_GPUBuffer *instance_buffer = SDL_CreateGPUBuffer(device, &buf_info);
  if (not instance_buffer) {
    std::string errorMsg{"SDL_CreateGPUBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUBuffer{instance_buffer, GPUBufferDeleter{device}};
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
createCaptionGPUGraphicsPipeline(SDL_GPUDevice *device, SDL_Window *window) {
  GPUShaderCreateInfo vertexInfo{.filepath = "caption.vert.spv",
                                 .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                 .num_samplers = 0,
                                 .num_uniform_buffers = 1};
  UniqueGPUShader vertexShader = createGPUShader(device, vertexInfo);

  GPUShaderCreateInfo fragmentInfo{.filepath = "caption.frag.spv",
                                   .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 1,
                                   .num_uniform_buffers = 1};
  UniqueGPUShader fragmentShader = createGPUShader(device, fragmentInfo);

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
  std::get<0>(vertexAttributes).offset = offsetof(GlyphInstanceData, rect_pos);
  std::get<1>(vertexAttributes).location = 1;
  std::get<1>(vertexAttributes).buffer_slot = 0;
  std::get<1>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  std::get<1>(vertexAttributes).offset = offsetof(GlyphInstanceData, rect_size);
  std::get<2>(vertexAttributes).location = 2;
  std::get<2>(vertexAttributes).buffer_slot = 0;
  std::get<2>(vertexAttributes).format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  std::get<2>(vertexAttributes).offset = offsetof(GlyphInstanceData, uv_rect);

  SDL_GPUVertexBufferDescription vertexBufferDescriptions{};
  vertexBufferDescriptions.slot = 0;
  vertexBufferDescriptions.pitch = sizeof(GlyphInstanceData);
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

UniqueGPURenderPass beginGPURenderPass(SDL_GPUCommandBuffer *commandBuffer,
                                       SDL_GPUTexture *texture,
                                       SDL_FColor clearColor) {
  SDL_GPUColorTargetInfo colorTarget{.texture = texture,
                                     .clear_color = clearColor,
                                     .load_op = SDL_GPU_LOADOP_CLEAR,
                                     .store_op = SDL_GPU_STOREOP_STORE};
  SDL_GPURenderPass *renderPass =
      SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, nullptr);
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

class AppState {
private:
  struct FrameCompositor {
    AppState *app;

    UniqueGPUBuffer instance_buffer;
    UniqueGPUTransferBuffer instance_transfer;
    EngagedGPUCommandBuffer command_buffer;

    void instantiate();
    void copy_instance_data();
    void issue_copy_pass();
    void issue_render_pass();
  };

  struct GlyphCombinator {
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

    void instantiate();
    void glyphs_shape(std::string_view textView);
    bool glyphs_fill_cache();
    void pack_atlas();
    void texturize_atlas();
    void measure_text();
  };

  UniqueHarfbuzzFont harfbuzz_font;

  UniqueGPUDevice device_ctx;
  UniqueWindow main_window;
  UniqueWindowForGPUDevice main_window_for_gpu_device;

  UniqueGPUSampler caption_sampler;
  UniqueGPUGraphicsPipeline caption_pipeline;

  std::string caption_string = "Hello world kerning Text";
  int caption_x = 0, caption_y = 0;
  GlyphCombinator caption_combinator{this};

  UniqueFreetypeGlyph load_freetype_glyph(hb_codepoint_t codepoint);

public:
  AppState(FT_Library library);

  SDL_AppResult iterate() noexcept;
  SDL_AppResult handle_event(SDL_Event *event) noexcept;

  void caption_text_combine();
};

AppState::AppState(FT_Library library) {
  device_ctx = createGPUDevice();
  main_window = createWindow();
  main_window_for_gpu_device =
      claimWindowForGPUDevice(device_ctx.get(), main_window.get());
  harfbuzz_font = createHarfbuzzFont(library, "assets/DejaVuSans.ttf", 10 * 64,
                                     getWindowDisplayScale(main_window.get()));
  caption_sampler = createCaptionGPUSampler(device_ctx.get());
  caption_pipeline =
      createCaptionGPUGraphicsPipeline(device_ctx.get(), main_window.get());
  caption_combinator.instantiate();
}

void AppState::FrameCompositor::instantiate() {
  command_buffer = acquireGPUCommandBuffer(app->device_ctx.get());
  std::uint32_t bufferSize{
      static_cast<std::uint32_t>(sizeof(GlyphInstanceData))};
  instance_buffer = createGPUVertexBuffer(app->device_ctx.get(), bufferSize);
  instance_transfer =
      createGPUTransferBuffer(app->device_ctx.get(), bufferSize);
}

void AppState::FrameCompositor::copy_instance_data() {
  UniqueGPUMappedBuffer mappedBuffer{mapGPUTransferBuffer(
      app->device_ctx.get(), instance_transfer.get(), true)};
  GlyphInstanceData instanceData{.uv_rect = {0.0f, 0.0f, 1.0f, 1.0f}};
  instanceData.rect_pos[0] = static_cast<float>(app->caption_x);
  instanceData.rect_pos[1] = static_cast<float>(app->caption_y);
  instanceData.rect_size[0] =
      static_cast<float>(app->caption_combinator.measured_width);
  instanceData.rect_size[1] =
      static_cast<float>(app->caption_combinator.measured_height);
  std::uint32_t bufferSize{
      static_cast<std::uint32_t>(sizeof(GlyphInstanceData))};
  std::memcpy(mappedBuffer.get(), &instanceData, bufferSize);
}

void AppState::FrameCompositor::issue_copy_pass() {
  UniqueGPUCopyPass copyPass{SDL_BeginGPUCopyPass(command_buffer.get())};
  SDL_GPUTransferBufferLocation src{.transfer_buffer = instance_transfer.get(),
                                    .offset = 0};
  std::uint32_t bufferSize{
      static_cast<std::uint32_t>(sizeof(GlyphInstanceData))};
  SDL_GPUBufferRegion dst{
      .buffer = instance_buffer.get(), .offset = 0, .size = bufferSize};
  SDL_UploadToGPUBuffer(copyPass.get(), &src, &dst, true);
}

void AppState::FrameCompositor::issue_render_pass() {
  std::uint32_t swapchainWidth{}, swapchainHeight{};
  SDL_GPUTexture *swapchainTexture = waitAndAcquireGPUSwapchainTexture(
      command_buffer.get(), app->main_window.get(), &swapchainWidth,
      &swapchainHeight);
  if (not swapchainTexture)
    return;

  UniqueGPURenderPass renderPass =
      beginGPURenderPass(command_buffer.get(), swapchainTexture,
                         SDL_FColor{0.08f, 0.08f, 0.10f, 1.0f});
  SDL_BindGPUGraphicsPipeline(renderPass.get(), app->caption_pipeline.get());

  SDL_GPUBufferBinding instanceBinding{instance_buffer.get(), 0};
  SDL_BindGPUVertexBuffers(renderPass.get(), 0, &instanceBinding, 1);

  struct QuadUniforms {
    float viewport[2];
  };
  QuadUniforms uniforms{};
  uniforms.viewport[0] = static_cast<float>(swapchainWidth);
  uniforms.viewport[1] = static_cast<float>(swapchainHeight);
  SDL_PushGPUVertexUniformData(command_buffer.get(), 0, &uniforms,
                               sizeof(QuadUniforms));

  struct BlitUniforms {
    float tint[4];
  };
  BlitUniforms blit{};
  blit.tint[0] = 1.0f;
  blit.tint[1] = 1.0f;
  blit.tint[2] = 1.0f;
  blit.tint[3] = 1.0f;
  SDL_PushGPUFragmentUniformData(command_buffer.get(), 0, &blit,
                                 sizeof(BlitUniforms));

  SDL_GPUTextureSamplerBinding samplerBinding{};
  samplerBinding.texture = app->caption_combinator.combined_texture.get();
  samplerBinding.sampler = app->caption_sampler.get();
  SDL_BindGPUFragmentSamplers(renderPass.get(), 0, &samplerBinding, 1);

  SDL_DrawGPUPrimitives(renderPass.get(), 4, 1, 0, 0);
}

SDL_AppResult AppState::iterate() noexcept {
  try {
    FrameCompositor frame_compositor{this};
    frame_compositor.instantiate();
    frame_compositor.copy_instance_data();
    frame_compositor.issue_copy_pass();
    frame_compositor.issue_render_pass();
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
    caption_x = (event->window.data1 - caption_combinator.measured_width) / 2;
    caption_y = (event->window.data2 - caption_combinator.measured_height) / 2;
    return SDL_APP_CONTINUE;
  } else
    return SDL_APP_CONTINUE;
}

void AppState::GlyphCombinator::instantiate() {
  GPUTextureCreateInfo textureInfo{.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                   .texture_width = ATLAS_SIDE,
                                   .texture_height = ATLAS_SIDE};
  atlas_texture = createGPUTexture(app->device_ctx.get(), textureInfo);
  atlas_transfer =
      createGPUTransferBuffer(app->device_ctx.get(), ATLAS_SIDE * ATLAS_SIDE);
}

void AppState::GlyphCombinator::glyphs_shape(std::string_view textView) {
  harfbuzz_buffer.reset(hb_buffer_create());
  hb_buffer_add_utf8(harfbuzz_buffer.get(), textView.data(),
                     static_cast<int>(textView.size()), 0,
                     static_cast<int>(textView.size()));
  hb_buffer_guess_segment_properties(harfbuzz_buffer.get());
  hb_shape(app->harfbuzz_font.get(), harfbuzz_buffer.get(), nullptr, 0);
}

bool AppState::GlyphCombinator::glyphs_fill_cache() {
  std::span<hb_glyph_info_t> glyphInfos(
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr),
      hb_buffer_get_length(harfbuzz_buffer.get()));
  bool isAtlasDirty = false;
  for (const hb_glyph_info_t &info : glyphInfos) {
    if (glyph_cache.contains(info.codepoint))
      continue;
    UniqueFreetypeGlyph glyph = app->load_freetype_glyph(info.codepoint);
    const FT_Bitmap &bitmap = asBitmapGlyph(glyph.get())->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0)
      continue;
    glyph_cache[info.codepoint] =
        GlyphCacheItem{std::move(glyph), std::make_unique<GlyphAtlasInfo>()};
    isAtlasDirty = true;
  }
  return isAtlasDirty;
}

void AppState::GlyphCombinator::pack_atlas() {
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
  auto copyBitmapRow = [&atlasTransfer](const CacheEntryView &entryView,
                                        std::uint32_t row) -> void {
    const FT_Bitmap &bitmap = asBitmapGlyph(entryView.freetype_glyph)->bitmap;
    unsigned char *source = bitmap.buffer + row * bitmap.pitch;
    std::uint32_t atlasOffset =
        entryView.atlas->y * ATLAS_SIDE + row * ATLAS_SIDE + entryView.atlas->x;
    unsigned char *transferStart =
        static_cast<unsigned char *>(atlasTransfer.get());
    std::memcpy(transferStart + atlasOffset, source, bitmap.width);
  };
  for (const CacheEntryView &entryView : entriesView) {
    const FT_Bitmap &bitmap = asBitmapGlyph(entryView.freetype_glyph)->bitmap;
    for (std::uint32_t row = 0; row < bitmap.rows; ++row)
      copyBitmapRow(entryView, row);
  }
}

void AppState::GlyphCombinator::texturize_atlas() {}

void AppState::GlyphCombinator::measure_text() {}

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

void AppState::caption_text_combine() {
  caption_combinator.glyphs_shape(caption_string);
  bool isAtlasDirty = caption_combinator.glyphs_fill_cache();
  if (isAtlasDirty) {
    caption_combinator.pack_atlas();
    caption_combinator.texturize_atlas();
  }
  caption_combinator.measure_text();
  caption_x = (INITIAL_WINDOW_WIDTH - caption_combinator.measured_width) / 2;
  caption_y = (INITIAL_WINDOW_HEIGHT - caption_combinator.measured_height) / 2;
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
    app->caption_text_combine();
    return SDL_APP_CONTINUE;
  } catch (const std::runtime_error &e) {
    SDL_Log("[AppInit] %s", e.what());
    return SDL_APP_FAILURE;
  }
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
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
