#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include <freetype/freetype.h>
#include <freetype/ftglyph.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <vulkan/vulkan.h>

struct Vertex {
  float position[2];
  float uv[2];
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
                     SDL_GPUTransferBuffer *transferBuffer) {
  void *mappedBuffer = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
  if (not mappedBuffer) {
    std::string errorMsg{"SDL_MapGPUTransferBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return UniqueGPUMappedBuffer{
      mappedBuffer, GPUTransferBufferUnmapper{device, transferBuffer}};
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

  SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.vertex_shader = vertexShader.get();
  pipelineInfo.fragment_shader = fragmentShader.get();
  pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipelineInfo.target_info = targetInfo;

  return createGPUGraphicsPipeline(device, &pipelineInfo);
}

UniqueGPUGraphicsPipeline
createGlyphGPUGraphicsPipeline(SDL_GPUDevice *device) {
  GPUShaderCreateInfo vertexInfo{.filepath = "caption.vert.spv",
                                 .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                 .num_samplers = 0,
                                 .num_uniform_buffers = 1};
  UniqueGPUShader vertexShader = createGPUShader(device, vertexInfo);

  GPUShaderCreateInfo fragmentInfo{.filepath = "glyph.frag.spv",
                                   .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 1,
                                   .num_uniform_buffers = 0};
  UniqueGPUShader fragmentShader = createGPUShader(device, fragmentInfo);

  SDL_GPUColorTargetBlendState blendState{};
  blendState.enable_blend = true;
  blendState.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  blendState.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  blendState.color_blend_op = SDL_GPU_BLENDOP_MAX;
  blendState.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  blendState.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  blendState.alpha_blend_op = SDL_GPU_BLENDOP_MAX;
  blendState.color_write_mask = SDL_GPU_COLORCOMPONENT_R;

  SDL_GPUColorTargetDescription colorTargetDescription{};
  colorTargetDescription.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
  colorTargetDescription.blend_state = blendState;

  SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.vertex_shader = vertexShader.get();
  pipelineInfo.fragment_shader = fragmentShader.get();
  pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipelineInfo.target_info.color_target_descriptions = &colorTargetDescription;
  pipelineInfo.target_info.num_color_targets = 1;

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
  struct GlyphCompositor {
    struct GlyphCacheItem {
      UniqueFreetypeGlyph freetypeGlyph;
      UniqueGPUTexture texture;
    };
    using GlyphCache = std::map<hb_codepoint_t, GlyphCacheItem>;
    AppState *app;

    UniqueHarfbuzzBuffer harfbuzz_buffer;
    GlyphCache glyph_cache;
    UniqueGPUTexture combined_texture;
    int combined_width = 0, combined_height = 0;

    void glyphs_shape(std::string_view textView);
    void glyphs_texturize();
    void glyphs_combine();
  };

  UniqueHarfbuzzFont harfbuzz_font;

  UniqueGPUDevice device_ctx;
  UniqueWindow main_window;
  UniqueWindowForGPUDevice main_window_for_gpu_device;

  UniqueGPUSampler caption_sampler;
  UniqueGPUGraphicsPipeline caption_pipeline;
  UniqueGPUGraphicsPipeline glyph_pipeline;

  std::string caption_string = "Hello world kerning Text";
  int caption_x = 0, caption_y = 0;
  GlyphCompositor caption_compositor{this};

  UniqueFreetypeGlyph load_freetype_glyph(hb_codepoint_t codepoint);

public:
  AppState(FT_Library library);

  SDL_AppResult iterate() noexcept;
  SDL_AppResult handle_event(SDL_Event *event) noexcept;

  void caption_text_compose();
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
  glyph_pipeline = createGlyphGPUGraphicsPipeline(device_ctx.get());
}

SDL_AppResult AppState::iterate() noexcept {
  EngagedGPUCommandBuffer commandBuffer =
      acquireGPUCommandBuffer(device_ctx.get());
  std::uint32_t swapchainWidth{}, swapchainHeight{};
  SDL_GPUTexture *swapchainTexture =
      waitAndAcquireGPUSwapchainTexture(commandBuffer.get(), main_window.get(),
                                        &swapchainWidth, &swapchainHeight);
  if (not swapchainTexture)
    return SDL_APP_CONTINUE;

  UniqueGPURenderPass renderPass =
      beginGPURenderPass(commandBuffer.get(), swapchainTexture,
                         SDL_FColor{0.08f, 0.08f, 0.10f, 1.0f});
  SDL_BindGPUGraphicsPipeline(renderPass.get(), caption_pipeline.get());
  struct QuadUniforms {
    float viewport[2];
    float rect_pos[2];
    float rect_size[2];
  };
  struct BlitUniforms {
    float tint[4];
  };

  QuadUniforms uniforms{};
  uniforms.viewport[0] = static_cast<float>(swapchainWidth);
  uniforms.viewport[1] = static_cast<float>(swapchainHeight);
  uniforms.rect_pos[0] = static_cast<float>(caption_x);
  uniforms.rect_pos[1] = static_cast<float>(caption_y);
  uniforms.rect_size[0] = static_cast<float>(caption_compositor.combined_width);
  uniforms.rect_size[1] =
      static_cast<float>(caption_compositor.combined_height);
  SDL_PushGPUVertexUniformData(commandBuffer.get(), 0, &uniforms,
                               sizeof(QuadUniforms));

  BlitUniforms blit{};
  blit.tint[0] = 1.0f;
  blit.tint[1] = 1.0f;
  blit.tint[2] = 1.0f;
  blit.tint[3] = 1.0f;
  SDL_PushGPUFragmentUniformData(commandBuffer.get(), 0, &blit,
                                 sizeof(BlitUniforms));

  SDL_GPUTextureSamplerBinding samplerBinding{};
  samplerBinding.texture = caption_compositor.combined_texture.get();
  samplerBinding.sampler = caption_sampler.get();
  SDL_BindGPUFragmentSamplers(renderPass.get(), 0, &samplerBinding, 1);

  SDL_DrawGPUPrimitives(renderPass.get(), 4, 1, 0, 0);
  return SDL_APP_CONTINUE;
}

SDL_AppResult AppState::handle_event(SDL_Event *event) noexcept {
  if (event->type == SDL_EVENT_QUIT)
    return SDL_APP_SUCCESS;
  else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
           event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    SDL_Log("x: %f, y: %f", event->button.x, event->button.y);
    return SDL_APP_CONTINUE;
  } else if (event->type == SDL_EVENT_WINDOW_RESIZED) {
    caption_x = (event->window.data1 - caption_compositor.combined_width) / 2;
    caption_y = (event->window.data2 - caption_compositor.combined_height) / 2;
    return SDL_APP_CONTINUE;
  } else
    return SDL_APP_CONTINUE;
}

