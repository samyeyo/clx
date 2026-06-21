// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  sokol_clx.cpp · Sokol clx Example          │
// └─────────────────────────────────────────────┘

#include <clx.h>
#include <cstring>
#include <string>
#include <functional>
#include <cmath>

#define SOKOL_NO_ENTRY
#define SOKOL_GLCORE

#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_GL_IMPL
#define SOKOL_LOG_IMPL
#define SOKOL_GLUE_IMPL

#include "sokol_log.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "util/sokol_gl.h"

namespace {

clx::LState* g_L = nullptr;
clx::LValue g_init_fn;
clx::LValue g_frame_fn;
clx::LValue g_event_fn;

float g_color_r = 1.0f;
float g_color_g = 1.0f;
float g_color_b = 1.0f;
float g_color_a = 1.0f;

float g_clear_r = 0.15f;
float g_clear_g = 0.15f;
float g_clear_b = 0.15f;
float g_clear_a = 1.0f;


sg_image  g_pixels_image  = {0};
sg_view   g_pixels_view   = {0};
sg_sampler g_pixels_sampler = {0};

} 

//------------------ SOKOL: init_cb - sokol init callback, sets up graphics and runs Lua init function
static void init_cb(void* user_data) {
    g_L = static_cast<clx::LState*>(user_data);

    sg_desc sgdesc;
    std::memset(&sgdesc, 0, sizeof(sgdesc));
    sgdesc.environment = sglue_environment();
    sgdesc.logger.func = slog_func;
    sg_setup(&sgdesc);

    sgl_desc_t sgldesc;
    std::memset(&sgldesc, 0, sizeof(sgldesc));
    sgl_setup(&sgldesc);

    sg_sampler_desc smp_desc;
    std::memset(&smp_desc, 0, sizeof(smp_desc));
    smp_desc.min_filter = SG_FILTER_LINEAR;
    smp_desc.mag_filter = SG_FILTER_LINEAR;
    g_pixels_sampler = sg_make_sampler(&smp_desc);

    float w = static_cast<float>(sapp_width());
    float h = static_cast<float>(sapp_height());
    sgl_matrix_mode_projection();
    sgl_ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f);

    if (g_init_fn.type() == clx::LType::Function) {
        try {
            clx::call(g_L, g_init_fn);
        } catch (...) {}
    }
}

//------------------ SOKOL: frame_cb - sokol frame callback, runs Lua frame function and renders
static void frame_cb(void* user_data) {
    g_L = static_cast<clx::LState*>(user_data);

    
    if (g_frame_fn.type() == clx::LType::Function) {
        try {
            double dt = sapp_frame_duration();
            clx::call(g_L, g_frame_fn, dt);
        } catch (...) {}
    }

    
    sg_pass_action action;
    std::memset(&action, 0, sizeof(action));
    action.colors[0].load_action = SG_LOADACTION_CLEAR;
    action.colors[0].clear_value = { g_clear_r, g_clear_g, g_clear_b, g_clear_a };

    sg_pass pass;
    std::memset(&pass, 0, sizeof(pass));
    pass.action = action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);

    sgl_draw();
    sg_end_pass();
    sg_commit();
}

