#include "debug_overlay_window.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cairo-xlib.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace iroha {

namespace {
// 余白・カーソルとの間（論理ピクセル。macOS版と同じ）
constexpr double kPaddingX = 6;
constexpr double kPaddingY = 3;
constexpr int kCaretGap = 2;
// 文字の大きさ（ポイント。macOS版の smallSystemFontSize - 1 にあたる）
constexpr char kFont[] = "Sans 9";
} // namespace

DebugOverlayWindow::Rect PlaceDebugOverlay(const DebugOverlayWindow::Rect& caret, int width, int height,
                                           int screenWidth, int screenHeight, bool aboveLine) {
    DebugOverlayWindow::Rect frame{caret.x + caret.width + kCaretGap, caret.y + caret.height + kCaretGap, width,
                                   height};
    const int above = caret.y - kCaretGap - height;
    if (aboveLine || frame.y + height > screenHeight) frame.y = above;
    if (frame.y < 0) frame.y = caret.y + caret.height + kCaretGap; // 上にも収まらなければ下
    frame.x = std::max(0, std::min(frame.x, screenWidth - width));
    return frame;
}

struct DebugOverlayWindow::Impl {
    Display* display = nullptr;
    Window window = 0;
    cairo_surface_t* surface = nullptr;
    double scale = 1; // Xft.dpi / 96
    std::string text;
    int width = 1;
    int height = 1;

    PangoLayout* MakeLayout(cairo_t* cr) const {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_cairo_context_set_resolution(pango_layout_get_context(layout), 96.0 * scale);
        pango_cairo_update_layout(cr, layout);
        PangoFontDescription* font = pango_font_description_from_string(kFont);
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_text(layout, text.c_str(), -1);
        return layout;
    }

    // 文字の大きさから窓の大きさを決める
    void Measure() {
        cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* cr = cairo_create(scratch);
        PangoLayout* layout = MakeLayout(cr);
        int textWidth = 0;
        int textHeight = 0;
        pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_destroy(scratch);
        width = std::max(1, textWidth + static_cast<int>(2 * kPaddingX * scale));
        height = std::max(1, textHeight + static_cast<int>(2 * kPaddingY * scale));
    }

    void Draw() {
        if (!surface) return;
        cairo_t* cr = cairo_create(surface);
        // 背景（明るい面）と縁。文字は少し薄い灰色（macOS版の secondaryLabelColor）
        cairo_set_source_rgb(cr, 0.97, 0.97, 0.975);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 0.72, 0.73, 0.76);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, 0.5, 0.5, width - 1, height - 1);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0.30, 0.31, 0.34);
        cairo_move_to(cr, kPaddingX * scale, kPaddingY * scale);
        PangoLayout* layout = MakeLayout(cr);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        XFlush(display);
    }
};

DebugOverlayWindow::DebugOverlayWindow() : impl_(new Impl) {}

DebugOverlayWindow::~DebugOverlayWindow() {
    if (impl_->surface) cairo_surface_destroy(impl_->surface);
    if (impl_->display) {
        if (impl_->window) XDestroyWindow(impl_->display, impl_->window);
        XCloseDisplay(impl_->display);
    }
    delete impl_;
}

bool DebugOverlayWindow::Open() {
    if (opened_) return true;
    if (openFailed_) return false;
    const char* session = std::getenv("XDG_SESSION_TYPE");
    if ((session && std::strcmp(session, "wayland") == 0) || std::getenv("WAYLAND_DISPLAY")) {
        openFailed_ = true;
        return false;
    }
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        openFailed_ = true;
        return false;
    }
    impl_->display = display;
    const int screen = DefaultScreen(display);
    // 文字の大きさはデスクトップの倍率に合わせる（Xft.dpi。無ければ 96）
    if (const char* dpi = XGetDefault(display, "Xft", "dpi")) {
        const double value = std::atof(dpi);
        if (value >= 48 && value <= 480) impl_->scale = value / 96.0;
    }
    XSetWindowAttributes attributes = {};
    attributes.override_redirect = True;
    attributes.save_under = True;
    attributes.event_mask = ExposureMask;
    attributes.background_pixel = WhitePixel(display, screen);
    impl_->window = XCreateWindow(display, RootWindow(display, screen), 0, 0, 1, 1, 0, CopyFromParent, InputOutput,
                                  CopyFromParent, CWOverrideRedirect | CWSaveUnder | CWEventMask | CWBackPixel,
                                  &attributes);
    XStoreName(display, impl_->window, "iroha debug");
    // マウスを透過する（入力の形を空にする）
    int shapeEvent = 0;
    int shapeError = 0;
    if (XShapeQueryExtension(display, &shapeEvent, &shapeError)) {
        XShapeCombineRectangles(display, impl_->window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
    }
    impl_->surface = cairo_xlib_surface_create(display, impl_->window, DefaultVisual(display, screen), 1, 1);
    XFlush(display);
    opened_ = true;
    return true;
}

int DebugOverlayWindow::Fd() const { return opened_ ? ConnectionNumber(impl_->display) : -1; }

void DebugOverlayWindow::Dispatch() {
    if (!opened_) return;
    bool redraw = false;
    while (XPending(impl_->display) > 0) {
        XEvent event;
        XNextEvent(impl_->display, &event);
        if (event.type == Expose && event.xexpose.count == 0) redraw = true;
    }
    if (redraw && visible_) impl_->Draw();
}

void DebugOverlayWindow::Show(const std::string& text, const Rect& caret, bool aboveLine) {
    if (!Open()) return;
    Display* display = impl_->display;
    impl_->text = text;
    impl_->Measure();
    const int screen = DefaultScreen(display);
    const Rect frame = PlaceDebugOverlay(caret, impl_->width, impl_->height, DisplayWidth(display, screen),
                                         DisplayHeight(display, screen), aboveLine);
    XMoveResizeWindow(display, impl_->window, frame.x, frame.y, static_cast<unsigned>(frame.width),
                      static_cast<unsigned>(frame.height));
    cairo_xlib_surface_set_size(impl_->surface, frame.width, frame.height);
    if (!visible_) XMapRaised(display, impl_->window);
    else XRaiseWindow(display, impl_->window);
    visible_ = true;
    impl_->Draw();
    Dispatch();
}

void DebugOverlayWindow::Hide() {
    if (!opened_ || !visible_) return;
    XUnmapWindow(impl_->display, impl_->window);
    XFlush(impl_->display);
    visible_ = false;
}

} // namespace iroha
