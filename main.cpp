#include <array>
#include <cmath>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
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

struct GlyphTexture {
  SDL_GPUTexture *texture = nullptr;
  int width = 0;
  int height = 0;
  int bitmap_left = 0;
  int bitmap_top = 0;
  float x_advance = 0.0f;
  float x_offset = 0.0f;
  float y_offset = 0.0f;
};

class AppState {
private:
  SDL_GPUDevice *device = nullptr;
  SDL_Window *window = nullptr;
  SDL_GPUGraphicsPipeline *pipeline = nullptr;
  SDL_GPUSampler *sampler = nullptr;

  FT_Library ft_library = nullptr;
  hb_font_t *hb_font = nullptr;
  hb_buffer_t *textBuffer = nullptr;
  std::string textString = "Hello world";

  std::expected<void, std::string>
  build_glyph_textures(std::vector<GlyphTexture> &glyph_textures);
  std::expected<void, std::string>
  build_combined_text_texture(const std::vector<GlyphTexture> &glyph_textures);

  std::expected<void, std::string> initialize_font();
  std::expected<void, std::string> initialize_device();
  std::expected<void, std::string> initialize_window();
  std::expected<void, std::string> initialize_sampler();
  std::expected<void, std::string> initialize_pipeline();

  void destroy_font();
  void destroy_freetype();
  void destroy_device();
  void destroy_window();
  void destroy_sampler();
  void destroy_pipeline();

public:
  SDL_AppResult initialize();
  SDL_AppResult iterate();
  void quit();
};