void AppState::GlyphCompositor::glyphs_shape(std::string_view textView) {
  harfbuzz_buffer.reset(hb_buffer_create());
  hb_buffer_add_utf8(harfbuzz_buffer.get(), textView.data(),
                     static_cast<int>(textView.size()), 0,
                     static_cast<int>(textView.size()));
  hb_buffer_guess_segment_properties(harfbuzz_buffer.get());
  hb_shape(app->harfbuzz_font.get(), harfbuzz_buffer.get(), nullptr, 0);
}

void AppState::GlyphCompositor::glyphs_texturize() {
  std::span<hb_glyph_info_t> glyphInfos(
      hb_buffer_get_glyph_infos(harfbuzz_buffer.get(), nullptr),
      hb_buffer_get_length(harfbuzz_buffer.get()));
  GlyphCache previousGlyphCache{};
  previousGlyphCache.swap(glyph_cache);
  std::map<hb_codepoint_t, UniqueGPUTransferBuffer> transferBuffers{};
  for (const hb_glyph_info_t &glyphInfo : glyphInfos) {
    GlyphCache::node_type cacheNode =
        previousGlyphCache.extract(glyphInfo.codepoint);
    GlyphCache::insert_return_type cacheInsert =
        glyph_cache.insert(std::move(cacheNode));
    if (cacheInsert.inserted)
      continue;
    UniqueFreetypeGlyph freetypeGlyph =
        app->load_freetype_glyph(glyphInfo.codepoint);
    const FT_Bitmap &bitmap = asBitmapGlyph(freetypeGlyph.get())->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0)
      continue;
    GPUTextureCreateInfo textureInfo{.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                     .texture_width = bitmap.width,
                                     .texture_height = bitmap.rows};
    glyph_cache[glyphInfo.codepoint] =
        GlyphCacheItem{std::move(freetypeGlyph),
                       createGPUTexture(app->device_ctx.get(), textureInfo)};
    std::uint32_t transferSize =
        static_cast<std::uint32_t>(bitmap.width * bitmap.rows);
    transferBuffers[glyphInfo.codepoint] =
        createGPUTransferBuffer(app->device_ctx.get(), transferSize);
    UniqueGPUMappedBuffer mappedBuffer = mapGPUTransferBuffer(
        app->device_ctx.get(), transferBuffers[glyphInfo.codepoint].get());
    for (std::uint32_t row = 0; row < bitmap.rows; ++row) {
      const uint8_t *source = bitmap.buffer + row * std::abs(bitmap.pitch);
      uint8_t *destination =
          static_cast<uint8_t *>(mappedBuffer.get()) + row * bitmap.width;
      std::memcpy(destination, source, bitmap.width);
    }
  }
  EngagedGPUCommandBuffer commandBuffer =
      acquireGPUCommandBuffer(app->device_ctx.get());
  UniqueGPUCopyPass copyPass{SDL_BeginGPUCopyPass(commandBuffer.get())};
  for (const auto &[codepoint, transferBuffer] : transferBuffers) {
    const auto &[freetypeGlyph, glyphTexture] = glyph_cache[codepoint];
    const FT_Bitmap &bitmap = asBitmapGlyph(freetypeGlyph.get())->bitmap;
    SDL_GPUTextureTransferInfo source{.transfer_buffer = transferBuffer.get(),
                                      .offset = 0,
                                      .pixels_per_row = bitmap.width,
                                      .rows_per_layer = bitmap.rows};
    SDL_GPUTextureRegion destination{.texture = glyphTexture.get(),
                                     .w = bitmap.width,
                                     .h = bitmap.rows,
                                     .d = 1};
    SDL_UploadToGPUTexture(copyPass.get(), &source, &destination, false);
  }
}