//------------------ SOKOL: event_cb - sokol event callback, dispatches input events to Lua
static void event_cb(const sapp_event* ev, void* user_data) {
    g_L = static_cast<clx::LState*>(user_data);
    if (g_event_fn.type() != clx::LType::Function) return;

    clx::LValue t = clx::table(g_L);

    
    const char* type_str = "unknown";
    switch (ev->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:    type_str = "key_down";    break;
        case SAPP_EVENTTYPE_KEY_UP:      type_str = "key_up";      break;
        case SAPP_EVENTTYPE_CHAR:        type_str = "char";        break;
        case SAPP_EVENTTYPE_MOUSE_DOWN:  type_str = "mouse_down";  break;
        case SAPP_EVENTTYPE_MOUSE_UP:    type_str = "mouse_up";    break;
        case SAPP_EVENTTYPE_MOUSE_MOVE:  type_str = "mouse_move";  break;
        case SAPP_EVENTTYPE_MOUSE_SCROLL:type_str = "mouse_scroll";break;
        case SAPP_EVENTTYPE_RESIZED:     type_str = "resized";     break;
        default: break;
    }
    clx::raw_set(g_L, t, "type", clx::string(g_L, type_str));

    
    const char* key_str = nullptr;
    switch (ev->key_code) {
        case SAPP_KEYCODE_SPACE:        key_str = "space";     break;
        case SAPP_KEYCODE_ESCAPE:       key_str = "escape";    break;
        case SAPP_KEYCODE_ENTER:        key_str = "enter";     break;
        case SAPP_KEYCODE_TAB:          key_str = "tab";       break;
        case SAPP_KEYCODE_BACKSPACE:    key_str = "backspace"; break;
        case SAPP_KEYCODE_W:            key_str = "w";         break;
        case SAPP_KEYCODE_S:            key_str = "s";         break;
        case SAPP_KEYCODE_UP:           key_str = "up";        break;
        case SAPP_KEYCODE_DOWN:         key_str = "down";      break;
        case SAPP_KEYCODE_LEFT:         key_str = "left";      break;
        case SAPP_KEYCODE_RIGHT:        key_str = "right";     break;
        case SAPP_KEYCODE_LEFT_SHIFT:   key_str = "lshift";    break;
        case SAPP_KEYCODE_RIGHT_SHIFT:  key_str = "rshift";    break;
        case SAPP_KEYCODE_LEFT_CONTROL: key_str = "lctrl";     break;
        case SAPP_KEYCODE_RIGHT_CONTROL:key_str = "rctrl";     break;
        case SAPP_KEYCODE_MINUS:        key_str = "-";         break;
        case SAPP_KEYCODE_EQUAL:        key_str = "=";         break;
        case SAPP_KEYCODE_0:            key_str = "0";         break;
        case SAPP_KEYCODE_1:            key_str = "1";         break;
        case SAPP_KEYCODE_2:            key_str = "2";         break;
        case SAPP_KEYCODE_3:            key_str = "3";         break;
        case SAPP_KEYCODE_4:            key_str = "4";         break;
        case SAPP_KEYCODE_5:            key_str = "5";         break;
        case SAPP_KEYCODE_6:            key_str = "6";         break;
        case SAPP_KEYCODE_7:            key_str = "7";         break;
        case SAPP_KEYCODE_8:            key_str = "8";         break;
        case SAPP_KEYCODE_9:            key_str = "9";         break;
        default: break;
    }
    if (key_str) {
        clx::raw_set(g_L, t, "key", clx::string(g_L, key_str));
    }

    
    if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN || ev->type == SAPP_EVENTTYPE_MOUSE_UP) {
        const char* btn = "left";
        switch (ev->mouse_button) {
            case SAPP_MOUSEBUTTON_LEFT:   btn = "left";   break;
            case SAPP_MOUSEBUTTON_RIGHT:  btn = "right";  break;
            case SAPP_MOUSEBUTTON_MIDDLE: btn = "middle"; break;
            default: break;
        }
        clx::raw_set(g_L, t, "button", clx::string(g_L, btn));
    }

    clx::raw_set(g_L, t, "x",        clx::number(static_cast<double>(ev->mouse_x)));
    clx::raw_set(g_L, t, "y",        clx::number(static_cast<double>(ev->mouse_y)));
    clx::raw_set(g_L, t, "dx",       clx::number(static_cast<double>(ev->mouse_dx)));
    clx::raw_set(g_L, t, "dy",       clx::number(static_cast<double>(ev->mouse_dy)));
    clx::raw_set(g_L, t, "scroll_x", clx::number(static_cast<double>(ev->scroll_x)));
    clx::raw_set(g_L, t, "scroll_y", clx::number(static_cast<double>(ev->scroll_y)));

    try {
        clx::call(g_L, g_event_fn, t);
    } catch (...) {}
}

//------------------ SOKOL: cleanup_cb - sokol cleanup callback, destroys GPU resources
static void cleanup_cb(void* user_data) {
    g_L = static_cast<clx::LState*>(user_data);
    g_init_fn = clx::nil();
    g_frame_fn = clx::nil();
    g_event_fn = clx::nil();
    sg_destroy_image(g_pixels_image);
    sg_destroy_view(g_pixels_view);
    sg_destroy_sampler(g_pixels_sampler);
    std::memset(&g_pixels_image, 0, sizeof(g_pixels_image));
    std::memset(&g_pixels_view, 0, sizeof(g_pixels_view));
    std::memset(&g_pixels_sampler, 0, sizeof(g_pixels_sampler));
    sgl_shutdown();
    sg_shutdown();
}





