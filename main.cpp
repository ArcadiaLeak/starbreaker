#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include <freetype/freetype.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <vulkan/vulkan.h>

struct Vertex {
  float position[2];
  float uv[2];
};

static FT_Library globalFTLibrary = nullptr;

struct FreetypeFaceDeleter {
  void operator()(FT_Face facePtr) const { FT_Done_Face(facePtr); }
};
using UniqueFreetypeFace = std::unique_ptr<FT_FaceRec, FreetypeFaceDeleter>;

struct HarfbuzzFontDeleter {
  void operator()(hb_font_t *fontPtr) const { hb_font_destroy(fontPtr); }
};
using UniqueHarfbuzzFont = std::unique_ptr<hb_font_t, HarfbuzzFontDeleter>;

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

class AppGPUShader {
public:
  struct CreateInfo {
    const char *filepath;
    SDL_GPUShaderStage stage;
    Uint32 num_samplers;
    Uint32 num_uniform_buffers;
  };

  AppGPUShader(SDL_GPUDevice *device, CreateInfo createInfo);
  ~AppGPUShader();

  AppGPUShader(const AppGPUShader &) = delete;
  AppGPUShader &operator=(const AppGPUShader &) = delete;
  AppGPUShader(AppGPUShader &&) = delete;
  AppGPUShader &operator=(AppGPUShader &&) = delete;

