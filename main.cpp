#include <SDL3/SDL_video.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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

static FT_Library global_ft_library = nullptr;

class AppFont {
private:
  FT_Library ft_library = nullptr;
  FT_Face ft_face = nullptr;
  hb_font_t *hb_font = nullptr;

public:
  FT_Face get_face() const noexcept { return ft_face; }
  hb_font_t *get_font() const noexcept { return hb_font; }

  AppFont(FT_Library ft_lib, const char *filepath, FT_UInt pixel_height);
  ~AppFont();

  AppFont(const AppFont &) = delete;
  AppFont &operator=(const AppFont &) = delete;
  AppFont(AppFont &&) = delete;
  AppFont &operator=(AppFont &&) = delete;
};

AppFont::AppFont(FT_Library ft_lib, const char *filepath, FT_UInt pixel_height)
    : ft_library{ft_lib} {
  if (FT_New_Face(ft_library, filepath, 0, &ft_face))
    throw std::runtime_error{"FT_New_Face failed!"};
  if (FT_Set_Pixel_Sizes(ft_face, 0, pixel_height)) {
    FT_Done_Face(ft_face);
    throw std::runtime_error{"FT_Set_Pixel_Sizes failed!"};
  }
  hb_font = hb_ft_font_create(ft_face, nullptr);
  hb_ft_font_set_load_flags(hb_font, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
}

AppFont::~AppFont() {
  if (hb_font)
    hb_font_destroy(hb_font);
  if (ft_face)
    FT_Done_Face(ft_face);
}

class AppGPUDevice {
public:
  AppGPUDevice();
  ~AppGPUDevice();

  AppGPUDevice(const AppGPUDevice &) = delete;
  AppGPUDevice &operator=(const AppGPUDevice &) = delete;
  AppGPUDevice(AppGPUDevice &&) = delete;
  AppGPUDevice &operator=(AppGPUDevice &&) = delete;

  SDL_GPUDevice *get() { return gpu_device; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
};

AppGPUDevice::AppGPUDevice() {
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
  gpu_device = SDL_CreateGPUDeviceWithProperties(device_props);
  SDL_DestroyProperties(device_props);

  if (not gpu_device)
    throw std::runtime_error{"Failed to create GPU device!"};
}

AppGPUDevice::~AppGPUDevice() {
  if (not gpu_device)
    return;
  if (not SDL_WaitForGPUIdle(gpu_device)) {
    SDL_Log("SDL_WaitForGPUIdle failed: %s", SDL_GetError());
    std::terminate();
  }
  SDL_DestroyGPUDevice(gpu_device);
}

class AppWindow {
public:
  AppWindow(SDL_GPUDevice *device);
  ~AppWindow();

  AppWindow(const AppWindow &) = delete;
  AppWindow &operator=(const AppWindow &) = delete;
  AppWindow(AppWindow &&) = delete;
  AppWindow &operator=(AppWindow &&) = delete;

  SDL_Window *get() { return window; }

private:
  SDL_Window *window = nullptr;
  SDL_GPUDevice *gpu_device = nullptr;
};

AppWindow::AppWindow(SDL_GPUDevice *device) : gpu_device{device} {
  window =
      SDL_CreateWindow("Hello GPU", 800, 600,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (not window)
    throw std::runtime_error{"SDL_CreateWindow failed!"};
  if (not SDL_ClaimWindowForGPUDevice(device, window)) {
    SDL_DestroyWindow(window);
    throw std::runtime_error{"SDL_ClaimWindowForGPUDevice failed!"};
  }
}

AppWindow::~AppWindow() {
  if (not window)
    return;
  SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
  SDL_DestroyWindow(window);
}

class AppFile {
public:
  AppFile(const char *filepath);
  ~AppFile();

  AppFile(const AppFile &) = delete;
  AppFile &operator=(const AppFile &) = delete;
  AppFile(AppFile &&) = delete;
  AppFile &operator=(AppFile &&) = delete;

  void *get_data() { return file_data; }
  std::size_t get_size() { return file_size; }

private:
  void *file_data = nullptr;
  size_t file_size = 0;
};

AppFile::AppFile(const char *filepath) {
  file_data = SDL_LoadFile(filepath, &file_size);
  if (not file_data) {
    std::string errorMsg{"Failed to load file: "};
    errorMsg.append(filepath);
    throw std::runtime_error{errorMsg};
  }
}

AppFile::~AppFile() {
  if (not file_data)
    return;
  SDL_free(file_data);
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
  AppFile appFile{createInfo.filepath};

  SDL_GPUShaderCreateInfo shaderInfo{};
  shaderInfo.code_size = appFile.get_size();
  shaderInfo.code = static_cast<const Uint8 *>(appFile.get_data());
  shaderInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
  shaderInfo.stage = createInfo.stage;
  shaderInfo.num_samplers = createInfo.num_samplers;
  shaderInfo.num_uniform_buffers = createInfo.num_uniform_buffers;
  gpu_shader = SDL_CreateGPUShader(gpu_device, &shaderInfo);
  if (not gpu_shader) {
    std::string errorMsg{"SDL_CreateGPUShader failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

AppGPUShader::~AppGPUShader() {
  if (not gpu_shader)
    return;
  SDL_ReleaseGPUShader(gpu_device, gpu_shader);
}

class AppGPUSampler {
public:
  AppGPUSampler(SDL_GPUDevice *device);
  ~AppGPUSampler();

  AppGPUSampler(const AppGPUSampler &) = delete;
  AppGPUSampler &operator=(const AppGPUSampler &) = delete;
  AppGPUSampler(AppGPUSampler &&) = delete;
  AppGPUSampler &operator=(AppGPUSampler &&) = delete;

  SDL_GPUSampler *get() { return gpu_sampler; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUSampler *gpu_sampler = nullptr;
};

AppGPUSampler::AppGPUSampler(SDL_GPUDevice *device) : gpu_device{device} {
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

AppGPUSampler::~AppGPUSampler() {
  if (not gpu_sampler)
    return;
  SDL_ReleaseGPUSampler(gpu_device, gpu_sampler);
}

class AppGPUGlyphTexture {
public:
  AppGPUGlyphTexture(SDL_GPUDevice *device, std::uint32_t textureWidth,
                     std::uint32_t textureHeight);
  ~AppGPUGlyphTexture();

  AppGPUGlyphTexture(const AppGPUGlyphTexture &) = delete;
  AppGPUGlyphTexture &operator=(const AppGPUGlyphTexture &) = delete;
  AppGPUGlyphTexture(AppGPUGlyphTexture &&) = delete;
  AppGPUGlyphTexture &operator=(AppGPUGlyphTexture &&) = delete;

  SDL_GPUTexture *get() { return gpu_texture; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUTexture *gpu_texture = nullptr;
};

AppGPUGlyphTexture::AppGPUGlyphTexture(SDL_GPUDevice *device,
                                       std::uint32_t textureWidth,
                                       std::uint32_t textureHeight)
    : gpu_device{device} {
  SDL_GPUTextureCreateInfo textureInfo{};
  textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
  textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
  textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  textureInfo.width = textureWidth;
  textureInfo.height = textureHeight;
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

AppGPUGlyphTexture::~AppGPUGlyphTexture() {
  if (not gpu_texture)
    return;
  SDL_ReleaseGPUTexture(gpu_device, gpu_texture);
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
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
  mapped_buffer = SDL_MapGPUTransferBuffer(gpu_device, gpu_buffer, false);
  if (not mapped_buffer) {
    SDL_ReleaseGPUTransferBuffer(gpu_device, gpu_buffer);
    std::string errorMsg{"SDL_MapGPUTransferBuffer failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
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
                                        .num_uniform_buffers = 0};
  AppGPUShader fragmentShader{gpu_device, fragmentInfo};

  std::array<SDL_GPUVertexAttribute, 2> attrs{};
  attrs[0].location = 0;
  attrs[0].buffer_slot = 0;
  attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[0].offset = offsetof(Vertex, position);

  attrs[1].location = 1;
  attrs[1].buffer_slot = 0;
  attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[1].offset = offsetof(Vertex, uv);

  SDL_GPUVertexBufferDescription vertexDescription{};
  vertexDescription.slot = 0;
  vertexDescription.pitch = sizeof(Vertex);
  vertexDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

  SDL_GPUVertexInputState vertexInput{};
  vertexInput.vertex_buffer_descriptions = &vertexDescription;
  vertexInput.num_vertex_buffers = 1;
  vertexInput.vertex_attributes = attrs.data();
  vertexInput.num_vertex_attributes = 2;

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
  colorTarget.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
  targetInfo.color_target_descriptions = &colorTarget;
  targetInfo.num_color_targets = 1;

  SDL_GPUGraphicsPipelineCreateInfo pipeInfo{};
  pipeInfo.vertex_shader = vertexShader.get();
  pipeInfo.fragment_shader = fragmentShader.get();
  pipeInfo.vertex_input_state = vertexInput;
  pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pipeInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
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

  AppGPUShader::CreateInfo fragmentInfo{.filepath = "caption.frag.spv",
                                        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                        .num_samplers = 1,
                                        .num_uniform_buffers = 0};
  AppGPUShader fragmentShader{gpu_device, fragmentInfo};

  std::array<SDL_GPUVertexAttribute, 2> attrs{};
  attrs[0].location = 0;
  attrs[0].buffer_slot = 0;
  attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[0].offset = offsetof(Vertex, position);

  attrs[1].location = 1;
  attrs[1].buffer_slot = 0;
  attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[1].offset = offsetof(Vertex, uv);

  SDL_GPUVertexBufferDescription vertexDescription{};
  vertexDescription.slot = 0;
  vertexDescription.pitch = sizeof(Vertex);
  vertexDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

  SDL_GPUVertexInputState vertexInput{};
  vertexInput.vertex_buffer_descriptions = &vertexDescription;
  vertexInput.num_vertex_buffers = 1;
  vertexInput.vertex_attributes = attrs.data();
  vertexInput.num_vertex_attributes = 2;

  SDL_GPUColorTargetDescription colorTarget{};
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
  pipeInfo.vertex_shader = vertexShader.get();
  pipeInfo.fragment_shader = fragmentShader.get();
  pipeInfo.vertex_input_state = vertexInput;
  pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pipeInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  pipeInfo.target_info = targetInfo;

  gpu_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipeInfo);
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

class AppGPUSwapchainCommand {
public:
  AppGPUSwapchainCommand(SDL_GPUDevice *device, SDL_Window *window);
  ~AppGPUSwapchainCommand();

  AppGPUSwapchainCommand(const AppGPUSwapchainCommand &) = delete;
  AppGPUSwapchainCommand &operator=(const AppGPUSwapchainCommand &) = delete;
  AppGPUSwapchainCommand(AppGPUSwapchainCommand &&) = delete;
  AppGPUSwapchainCommand &operator=(AppGPUSwapchainCommand &&) = delete;

  SDL_GPUCommandBuffer *get_command() { return gpu_command; }
  SDL_GPUTexture *get_swapchain() { return swapchain; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUCommandBuffer *gpu_command = nullptr;
  SDL_GPUTexture *swapchain = nullptr;
  std::uint32_t swapchain_width = 0, swapchain_height = 0;
};

AppGPUSwapchainCommand::AppGPUSwapchainCommand(SDL_GPUDevice *device,
                                               SDL_Window *window)
    : gpu_device{device} {
  gpu_command = SDL_AcquireGPUCommandBuffer(gpu_device);
  if (not gpu_command) {
    std::string errorMsg{"SDL_AcquireGPUCommandBuffer failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
  if (not SDL_WaitAndAcquireGPUSwapchainTexture(gpu_command, window, &swapchain,
                                                &swapchain_width,
                                                &swapchain_height)) {
    std::string errorMsg{"SDL_WaitAndAcquireGPUSwapchainTexture failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
}

AppGPUSwapchainCommand::~AppGPUSwapchainCommand() {
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
  SDL_GPUCopyPass *get_copy_pass() { return gpu_copy_pass; }

private:
  SDL_GPUDevice *gpu_device = nullptr;
  SDL_GPUCommandBuffer *gpu_command = nullptr;
  SDL_GPUCopyPass *gpu_copy_pass = nullptr;
};

AppGPUCopyCommand::AppGPUCopyCommand(SDL_GPUDevice *device)
    : gpu_device{device} {
  gpu_command = SDL_AcquireGPUCommandBuffer(gpu_device);
  if (not gpu_command) {
    std::string errorMsg{"SDL_AcquireGPUCommandBuffer failed: "};
    errorMsg.append(SDL_GetError());
    throw std::runtime_error{errorMsg};
  }
  gpu_copy_pass = SDL_BeginGPUCopyPass(gpu_command);
}

AppGPUCopyCommand::~AppGPUCopyCommand() {
  SDL_EndGPUCopyPass(gpu_copy_pass);
  if (SDL_SubmitGPUCommandBuffer(gpu_command))
    return;
  SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
  std::terminate();
}

struct GlyphData {
  hb_codepoint_t codepoint;
  std::uint32_t cluster;
  hb_position_t x_advance, y_advance;
  hb_position_t x_offset, y_offset;
  std::vector<unsigned char> bitmap;
  unsigned int width, height;
  int bitmap_left, bitmap_top;
  std::optional<AppGPUGlyphTexture> texture;
};

class AppState {
private:
  AppFont app_font;

  AppGPUDevice app_device;
  AppWindow app_window;
  AppGPUSampler app_sampler;

  CaptionGPUGraphicsPipeline caption_pipeline;
  GlyphGPUGraphicsPipeline glyph_pipeline;

  std::string caption_string = "Hello world";
  std::vector<GlyphData> caption_glyphs;

public:
  AppState(FT_Library ft_library)
      : app_font{ft_library, "assets/DejaVuSans.ttf", 14}, app_device{},
        app_window{app_device.get()}, app_sampler{app_device.get()},
        caption_pipeline{app_device.get(), app_window.get()},
        glyph_pipeline{app_device.get()} {}

  void caption_glyphs_alloc();
  void caption_glyphs_render();
  void caption_glyphs_upload();
  void caption_glyphs_combine();

  SDL_AppResult iterate() noexcept;
};

SDL_AppResult AppState::iterate() noexcept {
  AppGPUSwapchainCommand swapchainCommand{app_device.get(), app_window.get()};

  if (SDL_GPUTexture *swapchain = swapchainCommand.get_swapchain(); swapchain) {
    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = swapchain;
    colorTarget.clear_color = {0.08f, 0.08f, 0.10f, 1.0f};
    colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(
        swapchainCommand.get_command(), &colorTarget, 1, nullptr);
    SDL_EndGPURenderPass(renderPass);
  }

  return SDL_APP_CONTINUE;
}

void AppState::caption_glyphs_alloc() {
  hb_buffer_t *glyphBuffer = hb_buffer_create();

  hb_buffer_add_utf8(glyphBuffer, caption_string.data(), -1, 0, -1);
  hb_buffer_guess_segment_properties(glyphBuffer);
  hb_shape(app_font.get_font(), glyphBuffer, nullptr, 0);

  unsigned int glyphCount = 0;
  hb_glyph_info_t *glyphInfos =
      hb_buffer_get_glyph_infos(glyphBuffer, &glyphCount);
  hb_glyph_position_t *glyphPositions =
      hb_buffer_get_glyph_positions(glyphBuffer, &glyphCount);

  caption_glyphs = std::vector<GlyphData>(glyphCount);
  for (unsigned int i = 0; i < glyphCount; ++i) {
    caption_glyphs[i].codepoint = glyphInfos[i].codepoint;
    caption_glyphs[i].cluster = glyphInfos[i].cluster;
    caption_glyphs[i].x_advance = glyphPositions[i].x_advance;
    caption_glyphs[i].y_advance = glyphPositions[i].y_advance;
    caption_glyphs[i].x_offset = glyphPositions[i].x_offset;
    caption_glyphs[i].y_offset = glyphPositions[i].y_offset;
  }

  hb_buffer_destroy(glyphBuffer);
}

void AppState::caption_glyphs_render() {
  for (GlyphData &glyphData : caption_glyphs) {
    FT_Int32 glyphFlags =
        FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER;
    FT_Error glyphLoadErr =
        FT_Load_Glyph(app_font.get_face(), glyphData.codepoint, glyphFlags);
    if (glyphLoadErr != 0) {
      SDL_Log("FT_Load_Glyph failed for glyph: %d", glyphData.codepoint);
      continue;
    }
    FT_GlyphSlot glyphSlot = app_font.get_face()->glyph;
    glyphData.bitmap_left = glyphSlot->bitmap_left;
    glyphData.bitmap_top = glyphSlot->bitmap_top;
    const FT_Bitmap &glyphBitmap = glyphSlot->bitmap;
    glyphData.width = glyphBitmap.width;
    glyphData.height = glyphBitmap.rows;
    if (glyphData.width == 0 || glyphData.height == 0)
      continue;
    glyphData.bitmap.resize(glyphBitmap.width * glyphBitmap.rows);
    for (unsigned int y = 0; y < glyphBitmap.rows; ++y) {
      const uint8_t *rowSource =
          glyphBitmap.buffer + y * std::abs(glyphBitmap.pitch);
      uint8_t *rowTarget = glyphData.bitmap.data() + y * glyphBitmap.width;
      std::memcpy(rowTarget, rowSource, glyphBitmap.width);
    }
  }
}

void AppState::caption_glyphs_upload() {
  std::vector<std::optional<AppGPUUploadBuffer>> uploadBuffers(
      caption_glyphs.size());
  for (unsigned int i = 0; i < caption_glyphs.size(); ++i) {
    if (caption_glyphs[i].width == 0 || caption_glyphs[i].height == 0)
      continue;
    caption_glyphs[i].texture.emplace(app_device.get(), caption_glyphs[i].width,
                                      caption_glyphs[i].height);
    uploadBuffers[i].emplace(
        app_device.get(),
        static_cast<std::uint32_t>(caption_glyphs[i].bitmap.size()));
    std::memcpy(uploadBuffers[i]->get_mapped(), caption_glyphs[i].bitmap.data(),
                caption_glyphs[i].bitmap.size());
    uploadBuffers[i]->unmap();
  }

  AppGPUCopyCommand copyCommand{app_device.get()};
  for (unsigned int i = 0; i < caption_glyphs.size(); ++i) {
    if (not caption_glyphs[i].texture)
      continue;
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = uploadBuffers[i]->get_buffer();
    src.offset = 0;
    src.pixels_per_row = caption_glyphs[i].width;
    src.rows_per_layer = caption_glyphs[i].height;
    SDL_GPUTextureRegion dst{};
    dst.texture = caption_glyphs[i].texture->get();
    dst.w = caption_glyphs[i].width;
    dst.h = caption_glyphs[i].height;
    dst.d = 1;
    SDL_UploadToGPUTexture(copyCommand.get_copy_pass(), &src, &dst, false);
  }
}

void AppState::caption_glyphs_combine() {
  float penX = 0.0f;
  int minX = INT32_MAX, maxX = INT32_MIN;
  int minY = INT32_MAX, maxY = INT32_MIN;

  for (const GlyphData &glyphData : caption_glyphs) {
    if (glyphData.width > 0 && glyphData.height > 0) {
      int left = static_cast<int>(
          std::floor(penX + static_cast<float>(glyphData.x_offset) / 64.0f));
      left += glyphData.bitmap_left;
      int right = left + glyphData.width;
      int top = static_cast<int>(
          std::floor(-1.0f * static_cast<float>(glyphData.y_offset) / 64.0f));
      top -= glyphData.bitmap_top;
      int bottom = top + glyphData.height;

      minX = std::min(minX, left);
      maxX = std::max(maxX, right);
      minY = std::min(minY, top);
      maxY = std::max(maxY, bottom);
    }
    penX += static_cast<float>(glyphData.x_advance) / 64.0f;
  }

  int combinedWidth = maxX - minX;
  int combinedHeight = maxY - minY;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  if (not SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  if (FT_Init_FreeType(&global_ft_library)) {
    SDL_Log("FT_Init_FreeType failed!");
    return SDL_APP_FAILURE;
  }
  try {
    AppState *app = new AppState{global_ft_library};
    *appstate = app;
    app->caption_glyphs_alloc();
    app->caption_glyphs_render();
    app->caption_glyphs_upload();
    app->caption_glyphs_combine();
    return SDL_APP_CONTINUE;
  } catch (const std::runtime_error &e) {
    SDL_Log("[App] %s", e.what());
    return SDL_APP_FAILURE;
  }
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  delete app;
  if (global_ft_library) {
    FT_Done_FreeType(global_ft_library);
    global_ft_library = nullptr;
  }
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  AppState *app = (AppState *)appstate;
  return app->iterate();
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_KEY_DOWN)
    return SDL_APP_SUCCESS;
  else
    return SDL_APP_CONTINUE;
}
