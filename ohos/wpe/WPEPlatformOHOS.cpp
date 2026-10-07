/*
 * Площадка вывода WPE для OpenHarmony.
 *
 * Подключаемая часть: кладётся в /system/lib64/wpe-platform-2.0/modules/,
 * выбирается переменной окружения WPE_PLATFORM=ohos. Исходники WebKit при
 * этом не меняются вовсе — площадки ищутся по каталогу.
 *
 * Окно площадка получает не сама, а по номеру поверхности, который ей даёт
 * хозяин через WPE_OHOS_SURFACE_ID. Так она одинаково работает и под
 * самостоятельным слоем, и под окном оконного распорядителя, и под
 * XComponent в приложении.
 *
 * Отрисовка устроена просто: WPE отдаёт готовый кадр, мы просим у него
 * картинку для EGL (wpe_buffer_import_to_egl_image — он сам разберётся,
 * лежит ли кадр в dma-buf или в общей памяти), натягиваем её на
 * прямоугольник во всё окно и отдаём кадр системе.
 *
 * Переменные окружения:
 *   WPE_OHOS_SURFACE_ID  номер поверхности (обязательно)
 *   WPE_OHOS_WIDTH       ширина окна, по умолчанию 1920
 *   WPE_OHOS_HEIGHT      высота окна, по умолчанию 1080
 *   WPE_OHOS_DEBUG       если задана — говорить о каждом шаге
 */

#include <gio/gio.h>
#include <wpe/wpe-platform.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <native_window/external_window.h>

#include <cstdlib>
#include <cstring>

#define OHOS_LOG(...) do { if (g_getenv("WPE_OHOS_DEBUG")) g_printerr(__VA_ARGS__); } while (0)

// Этого вызова нет в обычных объявлениях GLES2 — берём у EGL по имени.
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture2D;

// ============================================================================
//  Объявление видов
// ============================================================================

#define WPE_TYPE_DISPLAY_OHOS (wpe_display_ohos_get_type())
G_DECLARE_FINAL_TYPE(WPEDisplayOHOS, wpe_display_ohos, WPE, DISPLAY_OHOS, WPEDisplay)

#define WPE_TYPE_TOPLEVEL_OHOS (wpe_toplevel_ohos_get_type())
G_DECLARE_FINAL_TYPE(WPEToplevelOHOS, wpe_toplevel_ohos, WPE, TOPLEVEL_OHOS, WPEToplevel)

#define WPE_TYPE_VIEW_OHOS (wpe_view_ohos_get_type())
G_DECLARE_FINAL_TYPE(WPEViewOHOS, wpe_view_ohos, WPE, VIEW_OHOS, WPEView)

struct _WPEDisplayOHOS {
    WPEDisplay parent;

    EGLDisplay eglDisplay;
    guint64 surfaceId;
    OHNativeWindow* window;
    int width;
    int height;
    WPEKeymap* keymap;
};
G_DEFINE_TYPE(WPEDisplayOHOS, wpe_display_ohos, WPE_TYPE_DISPLAY)

struct _WPEToplevelOHOS {
    WPEToplevel parent;
};
G_DEFINE_TYPE(WPEToplevelOHOS, wpe_toplevel_ohos, WPE_TYPE_TOPLEVEL)

struct _WPEViewOHOS {
    WPEView parent;

    EGLSurface eglSurface;
    EGLContext eglContext;
    GLuint program;
    GLuint texture;
    gboolean glReady;
    WPEBuffer* committedBuffer;
};
G_DEFINE_TYPE(WPEViewOHOS, wpe_view_ohos, WPE_TYPE_VIEW)

// ============================================================================
//  Мелкая помощь по OpenGL
// ============================================================================

static const char* kVertexShader =
    "attribute vec2 position;\n"
    "attribute vec2 texCoord;\n"
    "varying vec2 v_texCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    v_texCoord = texCoord;\n"
    "}\n";

static const char* kFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(tex, v_texCoord);\n"
    "}\n";