//------------------ SOKOL: run - sokol.run() Lua binding: starts sokol app with Lua callbacks
static clx::MultiValue run(clx::LState* L, const clx::LValue* args, size_t n) {
    if (n < 1 || !clx::is_table(args[0]))
        clx::error(L, "sokol.run: expected a config table");

    clx::LValue cfg = args[0];

    g_init_fn  = clx::raw_get(L, cfg, "init");
    g_frame_fn = clx::raw_get(L, cfg, "frame");
    g_event_fn = clx::raw_get(L, cfg, "event");

    int w = 800, h = 600;
    clx::LValue wv = clx::raw_get(L, cfg, "width");
    if (clx::is_number(wv)) w = static_cast<int>(wv.as_number());
    clx::LValue hv = clx::raw_get(L, cfg, "height");
    if (clx::is_number(hv)) h = static_cast<int>(hv.as_number());

    clx::LValue tv = clx::raw_get(L, cfg, "title");
    const char* title = tv.type() == clx::LType::String ? tv.as_string() : "Pong";

    sapp_desc desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.init_userdata_cb    = init_cb;
    desc.frame_userdata_cb   = frame_cb;
    desc.cleanup_userdata_cb = cleanup_cb;
    desc.event_userdata_cb   = event_cb;
    desc.user_data           = L;
    desc.width               = w;
    desc.height              = h;
    desc.window_title        = title;
    desc.enable_clipboard    = true;
    desc.logger.func         = slog_func;

    sapp_run(&desc);
    return clx::MultiValue();
}

//------------------ SOKOL: width - sokol.width() Lua binding: returns window width in pixels
static clx::MultiValue width(clx::LState*, const clx::LValue*, size_t) {
    return clx::integer(sapp_width());
}

//------------------ SOKOL: height - sokol.height() Lua binding: returns window height in pixels
static clx::MultiValue height(clx::LState*, const clx::LValue*, size_t) {
    return clx::integer(sapp_height());
}

//------------------ SOKOL: frame_duration - sokol.frame_duration() Lua binding: returns frame delta time
static clx::MultiValue frame_duration(clx::LState*, const clx::LValue*, size_t) {
    return clx::number(sapp_frame_duration());
}

//------------------ SOKOL: clear - sokol.clear() Lua binding: sets clear color (r, g, b, a)
static clx::MultiValue clear(clx::LState* L, const clx::LValue* args, size_t n) {
    g_clear_r = (n > 0) ? static_cast<float>(clx::check_number(L, args[0])) : 0.0f;
    g_clear_g = (n > 1) ? static_cast<float>(clx::check_number(L, args[1])) : 0.0f;
    g_clear_b = (n > 2) ? static_cast<float>(clx::check_number(L, args[2])) : 0.0f;
    g_clear_a = (n > 3) ? static_cast<float>(clx::check_number(L, args[3])) : 1.0f;
    return clx::MultiValue();
}

//------------------ SOKOL: color - sokol.color() Lua binding: sets current draw color (r, g, b, a)
static clx::MultiValue color(clx::LState* L, const clx::LValue* args, size_t n) {
    g_color_r = (n > 0) ? static_cast<float>(clx::check_number(L, args[0])) : 1.0f;
    g_color_g = (n > 1) ? static_cast<float>(clx::check_number(L, args[1])) : 1.0f;
    g_color_b = (n > 2) ? static_cast<float>(clx::check_number(L, args[2])) : 1.0f;
    g_color_a = (n > 3) ? static_cast<float>(clx::check_number(L, args[3])) : 1.0f;
    return clx::MultiValue();
}

//------------------ SOKOL: draw_rect - sokol.draw_rect() Lua binding: draws filled rectangle
static clx::MultiValue draw_rect(clx::LState* L, const clx::LValue* args, size_t n) {
    if (n < 4) clx::error(L, "draw_rect: expected x, y, w, h");
    float x = static_cast<float>(clx::check_number(L, args[0]));
    float y = static_cast<float>(clx::check_number(L, args[1]));
    float w = static_cast<float>(clx::check_number(L, args[2]));
    float h = static_cast<float>(clx::check_number(L, args[3]));
    sgl_c4f(g_color_r, g_color_g, g_color_b, g_color_a);
    sgl_begin_quads();
    sgl_v2f(x,   y);
    sgl_v2f(x+w, y);
    sgl_v2f(x+w, y+h);
    sgl_v2f(x,   y+h);
    sgl_end();
    return clx::MultiValue();
}