std::expected<void, std::string> AppState::initialize_font() {
  FT_Face ft_face{};
  if (FT_New_Face(ft_library, "assets/DejaVuSans.ttf", 0, &ft_face))
    return std::unexpected{"FT_New_Face failed!"};

  FT_Set_Pixel_Sizes(ft_face, 0, 14);

  hb_font = hb_ft_font_create_referenced(ft_face);
  FT_Done_Face(ft_face);

  textBuffer = hb_buffer_create();
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_device() {
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
  device = SDL_CreateGPUDeviceWithProperties(device_props);

  SDL_DestroyProperties(device_props);

  if (not device)
    return std::unexpected{
        std::format("Failed to create GPU device: {}", SDL_GetError())};
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_window() {
  window =
      SDL_CreateWindow("Hello GPU", 800, 600,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (not window)
    return std::unexpected{std::format("CreateWindow: {}", SDL_GetError())};
  if (not SDL_ClaimWindowForGPUDevice(device, window))
    return std::unexpected{
        std::format("ClaimWindowForGPUDevice: {}", SDL_GetError())};
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_sampler() {
  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

  sampler = SDL_CreateGPUSampler(device, &samplerInfo);
  if (not sampler)
    return std::unexpected{
        std::format("SDL_CreateGPUSampler failed: {}", SDL_GetError())};
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::initialize_pipeline() {
  struct CommonDeleter {
    void operator()(void *m) const {
      if (m)
        SDL_free(m);
    }
  };
  struct ShaderDeleter {
    SDL_GPUDevice *device;
    void operator()(SDL_GPUShader *shader) const {
      if (shader)
        SDL_ReleaseGPUShader(device, shader);
    }
  };

  std::unique_ptr<SDL_GPUShader, ShaderDeleter> vertex_shader{
      nullptr, ShaderDeleter{device}};
  {
    size_t shader_size{};
    std::unique_ptr<void, CommonDeleter> shader_code{
        SDL_LoadFile("text.vert.spv", &shader_size)};
    if (not shader_code)
      return std::unexpected{
          std::format("Failed to load vertex shader: {}", SDL_GetError())};

    SDL_GPUShaderCreateInfo shader_info{};
    shader_info.code_size = shader_size;
    shader_info.code = static_cast<const Uint8 *>(shader_code.get());
    shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shader_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    shader_info.num_samplers = 0;
    shader_info.num_storage_textures = 0;
    shader_info.num_storage_buffers = 0;
    shader_info.num_uniform_buffers = 1;
    vertex_shader.reset(SDL_CreateGPUShader(device, &shader_info));
    if (not vertex_shader)
      return std::unexpected{std::format(
          "SDL_CreateGPUShader for vertex failed: {}", SDL_GetError())};
  }

  std::unique_ptr<SDL_GPUShader, ShaderDeleter> fragment_shader{
      nullptr, ShaderDeleter{device}};
  {
    size_t shader_size{};
    std::unique_ptr<void, CommonDeleter> shader_code{
        SDL_LoadFile("text.frag.spv", &shader_size)};
    if (not shader_code)
      return std::unexpected{
          std::format("Failed to load fragment shader: {}", SDL_GetError())};

    SDL_GPUShaderCreateInfo shader_info{};
    shader_info.code_size = shader_size;
    shader_info.code = static_cast<const Uint8 *>(shader_code.get());
    shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    shader_info.num_samplers = 1; // combined image sampler
    shader_info.num_storage_textures = 0;
    shader_info.num_storage_buffers = 0;
    shader_info.num_uniform_buffers = 0;
    fragment_shader.reset(SDL_CreateGPUShader(device, &shader_info));
    if (not fragment_shader)
      return std::unexpected{std::format(
          "SDL_CreateGPUShader for fragment failed: {}", SDL_GetError())};
  }

  std::array<SDL_GPUVertexAttribute, 2> attrs{};
  attrs[0].location = 0;
  attrs[0].buffer_slot = 0;
  attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[0].offset = offsetof(Vertex, position);

  attrs[1].location = 1;
  attrs[1].buffer_slot = 0;
  attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[1].offset = offsetof(Vertex, uv);

  SDL_GPUVertexBufferDescription vtxDesc{};
  vtxDesc.slot = 0;
  vtxDesc.pitch = sizeof(Vertex);
  vtxDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

  SDL_GPUVertexInputState vtxInput{};
  vtxInput.vertex_buffer_descriptions = &vtxDesc;
  vtxInput.num_vertex_buffers = 1;
  vtxInput.vertex_attributes = attrs.data();
  vtxInput.num_vertex_attributes = 2;

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
  pipeInfo.vertex_shader = vertex_shader.get();
  pipeInfo.fragment_shader = fragment_shader.get();
  pipeInfo.vertex_input_state = vtxInput;
  pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pipeInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  pipeInfo.target_info = targetInfo;

  pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeInfo);
  if (not pipeline)
    return std::unexpected{std::format(
        "SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError())};

  return std::expected<void, std::string>{};
}

SDL_AppResult AppState::initialize() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  } else if (FT_Init_FreeType(&ft_library)) {
    SDL_Log("FT_Init_FreeType failed!");
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_font(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_device(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_window(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_sampler(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else if (std::expected result = initialize_pipeline(); not result) {
    SDL_Log("%s", result.error().data());
    return SDL_APP_FAILURE;
  } else
    return SDL_APP_CONTINUE;
}

void AppState::quit() {
  destroy_pipeline();
  destroy_sampler();
  destroy_window();
  destroy_device();
  destroy_font();
  destroy_freetype();
  SDL_Quit();
}

void AppState::destroy_font() {
  if (not hb_font)
    return;
  hb_buffer_destroy(textBuffer);
  hb_font_destroy(hb_font);
}

void AppState::destroy_freetype() {
  if (not ft_library)
    return;
  FT_Done_FreeType(ft_library);
}

void AppState::destroy_device() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  SDL_DestroyGPUDevice(device);
}

void AppState::destroy_window() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  if (not window)
    return;
  SDL_ReleaseWindowFromGPUDevice(device, window);
  SDL_DestroyWindow(window);
}

void AppState::destroy_sampler() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  if (not sampler)
    return;
  SDL_ReleaseGPUSampler(device, sampler);
}

void AppState::destroy_pipeline() {
  if (not device)
    return;
  SDL_WaitForGPUIdle(device);
  if (not sampler)
    return;
  SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
}

SDL_AppResult AppState::iterate() {
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  if (not cmd)
    return SDL_APP_FAILURE;

  SDL_GPUTexture *swapchain = nullptr;
  Uint32 sw = 0, sh = 0;
  SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &sw, &sh);
  if (not swapchain) {
    SDL_SubmitGPUCommandBuffer(cmd);
    return SDL_APP_CONTINUE;
  }

  SDL_GPUColorTargetInfo colorTarget{};
  colorTarget.texture = swapchain;
  colorTarget.clear_color = {0.08f, 0.08f, 0.10f, 1.0f};
  colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
  colorTarget.store_op = SDL_GPU_STOREOP_STORE;

  SDL_GPURenderPass *renderPass =
      SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);
  SDL_EndGPURenderPass(renderPass);

  SDL_SubmitGPUCommandBuffer(cmd);
  return SDL_APP_CONTINUE;
}

std::expected<void, std::string>
AppState::build_glyph_textures(std::vector<GlyphTexture> &glyph_textures) {
  hb_buffer_reset(textBuffer);
  hb_buffer_add_utf8(textBuffer, textString.data(), -1, 0, -1);
  hb_buffer_guess_segment_properties(textBuffer);
  hb_shape(hb_font, textBuffer, nullptr, 0);

  unsigned int glyph_count = 0;
  hb_glyph_info_t *glyph_info =
      hb_buffer_get_glyph_infos(textBuffer, &glyph_count);
  hb_glyph_position_t *glyph_pos =
      hb_buffer_get_glyph_positions(textBuffer, &glyph_count);

  std::vector<GlyphTexture> local_textures{};
  std::vector<SDL_GPUTransferBuffer *> pending_tbs{};
  local_textures.reserve(glyph_count);
  pending_tbs.reserve(glyph_count);

  // RAII cleanup that runs on any early return.
  auto cleanup = [&]() {
    for (SDL_GPUTransferBuffer *tb : pending_tbs)
      SDL_ReleaseGPUTransferBuffer(device, tb);
    for (GlyphTexture &g : local_textures)
      if (g.texture)
        SDL_ReleaseGPUTexture(device, g.texture);
  };

  // ---- Pass 1: rasterize, create textures, fill transfer buffers ----
  FT_Face face = hb_ft_font_get_ft_face(hb_font);
  for (unsigned int i = 0; i < glyph_count; ++i) {
    if (FT_Load_Glyph(face, glyph_info[i].codepoint, FT_LOAD_DEFAULT) != 0) {
      cleanup();
      return std::unexpected{
          std::format("FT_Load_Glyph failed for glyph {}", i)};
    }

    FT_GlyphSlot slot = face->glyph;

    GlyphTexture gtex{};
    gtex.bitmap_left = slot->bitmap_left;
    gtex.bitmap_top = slot->bitmap_top;
    gtex.x_advance = static_cast<float>(glyph_pos[i].x_advance) / 64.0f;
    gtex.x_offset = static_cast<float>(glyph_pos[i].x_offset) / 64.0f;
    gtex.y_offset = static_cast<float>(glyph_pos[i].y_offset) / 64.0f;

    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
      cleanup();
      return std::unexpected{
          std::format("FT_Render_Glyph failed for glyph {}", i)};
    }

    FT_Bitmap &bmp = slot->bitmap;
    gtex.width = static_cast<int>(bmp.width);
    gtex.height = static_cast<int>(bmp.rows);

    if (gtex.width == 0 || gtex.height == 0) {
      // Space or zero-ink glyph: keep metrics, no texture.
      local_textures.push_back(gtex);
      pending_tbs.push_back(nullptr);
      continue;
    }

    // Create an R8 texture for this glyph.
    SDL_GPUTextureCreateInfo tex_info{};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_info.width = static_cast<Uint32>(gtex.width);
    tex_info.height = static_cast<Uint32>(gtex.height);
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    gtex.texture = SDL_CreateGPUTexture(device, &tex_info);
    if (not gtex.texture) {
      cleanup();
      return std::unexpected{
          std::format("SDL_CreateGPUTexture failed: {}", SDL_GetError())};
    }
    local_textures.push_back(gtex);

    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_info.size = static_cast<Uint32>(gtex.width) * gtex.height;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tb_info);
    pending_tbs.push_back(tb);

    void *mapped = SDL_MapGPUTransferBuffer(device, tb, false);
    // Copy row-by-row to strip FreeType's pitch padding.
    uint8_t *dst_base = static_cast<uint8_t *>(mapped);
    for (int row = 0; row < gtex.height; ++row) {
      const uint8_t *src_row = bmp.buffer + row * bmp.pitch;
      uint8_t *dst_row = dst_base + static_cast<size_t>(row) * gtex.width;
      memcpy(dst_row, src_row, gtex.width);
    }
    SDL_UnmapGPUTransferBuffer(device, tb);
  }

  // ---- Pass 2: single command buffer, single copy pass, N uploads ----
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

  for (size_t i = 0; i < local_textures.size(); ++i) {
    GlyphTexture &gtex = local_textures[i];
    if (gtex.texture == nullptr)
      continue;

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = pending_tbs[i];
    src.offset = 0;
    src.pixels_per_row = static_cast<Uint32>(gtex.width);
    src.rows_per_layer = static_cast<Uint32>(gtex.height);

    SDL_GPUTextureRegion dst{};
    dst.texture = gtex.texture;
    dst.w = static_cast<Uint32>(gtex.width);
    dst.h = static_cast<Uint32>(gtex.height);
    dst.d = 1;

    SDL_UploadToGPUTexture(copy, &src, &dst, false);
  }

  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);
  for (SDL_GPUTransferBuffer *tb : pending_tbs)
    SDL_ReleaseGPUTransferBuffer(device, tb);

  glyph_textures = std::move(local_textures);
  return std::expected<void, std::string>{};
}

std::expected<void, std::string> AppState::build_combined_text_texture(
    const std::vector<GlyphTexture> &glyph_textures) {

  if (glyph_textures.empty())
    return std::unexpected{"no glyphs to combine"};

  float pen_x = 0.0f;
  int min_x = INT32_MAX, max_x = INT32_MIN;
  int min_y = INT32_MAX, max_y = INT32_MIN;

  for (const auto &g : glyph_textures) {
    if (g.width > 0 && g.height > 0) {
      float x = pen_x + g.x_offset;
      float y = -g.y_offset; // pen_y == 0 for horizontal text

      int left = static_cast<int>(std::floor(x)) + g.bitmap_left;
      int right = left + g.width;
      int top = static_cast<int>(std::floor(y)) - g.bitmap_top;
      int bottom = top + g.height;

      min_x = std::min(min_x, left);
      max_x = std::max(max_x, right);
      min_y = std::min(min_y, top);
      max_y = std::max(max_y, bottom);
    }
    pen_x += g.x_advance;
  }

  if (min_x == INT32_MAX)
    return std::unexpected{"no inked glyphs to combine"};

  int new_width = max_x - min_x;
  int new_height = max_y - min_y;
  if (new_width <= 0 || new_height <= 0)
    return std::unexpected{"computed text size is empty"};

  SDL_GPUTextureCreateInfo tex_info{};
  tex_info.type = SDL_GPU_TEXTURETYPE_2D;
  tex_info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
  tex_info.usage =
      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  tex_info.width = static_cast<Uint32>(new_width);
  tex_info.height = static_cast<Uint32>(new_height);
  tex_info.layer_count_or_depth = 1;
  tex_info.num_levels = 1;
  tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

  SDL_GPUTexture *new_texture = SDL_CreateGPUTexture(device, &tex_info);
  if (!new_texture)
    return std::unexpected{
        std::format("SDL_CreateGPUTexture failed: {}", SDL_GetError())};

  auto cleanup_texture = [&]() { SDL_ReleaseGPUTexture(device, new_texture); };

  std::vector<Vertex> vertices;
  vertices.reserve(glyph_textures.size() * 6);

  pen_x = 0.0f;
  for (const auto &g : glyph_textures) {
    if (g.width > 0 && g.height > 0) {
      float x = pen_x + g.x_offset;
      float y = -g.y_offset;

      // Top-left of this glyph's quad in combined-texture space.
      float qx = x + static_cast<float>(g.bitmap_left - min_x);
      float qy =
          y - static_cast<float>(g.bitmap_top) - static_cast<float>(min_y);
      float qw = static_cast<float>(g.width);
      float qh = static_cast<float>(g.height);

      Vertex tl{qx, qy, 0.0f, 0.0f};
      Vertex tr{qx + qw, qy, 1.0f, 0.0f};
      Vertex bl{qx, qy + qh, 0.0f, 1.0f};
      Vertex br{qx + qw, qy + qh, 1.0f, 1.0f};

      vertices.push_back(tl);
      vertices.push_back(tr);
      vertices.push_back(bl);
      vertices.push_back(tr);
      vertices.push_back(br);
      vertices.push_back(bl);
    }
    pen_x += g.x_advance;
  }

  if (vertices.empty()) {
    cleanup_texture();
    return std::unexpected{"no inked glyphs produced vertices"};
  }

  const Uint32 vertex_bytes =
      static_cast<Uint32>(vertices.size() * sizeof(Vertex));

  SDL_GPUBufferCreateInfo vb_info{};
  vb_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
  vb_info.size = vertex_bytes;

  SDL_GPUBuffer *vb = SDL_CreateGPUBuffer(device, &vb_info);
  if (!vb) {
    cleanup_texture();
    return std::unexpected{
        std::format("SDL_CreateGPUBuffer failed: {}", SDL_GetError())};
  }

  auto cleanup_all = [&]() {
    SDL_ReleaseGPUBuffer(device, vb);
    cleanup_texture();
  };

  SDL_GPUTransferBufferCreateInfo tb_info{};
  tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  tb_info.size = vertex_bytes;

  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tb_info);
  if (!tb) {
    cleanup_all();
    return std::unexpected{
        std::format("SDL_CreateGPUTransferBuffer failed: {}", SDL_GetError())};
  }

  void *mapped = SDL_MapGPUTransferBuffer(device, tb, false);
  std::memcpy(mapped, vertices.data(), vertex_bytes);
  SDL_UnmapGPUTransferBuffer(device, tb);

  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);

  SDL_GPUTransferBufferLocation copy_src{};
  copy_src.transfer_buffer = tb;
  copy_src.offset = 0;

  SDL_GPUBufferRegion copy_dst{};
  copy_dst.buffer = vb;
  copy_dst.offset = 0;
  copy_dst.size = vertex_bytes;

  SDL_UploadToGPUBuffer(copy_pass, &copy_src, &copy_dst, false);
  SDL_EndGPUCopyPass(copy_pass);

  float viewport[2] = {static_cast<float>(new_width),
                       static_cast<float>(new_height)};
  SDL_PushGPUVertexUniformData(cmd, 0, viewport, sizeof(viewport));

  SDL_GPUColorTargetInfo color_target{};
  color_target.texture = new_texture;
  color_target.mip_level = 0;
  color_target.layer_or_depth_plane = 0;
  color_target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
  color_target.load_op = SDL_GPU_LOADOP_CLEAR;
  color_target.store_op = SDL_GPU_STOREOP_STORE;

  SDL_GPURenderPass *pass =
      SDL_BeginGPURenderPass(cmd, &color_target, 1, nullptr);

  SDL_BindGPUGraphicsPipeline(pass, glyph_pipeline);

  SDL_GPUBufferBinding vb_binding{};
  vb_binding.buffer = vb;
  vb_binding.offset = 0;
  SDL_BindGPUVertexBuffers(pass, 0, &vb_binding, 1);

  // One draw per inked glyph, each with its own source texture bound.
  Uint32 vertex_offset = 0;
  for (const auto &g : glyph_textures) {
    if (g.width > 0 && g.height > 0 && g.texture) {
      SDL_GPUTextureSamplerBinding sb{};
      sb.texture = g.texture;
      sb.sampler = glyph_sampler;
      SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
      SDL_DrawGPUPrimitives(pass, 6, 1, vertex_offset, 0);
    }
    vertex_offset += 6;
  }

  SDL_EndGPURenderPass(pass);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(device, tb);
  SDL_ReleaseGPUBuffer(device, vb);

  if (text_cache_texture)
    SDL_ReleaseGPUTexture(device, text_cache_texture);

  text_cache_texture = new_texture;
  text_cache_width = new_width;
  text_cache_height = new_height;

  return std::expected<void, std::string>{};
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  AppState *app = new AppState{};
  *appstate = app;
  return app->initialize();
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  app->quit();
  delete app;
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