static GLuint compileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { 0 };
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        g_warning("WPEPlatformOHOS: не собралась часть рисовальщика: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// ============================================================================
//  Площадка: обязанности
// ============================================================================

static gpointer wpeDisplayOHOSGetEGLDisplay(WPEDisplay* display, GError** error)
{
    WPEDisplayOHOS* self = WPE_DISPLAY_OHOS(display);
    if (self->eglDisplay != EGL_NO_DISPLAY)
        return self->eglDisplay;

    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY || !eglInitialize(eglDisplay, nullptr, nullptr)) {
        g_set_error(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE,
            "подключение к EGL не получено: 0x%04x", eglGetError());
        return nullptr;
    }
    self->eglDisplay = eglDisplay;
    return eglDisplay;
}

static gboolean wpeDisplayOHOSConnect(WPEDisplay* display, GError** error)
{
    WPEDisplayOHOS* self = WPE_DISPLAY_OHOS(display);

    const char* idText = g_getenv("WPE_OHOS_SURFACE_ID");
    if (!idText || !*idText) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "не задан номер поверхности: нужна переменная WPE_OHOS_SURFACE_ID");
        return FALSE;
    }
    self->surfaceId = g_ascii_strtoull(idText, nullptr, 10);

    const char* widthText = g_getenv("WPE_OHOS_WIDTH");
    const char* heightText = g_getenv("WPE_OHOS_HEIGHT");
    self->width = widthText ? atoi(widthText) : 1920;
    self->height = heightText ? atoi(heightText) : 1080;

    int ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId(self->surfaceId, &self->window);
    if (ret != 0 || !self->window) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "окно из поверхности %" G_GUINT64_FORMAT " не получилось: %d", self->surfaceId, ret);
        return FALSE;
    }

    // Размер буфера надо задать до того, как у окна попросят поверхность EGL,
    // иначе слой связи с Vulkan не заведёт ни одного образа для обмена.
    OH_NativeWindow_NativeWindowHandleOpt(self->window, SET_BUFFER_GEOMETRY, self->width, self->height);

    OHOS_LOG("WPEPlatformOHOS: окно %p из поверхности %" G_GUINT64_FORMAT ", %dx%d\n",
        (void*)self->window, self->surfaceId, self->width, self->height);
    return TRUE;
}

static WPEView* wpeDisplayOHOSCreateView(WPEDisplay* display)
{
    return WPE_VIEW(g_object_new(WPE_TYPE_VIEW_OHOS, "display", display, nullptr));
}

static WPEToplevel* wpeDisplayOHOSCreateToplevel(WPEDisplay* display, guint maxViews)
{
    return WPE_TOPLEVEL(g_object_new(WPE_TYPE_TOPLEVEL_OHOS,
        "display", display, "max-views", maxViews, nullptr));
}

static WPEKeymap* wpeDisplayOHOSGetKeymap(WPEDisplay* display)
{
    WPEDisplayOHOS* self = WPE_DISPLAY_OHOS(display);
    if (!self->keymap)
        self->keymap = wpe_keymap_xkb_new();
    return self->keymap;
}

static void wpeDisplayOHOSDispose(GObject* object)
{
    WPEDisplayOHOS* self = WPE_DISPLAY_OHOS(object);
    g_clear_object(&self->keymap);
    if (self->window) {
        OH_NativeWindow_DestroyNativeWindow(self->window);
        self->window = nullptr;
    }
    G_OBJECT_CLASS(wpe_display_ohos_parent_class)->dispose(object);
}

static void wpe_display_ohos_init(WPEDisplayOHOS* self)
{
    self->eglDisplay = EGL_NO_DISPLAY;
}

static void wpe_display_ohos_class_init(WPEDisplayOHOSClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = wpeDisplayOHOSDispose;

    WPEDisplayClass* displayClass = WPE_DISPLAY_CLASS(klass);
    displayClass->connect = wpeDisplayOHOSConnect;
    displayClass->create_view = wpeDisplayOHOSCreateView;
    displayClass->create_toplevel = wpeDisplayOHOSCreateToplevel;
    displayClass->get_egl_display = wpeDisplayOHOSGetEGLDisplay;
    displayClass->get_keymap = wpeDisplayOHOSGetKeymap;
}