//------------------ SOKOL: draw_filled_circle - sokol.draw_filled_circle() Lua binding
static clx::MultiValue draw_filled_circle(clx::LState* L, const clx::LValue* args, size_t n) {
    if (n < 3) clx::error(L, "draw_filled_circle: expected cx, cy, r");
    float cx = static_cast<float>(clx::check_number(L, args[0]));
    float cy = static_cast<float>(clx::check_number(L, args[1]));
    float r  = static_cast<float>(clx::check_number(L, args[2]));
    int segs = (n > 3) ? static_cast<int>(clx::check_number(L, args[3])) : 24;
    sgl_c4f(g_color_r, g_color_g, g_color_b, g_color_a);
    sgl_begin_triangles();
    float step = 6.283185f / segs;
    for (int i = 0; i < segs; i++) {
        float a1 = step * i;
        float a2 = step * (i + 1);
        sgl_v2f(cx, cy);
        sgl_v2f(cx + r * std::cos(a1), cy + r * std::sin(a1));
        sgl_v2f(cx + r * std::cos(a2), cy + r * std::sin(a2));
    }
    sgl_end();
    return clx::MultiValue();
}

//------------------ SOKOL: pixels - sokol.pixels() Lua binding: upload raw RGBA pixel data and display
static clx::MultiValue pixels(clx::LState* L, const clx::LValue* args, size_t n) {
    if (n < 3) clx::error(L, "pixels: expected width, height, data_table");
    int w = static_cast<int>(clx::check_number(L, args[0]));
    int h = static_cast<int>(clx::check_number(L, args[1]));
    clx::LValue data = args[2];
    if (!clx::is_table(data))
        clx::error(L, "pixels: data must be a table of RGBA bytes {r,g,b,a, r,g,b,a, ...}");

    size_t count = static_cast<size_t>(w) * h * 4;
    std::vector<uint8_t> buf(count);
    for (size_t i = 0; i < count; i++)
        buf[i] = static_cast<uint8_t>(clx::check_number(L, clx::raw_get_i(L, data, static_cast<int64_t>(i + 1))));

    if (g_pixels_image.id == 0) {
        sg_image_desc img_desc;
        std::memset(&img_desc, 0, sizeof(img_desc));
        img_desc.width  = w;
        img_desc.height = h;
        img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        img_desc.usage.stream_update = true;
        g_pixels_image = sg_make_image(&img_desc);

        sg_image_data img_data;
        std::memset(&img_data, 0, sizeof(img_data));
        img_data.mip_levels[0] = { buf.data(), count };
        sg_update_image(g_pixels_image, &img_data);

        sg_view_desc view_desc;
        std::memset(&view_desc, 0, sizeof(view_desc));
        view_desc.texture.image = g_pixels_image;
        g_pixels_view = sg_make_view(&view_desc);
    } else {
        sg_image_data img_data;
        std::memset(&img_data, 0, sizeof(img_data));
        img_data.mip_levels[0] = { buf.data(), count };
        sg_update_image(g_pixels_image, &img_data);
    }

    sgl_enable_texture();
    sgl_texture(g_pixels_view, g_pixels_sampler);

    float sw = static_cast<float>(sapp_width());
    float sh = static_cast<float>(sapp_height());
    sgl_begin_quads();
    sgl_v2f_t2f(0,   0,  0, 1);
    sgl_v2f_t2f(sw,  0,  1, 1);
    sgl_v2f_t2f(sw,  sh, 1, 0);
    sgl_v2f_t2f(0,   sh, 0, 0);
    sgl_end();
    sgl_disable_texture();
    return clx::MultiValue();
}

static constexpr clx::LazyReg sokol_funcs[] = {
    { "run",                run },
    { "width",              width },
    { "height",             height },
    { "frame_duration",     frame_duration },
    { "clear",              clear },
    { "color",              color },
    { "draw_rect",          draw_rect },
    { "draw_filled_circle", draw_filled_circle },
    { "pixels",             pixels },
};

//------------------ SOKOL: luaopen_sokol_clx - module entry point, registers all sokol bindings
CLX_API clx::LValue luaopen_sokol_clx(clx::LState* L) {
    clx::LValue t = clx::table(L);
    clx::set_lazy_funcs(L, t, sokol_funcs, 9);
    return t;
}