void AppState::GlyphCompositor::glyphs_combine() {
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
        asBitmapGlyph(cacheIt->second.freetypeGlyph.get());
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

  combined_width = static_cast<std::int32_t>(
      std::ceil(static_cast<float>(maxX - minX) / 64.0f));
  combined_height = static_cast<std::int32_t>(
      std::ceil(static_cast<float>(maxY - minY) / 64.0f));

  EngagedGPUCommandBuffer commandBuffer =
      acquireGPUCommandBuffer(app->device_ctx.get());
  GPUTextureCreateInfo textureInfo{
      .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
      .texture_width = static_cast<std::uint32_t>(combined_width),
      .texture_height = static_cast<std::uint32_t>(combined_height),
  };
  combined_texture = createGPUTexture(app->device_ctx.get(), textureInfo);
  UniqueGPURenderPass renderPass =
      beginGPURenderPass(commandBuffer.get(), combined_texture.get(),
                         SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f});
  SDL_BindGPUGraphicsPipeline(renderPass.get(), app->glyph_pipeline.get());

  struct GlyphUniforms {
    float viewport[2];
    float rect_pos[2];
    float rect_size[2];
  };
  for (GlyphCarry c{};
       c.glyph_index < hb_buffer_get_length(harfbuzz_buffer.get());
       c.pen_x += positions[c.glyph_index].x_advance, ++c.glyph_index) {
    GlyphCache::iterator cacheIt =
        glyph_cache.find(infos[c.glyph_index].codepoint);
    if (cacheIt == glyph_cache.end())
      continue;

    FT_BitmapGlyph bitmapGlyph =
        asBitmapGlyph(cacheIt->second.freetypeGlyph.get());
    GlyphUniforms uniforms{};
    uniforms.viewport[0] = static_cast<float>(combined_width);
    uniforms.viewport[1] = static_cast<float>(combined_height);
    uniforms.rect_pos[0] =
        static_cast<float>(c.pen_x + positions[c.glyph_index].x_offset +
                           bitmapGlyph->left * 64 - minX) /
        64.0f;
    uniforms.rect_pos[1] =
        static_cast<float>(-positions[c.glyph_index].y_offset -
                           bitmapGlyph->top * 64 - minY) /
        64.0f;
    uniforms.rect_size[0] = static_cast<float>(bitmapGlyph->bitmap.width);
    uniforms.rect_size[1] = static_cast<float>(bitmapGlyph->bitmap.rows);
    SDL_PushGPUVertexUniformData(commandBuffer.get(), 0, &uniforms,
                                 sizeof(GlyphUniforms));

    SDL_GPUTextureSamplerBinding samplerBinding{};
    samplerBinding.texture = cacheIt->second.texture.get();
    samplerBinding.sampler = app->caption_sampler.get();
    SDL_BindGPUFragmentSamplers(renderPass.get(), 0, &samplerBinding, 1);

    SDL_DrawGPUPrimitives(renderPass.get(), 4, 1, 0, 0);
  }
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

void AppState::caption_text_compose() {
  caption_compositor.glyphs_shape(caption_string);
  caption_compositor.glyphs_texturize();
  caption_compositor.glyphs_combine();
  caption_x = (INITIAL_WINDOW_WIDTH - caption_compositor.combined_width) / 2;
  caption_y = (INITIAL_WINDOW_HEIGHT - caption_compositor.combined_height) / 2;
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
    app->caption_text_compose();
    return SDL_APP_CONTINUE;
  } catch (const std::runtime_error &e) {
    SDL_Log("[App] %s", e.what());
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