// ============================================================================
//  Верхний уровень: обязанности
// ============================================================================

static gboolean wpeToplevelOHOSResize(WPEToplevel* toplevel, int width, int height)
{
    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_toplevel_get_display(toplevel));
    if (display->window)
        OH_NativeWindow_NativeWindowHandleOpt(display->window, SET_BUFFER_GEOMETRY, width, height);
    wpe_toplevel_resized(toplevel, width, height);
    return TRUE;
}

static void wpe_toplevel_ohos_init(WPEToplevelOHOS*)
{
}

static void wpe_toplevel_ohos_class_init(WPEToplevelOHOSClass* klass)
{
    WPE_TOPLEVEL_CLASS(klass)->resize = wpeToplevelOHOSResize;
}

// ============================================================================
//  Окно: обязанности
// ============================================================================

static gboolean viewEnsureGL(WPEViewOHOS* self, GError** error)
{
    if (self->glReady)
        return TRUE;

    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_view_get_display(WPE_VIEW(self)));
    EGLDisplay eglDisplay = (EGLDisplay)wpeDisplayOHOSGetEGLDisplay(WPE_DISPLAY(display), error);
    if (!eglDisplay)
        return FALSE;

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "подходящего набора свойств EGL не нашлось: 0x%04x", eglGetError());
        return FALSE;
    }

    self->eglSurface = eglCreateWindowSurface(eglDisplay, config,
        (EGLNativeWindowType)display->window, nullptr);
    if (self->eglSurface == EGL_NO_SURFACE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "оконная поверхность EGL не создалась: 0x%04x", eglGetError());
        return FALSE;
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    self->eglContext = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (self->eglContext == EGL_NO_CONTEXT) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "окружение отрисовки не создалось: 0x%04x", eglGetError());
        return FALSE;
    }

    if (!eglMakeCurrent(eglDisplay, self->eglSurface, self->eglSurface, self->eglContext)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "окружение отрисовки не выбралось: 0x%04x", eglGetError());
        return FALSE;
    }

    if (!imageTargetTexture2D) {
        imageTargetTexture2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
    }
    if (!imageTargetTexture2D) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "видеоподсистема не умеет превращать картинку EGL в полотно");
        return FALSE;
    }

    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertexShader || !fragmentShader) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "рисовальщик не собрался");
        return FALSE;
    }
    self->program = glCreateProgram();
    glAttachShader(self->program, vertexShader);
    glAttachShader(self->program, fragmentShader);
    glBindAttribLocation(self->program, 0, "position");
    glBindAttribLocation(self->program, 1, "texCoord");
    glLinkProgram(self->program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(self->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = { 0 };
        glGetProgramInfoLog(self->program, sizeof(log) - 1, nullptr, log);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "рисовальщик не связался: %s", log);
        return FALSE;
    }

    glGenTextures(1, &self->texture);
    glBindTexture(GL_TEXTURE_2D, self->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    self->glReady = TRUE;
    OHOS_LOG("WPEPlatformOHOS: рисуем через %s\n", (const char*)glGetString(GL_RENDERER));
    return TRUE;
}

