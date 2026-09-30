#include <cstdio>
#include <cstdlib>
#include <cstring>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include <freetype/freetype.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <vulkan/vulkan.h>

struct AppState {
  SDL_Window *window;
  SDL_GPUDevice *device;
  SDL_GPUGraphicsPipeline *pipeline;
  SDL_GPUTexture *textTexture;
  SDL_GPUSampler *sampler;
  int textureWidth, textureHeight;
};

static void teardown_app_state(AppState *app) {
  if (app->device && app->sampler)
    SDL_ReleaseGPUSampler(app->device, app->sampler);
  if (app->device && app->textTexture)
    SDL_ReleaseGPUTexture(app->device, app->textTexture);
  if (app->device && app->pipeline)
    SDL_ReleaseGPUGraphicsPipeline(app->device, app->pipeline);
  if (app->device && app->window)
    SDL_ReleaseWindowFromGPUDevice(app->device, app->window);
  if (app->device)
    SDL_DestroyGPUDevice(app->device);
  if (app->window)
    SDL_DestroyWindow(app->window);
}

struct TextBitmap {
  uint8_t *pixels;
  int width;
  int height;
};

static int render_text_bitmap(const char *font_path, const char *text, int size,
                              TextBitmap *out_bitmap) {
  FT_Library ft;
  if (FT_Init_FreeType(&ft))
    return 1;
  FT_Face face;
  if (FT_New_Face(ft, font_path, 0, &face)) {
    FT_Done_FreeType(ft);
    return 2;
  }
  FT_Set_Pixel_Sizes(face, 0, size);

  hb_font_t *hb_font = hb_ft_font_create(face, NULL);
  hb_buffer_t *buf = hb_buffer_create();

  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_buffer_guess_segment_properties(buf);
  hb_shape(hb_font, buf, NULL, 0);

  unsigned int glyph_count;
  hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
  hb_glyph_position_t *glyph_pos =
      hb_buffer_get_glyph_positions(buf, &glyph_count);

  int width = 0, height = 0;
  int pen_x = 0, pen_y = 0;
  int min_y = INT32_MAX, max_y = INT32_MIN;

  for (unsigned int i = 0; i < glyph_count; i++) {
    FT_Load_Glyph(face, glyph_info[i].codepoint, FT_LOAD_DEFAULT);
    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);

    int x = pen_x + glyph_pos[i].x_offset / 64;
    int y = pen_y - glyph_pos[i].y_offset / 64;

    int top = y - face->glyph->bitmap_top;
    int bottom = top + face->glyph->bitmap.rows;

    if (top < min_y)
      min_y = top;
    if (bottom > max_y)
      max_y = bottom;

    width = x + face->glyph->bitmap_left + face->glyph->bitmap.width;
    pen_x += glyph_pos[i].x_advance / 64;
    pen_y += glyph_pos[i].y_advance / 64;
  }

  height = max_y - min_y;

  size_t stride = width * 4;
  uint8_t *pixels = (uint8_t *)calloc(height * stride, 1);

  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) {
      uint8_t *p = pixels + y * stride + x * 4;
      p[0] = p[1] = p[2] = 0; // RGB black
      p[3] = 255;             // Alpha white (background)
    }

  pen_x = 0;
  pen_y = 0;
  for (unsigned int i = 0; i < glyph_count; i++) {
    FT_Load_Glyph(face, glyph_info[i].codepoint, FT_LOAD_DEFAULT);
    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);

    int x = pen_x + glyph_pos[i].x_offset / 64 + face->glyph->bitmap_left;
    int y =
        pen_y - glyph_pos[i].y_offset / 64 - face->glyph->bitmap_top - min_y;

    for (int row = 0; row < face->glyph->bitmap.rows; row++)
      for (int col = 0; col < face->glyph->bitmap.width; col++) {
        int px = x + col;
        int py = y + row;
        if (px >= 0 && px < width && py >= 0 && py < height) {
          uint8_t gray =
              face->glyph->bitmap.buffer[row * face->glyph->bitmap.width + col];
          uint8_t *p = pixels + py * stride + px * 4;
          // Black text with alpha = gray (255 = opaque)
          p[3] = 255 - gray; // alpha for blending
        }
      }

    pen_x += glyph_pos[i].x_advance / 64;
    pen_y += glyph_pos[i].y_advance / 64;
  }

  hb_buffer_destroy(buf);
  hb_font_destroy(hb_font);

  FT_Done_Face(face);
  FT_Done_FreeType(ft);

  out_bitmap->pixels = pixels;
  out_bitmap->width = width;
  out_bitmap->height = height;
  return 0;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  AppState *app = new AppState{};
  *appstate = app;

  app->window = SDL_CreateWindow("Hello GPU", 800, 600, SDL_WINDOW_RESIZABLE);
  if (!app->window) {
    SDL_Log("CreateWindow: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_GPUVulkanOptions vulkan_options{};
  vulkan_options.vulkan_api_version = VK_API_VERSION_1_3;

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
                        "vulkan");
  SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
                         true);
  SDL_SetBooleanProperty(
      props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
  SDL_SetPointerProperty(props,
                         SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER,
                         &vulkan_options);

  app->device = SDL_CreateGPUDeviceWithProperties(props);
  SDL_DestroyProperties(props);

  if (!app->device) {
    SDL_Log("Failed to create GPU device: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  SDL_ClaimWindowForGPUDevice(app->device, app->window);

  TextBitmap bitmap;
  if (render_text_bitmap("assets/DejaVuSans.ttf", "Hello world", 48, &bitmap)) {
    SDL_Log("Failed to render text bitmap");
    return SDL_APP_FAILURE;
  }

  size_t shader_size;
  void *shader_code = SDL_LoadFile("fullscreen.spv", &shader_size);
  if (!shader_code) {
    SDL_Log("Failed to load file: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  SDL_GPUShaderCreateInfo fs_info{
      .code_size = shader_size,
      .code = (const Uint8 *)shader_code,
      .entrypoint = "fs_main",
      .format = SDL_GPU_SHADERFORMAT_SPIRV,
      .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
      .num_samplers = 0,
      .num_storage_textures = 0,
      .num_storage_buffers = 0,
      .num_uniform_buffers = 0,
  };
  SDL_GPUShader *fs_program = SDL_CreateGPUShader(app->device, &fs_info);

  SDL_GPUShaderCreateInfo vs_info{
      .code_size = shader_size,
      .code = (const Uint8 *)shader_code,
      .entrypoint = "vs_main",
      .format = SDL_GPU_SHADERFORMAT_SPIRV,
      .stage = SDL_GPU_SHADERSTAGE_VERTEX,
      .num_samplers = 0,
      .num_storage_textures = 0,
      .num_storage_buffers = 0,
      .num_uniform_buffers = 0,
  };
  SDL_GPUShader *vs_program = SDL_CreateGPUShader(app->device, &vs_info);

  SDL_GPUColorTargetDescription colorTarget = {
      .format = SDL_GetGPUSwapchainTextureFormat(app->device, app->window),
  };
  SDL_GPUGraphicsPipelineTargetInfo targetInfo = {
      .color_target_descriptions = &colorTarget,
      .num_color_targets = 1,
  };
  SDL_GPUGraphicsPipelineCreateInfo pipeInfo = {
      .vertex_shader = vs_program,
      .fragment_shader = fs_program,
      .vertex_input_state = SDL_GPUVertexInputState{},
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL,
                           .cull_mode = SDL_GPU_CULLMODE_NONE},
      .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
      .target_info = targetInfo,
  };
  app->pipeline = SDL_CreateGPUGraphicsPipeline(app->device, &pipeInfo);

  SDL_ReleaseGPUShader(app->device, fs_program);
  SDL_ReleaseGPUShader(app->device, vs_program);
  SDL_free(shader_code);

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_KEY_DOWN) {
    return SDL_APP_SUCCESS;
  }
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  AppState *app = (AppState *)appstate;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(app->device);
  SDL_GPUTexture *swapchain;
  Uint32 sw, sh;
  SDL_WaitAndAcquireGPUSwapchainTexture(cmd, app->window, &swapchain, &sw, &sh);
  if (!swapchain) {
    SDL_SubmitGPUCommandBuffer(cmd);
    return SDL_APP_CONTINUE;
  }

  SDL_GPUColorTargetInfo colorTarget{
      .texture = swapchain,
      .clear_color = {1.0f, 1.0f, 1.0f, 1.0f},
      .load_op = SDL_GPU_LOADOP_CLEAR,
      .store_op = SDL_GPU_STOREOP_STORE,
  };
  SDL_GPURenderPass *pass =
      SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, app->pipeline);

  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
  SDL_SubmitGPUCommandBuffer(cmd);

  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  AppState *app = (AppState *)appstate;
  if (!app)
    return;
  teardown_app_state(app);
  delete app;
}