  SDL_GPUShader *get() { return gpu_shader; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUShader *gpu_shader = nullptr;
};

AppGPUShader::AppGPUShader(SDL_GPUDevice *device, CreateInfo createInfo)
    : gpu_device{device} {
  SDL_GPUShaderCreateInfo shaderInfo{};
  UniqueFileContent shaderCode =
      loadFile(createInfo.filepath, &shaderInfo.code_size);
  shaderInfo.code = static_cast<const Uint8 *>(shaderCode.get());
  shaderInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  shaderInfo.stage = createInfo.stage;
  shaderInfo.num_samplers = createInfo.num_samplers;
  shaderInfo.num_uniform_buffers = createInfo.num_uniform_buffers;
  gpu_shader = SDL_CreateGPUShader(gpu_device, &shaderInfo);
  if (not gpu_shader) {
    std::string errorMsg{"SDL_CreateGPUShader failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
}

AppGPUShader::~AppGPUShader() {
  if (not gpu_shader)
    return;
  SDL_ReleaseGPUShader(gpu_device, gpu_shader);
}

class CaptionGPUSampler {
public:
  CaptionGPUSampler(SDL_GPUDevice *device);
  ~CaptionGPUSampler();

  CaptionGPUSampler(const CaptionGPUSampler &) = delete;
  CaptionGPUSampler &operator=(const CaptionGPUSampler &) = delete;
  CaptionGPUSampler(CaptionGPUSampler &&) = delete;
  CaptionGPUSampler &operator=(CaptionGPUSampler &&) = delete;

  SDL_GPUSampler *get() { return gpu_sampler; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUSampler *gpu_sampler = nullptr;
};

CaptionGPUSampler::CaptionGPUSampler(SDL_GPUDevice *device)
    : gpu_device{device} {
  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

  gpu_sampler = SDL_CreateGPUSampler(gpu_device, &samplerInfo);
  if (not gpu_sampler) {
    std::string errorMsg{"SDL_CreateGPUSampler failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

CaptionGPUSampler::~CaptionGPUSampler() {
  if (not gpu_sampler)
    return;
  SDL_ReleaseGPUSampler(gpu_device, gpu_sampler);
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
  SDL_GPUTexture *gpuTexture = SDL_CreateGPUTexture(device, &textureInfo);
  if (not gpuTexture) {
    std::string errorMsg{"SDL_CreateGPUTexture failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  return {gpuTexture, GPUTextureDeleter{device}};
}

class AppGPUUploadBuffer {
public:
  AppGPUUploadBuffer(SDL_GPUDevice *device, std::uint32_t bufferSize);
  ~AppGPUUploadBuffer();

  AppGPUUploadBuffer(const AppGPUUploadBuffer &) = delete;
  AppGPUUploadBuffer &operator=(const AppGPUUploadBuffer &) = delete;
  AppGPUUploadBuffer(AppGPUUploadBuffer &&) = delete;
  AppGPUUploadBuffer &operator=(AppGPUUploadBuffer &&) = delete;

  SDL_GPUTransferBuffer *get_buffer() { return gpu_buffer; }
  void *get_mapped() { return mapped_buffer; }

  void unmap();

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUTransferBuffer *gpu_buffer = nullptr;
  void *mapped_buffer = nullptr;
};

AppGPUUploadBuffer::AppGPUUploadBuffer(SDL_GPUDevice *device,
                                       std::uint32_t bufferSize)
    : gpu_device{device} {
  SDL_GPUTransferBufferCreateInfo bufferInfo{};
  bufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  bufferInfo.size = bufferSize;
  gpu_buffer = SDL_CreateGPUTransferBuffer(gpu_device, &bufferInfo);
  if (not gpu_buffer) {
    std::string errorMsg{"SDL_CreateGPUTransferBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  mapped_buffer = SDL_MapGPUTransferBuffer(gpu_device, gpu_buffer, false);
  if (not mapped_buffer) {
    SDL_ReleaseGPUTransferBuffer(gpu_device, gpu_buffer);
    std::string errorMsg{"SDL_MapGPUTransferBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
}

AppGPUUploadBuffer::~AppGPUUploadBuffer() {
  if (mapped_buffer)
    SDL_UnmapGPUTransferBuffer(gpu_device, gpu_buffer);
  if (gpu_buffer)
    SDL_ReleaseGPUTransferBuffer(gpu_device, gpu_buffer);
}

void AppGPUUploadBuffer::unmap() {
  if (mapped_buffer)
    SDL_UnmapGPUTransferBuffer(gpu_device, gpu_buffer);
  mapped_buffer = nullptr;
}

class CaptionGPUGraphicsPipeline {
public:
  CaptionGPUGraphicsPipeline(SDL_GPUDevice *device, SDL_Window *window);
  ~CaptionGPUGraphicsPipeline();

  CaptionGPUGraphicsPipeline(const CaptionGPUGraphicsPipeline &) = delete;
  CaptionGPUGraphicsPipeline &
  operator=(const CaptionGPUGraphicsPipeline &) = delete;
  CaptionGPUGraphicsPipeline(CaptionGPUGraphicsPipeline &&) = delete;
  CaptionGPUGraphicsPipeline &operator=(CaptionGPUGraphicsPipeline &&) = delete;

  SDL_GPUGraphicsPipeline *get() { return gpu_pipeline; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUGraphicsPipeline *gpu_pipeline = nullptr;
};

CaptionGPUGraphicsPipeline::CaptionGPUGraphicsPipeline(SDL_GPUDevice *device,
                                                       SDL_Window *window)
    : gpu_device{device} {
  AppGPUShader::CreateInfo vertexInfo{.filepath = "caption.vert.spv",
                                      .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                      .num_samplers = 0,
                                      .num_uniform_buffers = 1};
  AppGPUShader vertexShader{gpu_device, vertexInfo};

  AppGPUShader::CreateInfo fragmentInfo{.filepath = "caption.frag.spv",
                                        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                        .num_samplers = 1,
                                        .num_uniform_buffers = 1};
  AppGPUShader fragmentShader{gpu_device, fragmentInfo};

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

  SDL_GPUGraphicsPipelineCreateInfo pipeInfo{};
  pipeInfo.vertex_shader = vertexShader.get();
  pipeInfo.fragment_shader = fragmentShader.get();
  pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pipeInfo.target_info = targetInfo;

  gpu_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipeInfo);
  if (not gpu_pipeline) {
    std::string errorMsg{"SDL_CreateGPUGraphicsPipeline failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

CaptionGPUGraphicsPipeline::~CaptionGPUGraphicsPipeline() {
  if (not gpu_pipeline)
    return;
  SDL_ReleaseGPUGraphicsPipeline(gpu_device, gpu_pipeline);
}

class GlyphGPUGraphicsPipeline {
public:
  GlyphGPUGraphicsPipeline(SDL_GPUDevice *device);
  ~GlyphGPUGraphicsPipeline();

  GlyphGPUGraphicsPipeline(const GlyphGPUGraphicsPipeline &) = delete;
  GlyphGPUGraphicsPipeline &
  operator=(const GlyphGPUGraphicsPipeline &) = delete;
  GlyphGPUGraphicsPipeline(GlyphGPUGraphicsPipeline &&) = delete;
  GlyphGPUGraphicsPipeline &operator=(GlyphGPUGraphicsPipeline &&) = delete;

  SDL_GPUGraphicsPipeline *get() { return gpu_pipeline; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUGraphicsPipeline *gpu_pipeline = nullptr;
};

GlyphGPUGraphicsPipeline::GlyphGPUGraphicsPipeline(SDL_GPUDevice *device)
    : gpu_device{device} {
  AppGPUShader::CreateInfo vertexInfo{.filepath = "caption.vert.spv",
                                      .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                      .num_samplers = 0,
                                      .num_uniform_buffers = 1};
  AppGPUShader vertexShader{gpu_device, vertexInfo};

  AppGPUShader::CreateInfo fragmentInfo{.filepath = "glyph.frag.spv",
                                        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                        .num_samplers = 1,
                                        .num_uniform_buffers = 0};
  AppGPUShader fragmentShader{gpu_device, fragmentInfo};

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

  gpu_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipelineInfo);
  if (not gpu_pipeline) {
    std::string errorMsg{"SDL_CreateGPUGraphicsPipeline failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

GlyphGPUGraphicsPipeline::~GlyphGPUGraphicsPipeline() {
  if (not gpu_pipeline)
    return;
  SDL_ReleaseGPUGraphicsPipeline(gpu_device, gpu_pipeline);
}

class AppGPURenderCommand {
public:
  struct CreateInfo {
    SDL_Window *swapchain_window;
    std::uint32_t texture_width;
    std::uint32_t texture_height;
    SDL_FColor clear_color;
  };

  AppGPURenderCommand(SDL_GPUDevice *device, CreateInfo createInfo);
  ~AppGPURenderCommand();

  AppGPURenderCommand(const AppGPURenderCommand &) = delete;
  AppGPURenderCommand &operator=(const AppGPURenderCommand &) = delete;
  AppGPURenderCommand(AppGPURenderCommand &&) = delete;
  AppGPURenderCommand &operator=(AppGPURenderCommand &&) = delete;

  SDL_GPUCommandBuffer *get_command() { return gpu_command; }
  SDL_GPUTexture *get_texture() { return gpu_texture; }
  std::uint32_t get_texture_width() { return texture_width; }
  std::uint32_t get_texture_height() { return texture_height; }
  SDL_GPURenderPass *get_render_pass() { return render_pass; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUCommandBuffer *gpu_command = nullptr;
  SDL_GPUTexture *gpu_texture = nullptr;
  std::uint32_t texture_width = 0, texture_height = 0;
  SDL_GPURenderPass *render_pass = nullptr;
};

AppGPURenderCommand::AppGPURenderCommand(SDL_GPUDevice *device,
                                         CreateInfo createInfo)
    : gpu_device{device} {
  gpu_command = SDL_AcquireGPUCommandBuffer(gpu_device);
  if (not gpu_command) {
    std::string errorMsg{"SDL_AcquireGPUCommandBuffer failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }

  if (createInfo.swapchain_window) {
    if (not SDL_WaitAndAcquireGPUSwapchainTexture(
            gpu_command, createInfo.swapchain_window, &gpu_texture,
            &texture_width, &texture_height)) {
      std::string errorMsg{"SDL_WaitAndAcquireGPUSwapchainTexture failed: "};
      errorMsg.append(SDL_GetError());
      throw std::runtime_error{errorMsg};
    }
  } else {
    texture_height = createInfo.texture_height;
    texture_width = createInfo.texture_width;

    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
    textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    textureInfo.usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    textureInfo.width = texture_width;
    textureInfo.height = texture_height;
    textureInfo.layer_count_or_depth = 1;
    textureInfo.num_levels = 1;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    gpu_texture = SDL_CreateGPUTexture(device, &textureInfo);
    if (not gpu_texture) {
      std::string errorMsg{"SDL_CreateGPUTexture failed: "};
      errorMsg.append(SDL_GetError());
      throw std::runtime_error{errorMsg};
    }
  }

  if (gpu_texture) {
    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = gpu_texture;
    colorTarget.clear_color = createInfo.clear_color;
    colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;
    render_pass = SDL_BeginGPURenderPass(gpu_command, &colorTarget, 1, nullptr);
  }
}

AppGPURenderCommand::~AppGPURenderCommand() {
  if (gpu_texture)
    SDL_EndGPURenderPass(render_pass);
  if (SDL_SubmitGPUCommandBuffer(gpu_command))
    return;
  SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
  std::terminate();
}

class AppGPUCopyCommand {
public:
  AppGPUCopyCommand(SDL_GPUDevice *device);
  ~AppGPUCopyCommand();

  AppGPUCopyCommand(const AppGPUCopyCommand &) = delete;
  AppGPUCopyCommand &operator=(const AppGPUCopyCommand &) = delete;
  AppGPUCopyCommand(AppGPUCopyCommand &&) = delete;
  AppGPUCopyCommand &operator=(AppGPUCopyCommand &&) = delete;

  SDL_GPUCommandBuffer *get_command() { return gpu_command; }
  SDL_GPUCopyPass *get_copy_pass() { return copy_pass; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUCommandBuffer *gpu_command = nullptr;
  SDL_GPUCopyPass *copy_pass = nullptr;
};

AppGPUCopyCommand::AppGPUCopyCommand(SDL_GPUDevice *device)
    : gpu_device{device} {
  gpu_command = SDL_AcquireGPUCommandBuffer(gpu_device);
  if (not gpu_command) {
    std::string errorMsg{"SDL_AcquireGPUCommandBuffer failed: "};
    throw std::runtime_error{errorMsg + SDL_GetError()};
  }
  copy_pass = SDL_BeginGPUCopyPass(gpu_command);
}

AppGPUCopyCommand::~AppGPUCopyCommand() {
  SDL_EndGPUCopyPass(copy_pass);
  if (SDL_SubmitGPUCommandBuffer(gpu_command))
    return;
  SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
  std::terminate();
}

struct GlyphInfo {
  hb_codepoint_t codepoint;
  std::uint32_t cluster;
  hb_position_t x_advance, x_offset, y_offset;
  FT_Pos rsb_delta;
  unsigned int width, height;
  int bitmap_left, bitmap_top;
};

struct GlyphImage {
  std::vector<unsigned char> bitmap;
  UniqueGPUTexture texture;
};

class AppState {
private:
  class GlyphCompositor {
  private:
    AppState *app;

    unsigned int glyph_count;
    std::vector<GlyphInfo> glyph_infos;
    std::vector<GlyphImage> glyph_images;
    UniqueGPUTexture combined_texture;
    SDL_Rect combined_rect;

    void glyphs_alloc(std::string_view textView);
    void glyphs_render();
    void glyphs_upload();
    void glyphs_combine();

  public:
    GlyphCompositor(AppState *appstate);
    void operator()(std::string_view textView);
  };

  UniqueHarfbuzzFont harfbuzz_font;

  UniqueGPUDevice gpu_device;
  UniqueWindow main_window;
  UniqueWindowForGPUDevice main_window_for_gpu_device;

  CaptionGPUSampler caption_sampler;
  CaptionGPUGraphicsPipeline caption_pipeline;
  GlyphGPUGraphicsPipeline glyph_pipeline;

  std::string caption_string = "Hello world kerning Text";
  std::vector<GlyphInfo> caption_glyph_infos;
  UniqueGPUTexture caption_texture;
  SDL_Rect caption_rect;

public:
  AppState(FT_Library library);

  SDL_AppResult iterate() noexcept;
  SDL_AppResult handle_event(SDL_Event *event) noexcept;

  void caption_text_compose();
};

AppState::AppState(FT_Library library) {
  gpu_device = createGPUDevice();
  main_window = createWindow();
  main_window_for_gpu_device =
      claimWindowForGPUDevice(gpu_device.get(), main_window.get());
  harfbuzz_font = createHarfbuzzFont(library, "assets/DejaVuSans.ttf", 10 * 64,
                                     getWindowDisplayScale(main_window.get()));
}

SDL_AppResult AppState::iterate() noexcept {
  AppGPURenderCommand::CreateInfo commandInfo{
      .swapchain_window = main_window.get(),
      .clear_color = SDL_FColor{0.08f, 0.08f, 0.10f, 1.0f}};
  AppGPURenderCommand swapchainCommand{gpu_device.get(), commandInfo};

  SDL_BindGPUGraphicsPipeline(swapchainCommand.get_render_pass(),
                              caption_pipeline.get());
  struct QuadUniforms {
    float viewport[2];
    float rect_pos[2];
    float rect_size[2];
  };
  struct BlitUniforms {
    float tint[4];
  };

  QuadUniforms uniforms{};
  uniforms.viewport[0] =
      static_cast<float>(swapchainCommand.get_texture_width());
  uniforms.viewport[1] =
      static_cast<float>(swapchainCommand.get_texture_height());
  uniforms.rect_pos[0] = static_cast<float>(caption_rect.x);
  uniforms.rect_pos[1] = static_cast<float>(caption_rect.y);
  uniforms.rect_size[0] = static_cast<float>(caption_rect.w);
  uniforms.rect_size[1] = static_cast<float>(caption_rect.h);
  SDL_PushGPUVertexUniformData(swapchainCommand.get_command(), 0, &uniforms,
                               sizeof(QuadUniforms));

  BlitUniforms blit{};
  blit.tint[0] = 1.0f;
  blit.tint[1] = 1.0f;
  blit.tint[2] = 1.0f;
  blit.tint[3] = 1.0f;
  SDL_PushGPUFragmentUniformData(swapchainCommand.get_command(), 0, &blit,
                                 sizeof(BlitUniforms));

  SDL_GPUTextureSamplerBinding sb{};
  sb.texture = caption_texture.get();
  sb.sampler = caption_sampler.get();
  SDL_BindGPUFragmentSamplers(swapchainCommand.get_render_pass(), 0, &sb, 1);

  SDL_DrawGPUPrimitives(swapchainCommand.get_render_pass(), 4, 1, 0, 0);
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
    caption_rect.x = (event->window.data1 - caption_rect.w) / 2;
    caption_rect.y = (event->window.data2 - caption_rect.h) / 2;
    return SDL_APP_CONTINUE;
  } else
    return SDL_APP_CONTINUE;
}

void AppState::GlyphCompositor::glyphs_alloc(std::string_view textView) {
  hb_buffer_t *glyphBuffer = hb_buffer_create();

  hb_buffer_add_utf8(glyphBuffer, textView.data(),
                     static_cast<int>(textView.size()), 0,
                     static_cast<int>(textView.size()));
  hb_buffer_guess_segment_properties(glyphBuffer);
  hb_shape(app->harfbuzz_font.get(), glyphBuffer, nullptr, 0);

  hb_glyph_info_t *glyphInfos =
      hb_buffer_get_glyph_infos(glyphBuffer, &glyph_count);
  hb_glyph_position_t *glyphPositions =
      hb_buffer_get_glyph_positions(glyphBuffer, &glyph_count);

  glyph_infos = std::vector<GlyphInfo>(glyph_count);
  glyph_images = std::vector<GlyphImage>(glyph_count);
  for (unsigned int i = 0; i < glyph_count; ++i) {
    glyph_infos[i].codepoint = glyphInfos[i].codepoint;
    glyph_infos[i].cluster = glyphInfos[i].cluster;
    glyph_infos[i].x_advance = glyphPositions[i].x_advance;
    glyph_infos[i].x_offset = glyphPositions[i].x_offset;
    glyph_infos[i].y_offset = glyphPositions[i].y_offset;
  }

  hb_buffer_destroy(glyphBuffer);
}

void AppState::GlyphCompositor::glyphs_render() {
  for (unsigned int i = 0; i < glyph_count; ++i) {
    FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER;
    FT_Face face = hb_ft_font_get_ft_face(app->harfbuzz_font.get());
    if (FT_Load_Glyph(face, glyph_infos[i].codepoint, flags) != 0) {
      SDL_Log("FT_Load_Glyph failed for glyph: %d", glyph_infos[i].codepoint);
      continue;
    }
    FT_GlyphSlot slot = face->glyph;
    glyph_infos[i].bitmap_left = slot->bitmap_left;
    glyph_infos[i].bitmap_top = slot->bitmap_top;
    glyph_infos[i].rsb_delta = slot->rsb_delta;
    if (i != 0) {
      FT_Pos delta_diff = glyph_infos[i - 1].rsb_delta - slot->lsb_delta;
      if (delta_diff > 32)
        glyph_infos[i].x_offset -= 64;
      else if (delta_diff < -31)
        glyph_infos[i].x_offset += 64;
    }
    const FT_Bitmap &bitmap = slot->bitmap;
    glyph_infos[i].width = bitmap.width;
    glyph_infos[i].height = bitmap.rows;
    if (glyph_infos[i].width == 0 || glyph_infos[i].height == 0)
      continue;
    glyph_images[i].bitmap.resize(bitmap.width * bitmap.rows);
    for (unsigned int y = 0; y < bitmap.rows; ++y) {
      const uint8_t *rowSource = bitmap.buffer + y * std::abs(bitmap.pitch);
      uint8_t *rowTarget = glyph_images[i].bitmap.data() + y * bitmap.width;
      std::memcpy(rowTarget, rowSource, bitmap.width);
    }
  }
}

void AppState::GlyphCompositor::glyphs_upload() {
  std::vector<std::optional<AppGPUUploadBuffer>> uploadBuffers(glyph_count);
  for (unsigned int i = 0; i < glyph_count; ++i) {
    if (glyph_infos[i].width == 0 || glyph_infos[i].height == 0)
      continue;
    GPUTextureCreateInfo textureInfo{.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                     .texture_width = glyph_infos[i].width,
                                     .texture_height = glyph_infos[i].height};
    glyph_images[i].texture =
        createGPUTexture(app->gpu_device.get(), textureInfo);
    uploadBuffers[i].emplace(
        app->gpu_device.get(),
        static_cast<std::uint32_t>(glyph_images[i].bitmap.size()));
    std::memcpy(uploadBuffers[i]->get_mapped(), glyph_images[i].bitmap.data(),
                glyph_images[i].bitmap.size());
    uploadBuffers[i]->unmap();
  }

  AppGPUCopyCommand copyCommand{app->gpu_device.get()};
  for (unsigned int i = 0; i < glyph_count; ++i) {
    if (not glyph_images[i].texture)
      continue;
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = uploadBuffers[i]->get_buffer();
    src.offset = 0;
    src.pixels_per_row = glyph_infos[i].width;
    src.rows_per_layer = glyph_infos[i].height;
    SDL_GPUTextureRegion dst{};
    dst.texture = glyph_images[i].texture.get();
    dst.w = glyph_infos[i].width;
    dst.h = glyph_infos[i].height;
    dst.d = 1;
    SDL_UploadToGPUTexture(copyCommand.get_copy_pass(), &src, &dst, false);
  }
}

void AppState::GlyphCompositor::glyphs_combine() {
  hb_position_t minX = std::numeric_limits<hb_position_t>::max(),
                maxX = std::numeric_limits<hb_position_t>::min();
  hb_position_t minY = std::numeric_limits<hb_position_t>::max(),
                maxY = std::numeric_limits<hb_position_t>::min();

  for (hb_position_t accumulatedX{}; const GlyphInfo &glyphInfo : glyph_infos) {
    hb_position_t penX = accumulatedX;
    accumulatedX += glyphInfo.x_advance;

    if (glyphInfo.width == 0 || glyphInfo.height == 0)
      continue;

    hb_position_t left = penX + glyphInfo.x_offset + glyphInfo.bitmap_left * 64;
    hb_position_t right =
        left + static_cast<hb_position_t>(glyphInfo.width) * 64;
    hb_position_t top = -glyphInfo.y_offset - glyphInfo.bitmap_top * 64;
    hb_position_t bottom =
        top + static_cast<hb_position_t>(glyphInfo.height) * 64;

    minX = std::min(minX, left);
    maxX = std::max(maxX, right);
    minY = std::min(minY, top);
    maxY = std::max(maxY, bottom);
  }

  combined_rect.w = static_cast<std::int32_t>(
      std::ceil(static_cast<float>(maxX - minX) / 64.0f));
  combined_rect.h = static_cast<std::int32_t>(
      std::ceil(static_cast<float>(maxY - minY) / 64.0f));
  combined_rect.x = (INITIAL_WINDOW_WIDTH - combined_rect.w) / 2;
  combined_rect.y = (INITIAL_WINDOW_HEIGHT - combined_rect.h) / 2;

  AppGPURenderCommand::CreateInfo renderInfo{
      .texture_width = static_cast<std::uint32_t>(combined_rect.w),
      .texture_height = static_cast<std::uint32_t>(combined_rect.h),
      .clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f}};
  AppGPURenderCommand renderCommand{app->gpu_device.get(), renderInfo};
  combined_texture = UniqueGPUTexture{renderCommand.get_texture(),
                                      GPUTextureDeleter{app->gpu_device.get()}};

  SDL_BindGPUGraphicsPipeline(renderCommand.get_render_pass(),
                              glyph_pipeline.get());
  struct GlyphUniforms {
    float viewport[2];
    float rect_pos[2];
    float rect_size[2];
  };

  for (hb_position_t accumulatedX{};
       auto &&[glyphInfo, glyphImage] :
       std::views::zip(glyph_infos, glyph_images)) {
    hb_position_t penX = accumulatedX;
    accumulatedX += glyphInfo.x_advance;

    if (glyphInfo.width == 0 || glyphInfo.height == 0)
      continue;

    GlyphUniforms uniforms{};
    uniforms.viewport[0] = static_cast<float>(combined_rect.w);
    uniforms.viewport[1] = static_cast<float>(combined_rect.h);
    uniforms.rect_pos[0] =
        static_cast<float>(penX + glyphInfo.x_offset +
                           glyphInfo.bitmap_left * 64 - minX) /
        64.0f;
    uniforms.rect_pos[1] =
        static_cast<float>(-glyphInfo.y_offset - glyphInfo.bitmap_top * 64 -
                           minY) /
        64.0f;
    uniforms.rect_size[0] = static_cast<float>(glyphInfo.width);
    uniforms.rect_size[1] = static_cast<float>(glyphInfo.height);
    SDL_PushGPUVertexUniformData(renderCommand.get_command(), 0, &uniforms,
                                 sizeof(GlyphUniforms));

    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = glyphImage.texture.get();
    sb.sampler = caption_sampler.get();
    SDL_BindGPUFragmentSamplers(renderCommand.get_render_pass(), 0, &sb, 1);

    SDL_DrawGPUPrimitives(renderCommand.get_render_pass(), 4, 1, 0, 0);
  }
}

void AppState::caption_text_compose() {
  GlyphCompositor compose_glyphs{this};
  compose_glyphs(caption_string);
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