static gboolean wpeViewOHOSRenderBuffer(WPEView* view, WPEBuffer* buffer,
    const WPERectangle*, guint, GError** error)
{
    WPEViewOHOS* self = WPE_VIEW_OHOS(view);
    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_view_get_display(view));

    if (!viewEnsureGL(self, error))
        return FALSE;

    // WPE сам разберётся, лежит ли кадр в dma-buf или в общей памяти.
    EGLImage image = (EGLImage)wpe_buffer_import_to_egl_image(buffer, error);
    if (!image)
        return FALSE;

    eglMakeCurrent(display->eglDisplay, self->eglSurface, self->eglSurface, self->eglContext);

    glViewport(0, 0, wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer));

    glBindTexture(GL_TEXTURE_2D, self->texture);
    imageTargetTexture2D(GL_TEXTURE_2D, (GLeglImageOES)image);

    glUseProgram(self->program);

    // Прямоугольник во всё окно. Второй набор — положение точки в кадре;
    // по высоте перевёрнут, потому что кадр считается сверху вниз,
    // а поверхность рисования — снизу вверх.
    static const GLfloat vertices[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    static const GLfloat texCoords[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (!eglSwapBuffers(display->eglDisplay, self->eglSurface)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "кадр не отдался: 0x%04x", eglGetError());
        return FALSE;
    }

    if (self->committedBuffer)
        wpe_view_buffer_released(view, self->committedBuffer);
    self->committedBuffer = buffer;
    wpe_view_buffer_rendered(view, buffer);
    return TRUE;
}

// Когда окну назначают верхний уровень, надо узнать размер и показаться.
static void viewToplevelChanged(WPEView* view, GParamSpec*, gpointer)
{
    WPEToplevel* toplevel = wpe_view_get_toplevel(view);
    if (!toplevel) {
        wpe_view_unmap(view);
        return;
    }

    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_view_get_display(view));
    int width = 0;
    int height = 0;
    wpe_toplevel_get_size(toplevel, &width, &height);
    if (!width || !height) {
        width = display->width;
        height = display->height;
        wpe_toplevel_resized(toplevel, width, height);
    }
    wpe_view_resized(view, width, height);
    wpe_view_map(view);
    OHOS_LOG("WPEPlatformOHOS: окно показано, %dx%d\n", width, height);
}

static void wpeViewOHOSConstructed(GObject* object)
{
    G_OBJECT_CLASS(wpe_view_ohos_parent_class)->constructed(object);
    g_signal_connect(object, "notify::toplevel", G_CALLBACK(viewToplevelChanged), nullptr);
}

static void wpeViewOHOSDispose(GObject* object)
{
    WPEViewOHOS* self = WPE_VIEW_OHOS(object);
    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_view_get_display(WPE_VIEW(object)));
    if (display && display->eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(display->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (self->eglSurface != EGL_NO_SURFACE)
            eglDestroySurface(display->eglDisplay, self->eglSurface);
        if (self->eglContext != EGL_NO_CONTEXT)
            eglDestroyContext(display->eglDisplay, self->eglContext);
    }
    self->eglSurface = EGL_NO_SURFACE;
    self->eglContext = EGL_NO_CONTEXT;
    G_OBJECT_CLASS(wpe_view_ohos_parent_class)->dispose(object);
}

static void wpe_view_ohos_init(WPEViewOHOS* self)
{
    self->eglSurface = EGL_NO_SURFACE;
    self->eglContext = EGL_NO_CONTEXT;
}

static void wpe_view_ohos_class_init(WPEViewOHOSClass* klass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(klass);
    objectClass->constructed = wpeViewOHOSConstructed;
    objectClass->dispose = wpeViewOHOSDispose;

    WPE_VIEW_CLASS(klass)->render_buffer = wpeViewOHOSRenderBuffer;
}

// ============================================================================
//  Объявление себя
// ============================================================================

extern "C" {

G_MODULE_EXPORT void g_io_module_load(GIOModule*)
{
    g_type_ensure(wpe_display_ohos_get_type());
    g_io_extension_point_implement(WPE_DISPLAY_EXTENSION_POINT_NAME,
        wpe_display_ohos_get_type(), "ohos", 0);
    OHOS_LOG("WPEPlatformOHOS: площадка объявлена\n");
}

G_MODULE_EXPORT void g_io_module_unload(GIOModule*)
{
}

G_MODULE_EXPORT char** g_io_module_query(void)
{
    const char* points[] = { WPE_DISPLAY_EXTENSION_POINT_NAME, nullptr };
    return g_strdupv((char**)points);
}

} // extern "C"
