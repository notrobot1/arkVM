/*
 * Площадка вывода WPE для OpenHarmony.
 *
 * Подключаемая часть: кладётся в /system/lib64/wpe-platform-2.0/modules/,
 * выбирается переменной окружения WPE_PLATFORM=ohos. Исходники WebKit при
 * этом не меняются вовсе — площадки ищутся по каталогу.
 *
 * Окно берётся одним из двух способов. Если дан номер поверхности в
 * WPE_OHOS_SURFACE_ID — берём его (так будет в приложении на ArkTS, где
 * поверхность приходит от XComponent). Если не дан — заводим окно сами,
 * подгрузив libarkvm_ohos_window.z.so.
 *
 * Номер поверхности в OpenHarmony внутрипроцессный: SurfaceUtils хранит их
 * в обычном списке в памяти процесса. Поэтому принять номер от другого
 * процесса нельзя — тот, кто заводит окно, и тот, кто рисует, должны быть
 * одним процессом.
 *
 * Отрисовка устроена просто: WPE отдаёт готовый кадр, мы просим у него
 * картинку для EGL (wpe_buffer_import_to_egl_image — он сам разберётся,
 * лежит ли кадр в dma-buf или в общей памяти), натягиваем её на
 * прямоугольник во всё окно и отдаём кадр системе.
 *
 * Виды объявлены отчуждаемыми (G_DEFINE_DYNAMIC_TYPE) и привязаны к самой
 * части. Иначе GIO выгрузит нас сразу после просмотра каталога, а
 * зарегистрированные виды будут указывать в освобождённую память.
 *
 * Кадры движок может отдавать двумя способами: разделяемым буфером
 * (dma-buf) или через общую память. Первый быстрее: картинка не
 * переписывается вовсе, она просто становится полотном. Но движок выберет
 * его только если площадка объявит, какие виды кадров принимает и через
 * какое устройство отрисовки они разделяются. Этим заняты обязанности
 * get_drm_device и get_preferred_buffer_formats ниже.
 *
 * Ничего не зашито: узел отрисовки спрашивается у самого EGL, перечень
 * видов — тоже у него. Если нужных расширений нет, площадка молчит, и
 * движок сам отступает к общей памяти — тот путь никуда не делся.
 *
 * Переменные окружения:
 *   WPE_OHOS_SURFACE_ID  номер поверхности; если не задан, окно заводится само
 *   WPE_OHOS_WIDTH       ширина окна, по умолчанию 1920
 *   WPE_OHOS_HEIGHT      высота окна, по умолчанию 1080
 *   WPE_OHOS_DRM_DEVICE  узел отрисовки вручную, если угадать не вышло
 *   WPE_OHOS_NO_DMABUF   если задана — не объявлять разделяемые буферы
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
#include <dirent.h>
#include <dlfcn.h>

#define OHOS_LOG(...) do { if (g_getenv("WPE_OHOS_DEBUG")) g_printerr(__VA_ARGS__); } while (0)

// Этого вызова нет в обычных объявлениях GLES2 — берём у EGL по имени.
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture2D;

// Значения из расширений EGL. Объявляем сами: в заголовках, собранных без
// этих расширений, их может не оказаться, а числа закреплены навсегда.
#ifndef EGL_DEVICE_EXT
#define EGL_DEVICE_EXT 0x322C
typedef void* EGLDeviceEXT;
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

// Обозначения видов кадров. Четырёхбуквенные коды из drm_fourcc.h; заголовка
// libdrm у нас нет, а коды эти неизменны.
#define OHOS_FOURCC(a, b, c, d) \
    ((guint32)(a) | ((guint32)(b) << 8) | ((guint32)(c) << 16) | ((guint32)(d) << 24))
#define OHOS_FORMAT_ARGB8888 OHOS_FOURCC('A', 'R', '2', '4')
#define OHOS_FORMAT_XRGB8888 OHOS_FOURCC('X', 'R', '2', '4')
#define OHOS_FORMAT_ABGR8888 OHOS_FOURCC('A', 'B', '2', '4')
#define OHOS_FORMAT_XBGR8888 OHOS_FOURCC('X', 'B', '2', '4')
#define OHOS_MODIFIER_INVALID ((guint64)0x00ffffffffffffffULL)

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
    // Растёт при каждом изменении размера. Отрисовка сверяется с ним и
    // пересоздаёт поверхность EGL: сама она за размером буфера не следует.
    guint sizeSerial;
    WPEKeymap* keymap;

    // Про разделяемые буферы. Определяется один раз, при первом вопросе.
    gboolean dmaBufChecked;
    WPEDRMDevice* drmDevice;
    WPEBufferFormats* bufferFormats;
};
G_DEFINE_DYNAMIC_TYPE(WPEDisplayOHOS, wpe_display_ohos, WPE_TYPE_DISPLAY)

struct _WPEToplevelOHOS {
    WPEToplevel parent;
};
G_DEFINE_DYNAMIC_TYPE(WPEToplevelOHOS, wpe_toplevel_ohos, WPE_TYPE_TOPLEVEL)

struct _WPEViewOHOS {
    WPEView parent;

    EGLSurface eglSurface;
    EGLContext eglContext;
    EGLConfig eglConfig;      // нужен, чтобы пересоздать поверхность
    guint surfaceSerial;      // с каким размером она создана
    GLuint program;
    GLuint texture;
    GLint swapLocation;
    gboolean glReady;
    WPEBuffer* committedBuffer;
};
G_DEFINE_DYNAMIC_TYPE(WPEViewOHOS, wpe_view_ohos, WPE_TYPE_VIEW)

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

// swapRB: кадры из общей памяти приходят в порядке ARGB8888, что на
// машине с обратным порядком байтов означает B, G, R, A. Полотно же
// читается как R, G, B, A. Поэтому при заливке точек меняем местами
// первую и третью составляющие; для картинки из dma-buf это не нужно.
static const char* kFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D tex;\n"
    "uniform float swapRB;\n"
    "void main() {\n"
    "    vec4 c = texture2D(tex, v_texCoord);\n"
    "    gl_FragColor = mix(c, c.bgra, swapRB);\n"
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

// ============================================================================
//  Разделяемые буферы: что мы умеем принимать
// ============================================================================

// Узел отрисовки спрашиваем у EGL. Так выходит правильно на любой машине:
// то же устройство, на котором EGL и работает, а не первое попавшееся.
static char* queryRenderNodeFromEGL(EGLDisplay eglDisplay)
{
    typedef EGLBoolean (*QueryDisplayAttribFn)(EGLDisplay, EGLint, EGLAttrib*);
    typedef const char* (*QueryDeviceStringFn)(EGLDeviceEXT, EGLint);

    QueryDisplayAttribFn queryDisplayAttrib =
        (QueryDisplayAttribFn)eglGetProcAddress("eglQueryDisplayAttribEXT");
    QueryDeviceStringFn queryDeviceString =
        (QueryDeviceStringFn)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!queryDisplayAttrib || !queryDeviceString)
        return nullptr;

    EGLAttrib attribute = 0;
    if (!queryDisplayAttrib(eglDisplay, EGL_DEVICE_EXT, &attribute))
        return nullptr;

    EGLDeviceEXT device = (EGLDeviceEXT)attribute;
    const char* node = queryDeviceString(device, EGL_DRM_RENDER_NODE_FILE_EXT);
    if (!node || !*node)
        node = queryDeviceString(device, EGL_DRM_DEVICE_FILE_EXT);
    return (node && *node) ? g_strdup(node) : nullptr;
}

// Запасной путь, если EGL о себе не рассказывает: берём первый узел
// отрисовки из каталога устройств. Перебор по именам, без libdrm.
static char* findAnyRenderNode(void)
{
    DIR* dir = opendir("/dev/dri");
    if (!dir)
        return nullptr;

    char* found = nullptr;
    while (struct dirent* entry = readdir(dir)) {
        if (!g_str_has_prefix(entry->d_name, "renderD"))
            continue;
        found = g_build_filename("/dev/dri", entry->d_name, nullptr);
        break;
    }
    closedir(dir);
    return found;
}

// Виды кадров, которые мы в самом деле умеем натянуть на полотно.
// Остальное объявлять нечестно: движок отдаст, а показать не выйдет.
static gboolean isFormatWeCanDraw(guint32 fourcc)
{
    return fourcc == OHOS_FORMAT_ARGB8888 || fourcc == OHOS_FORMAT_XRGB8888
        || fourcc == OHOS_FORMAT_ABGR8888 || fourcc == OHOS_FORMAT_XBGR8888;
}

// Перечень собираем из ответов самого EGL. Если расширения с признаками
// размещения нет — объявляем те же виды с признаком «как получится»:
// это и есть обычное последовательное размещение.
static WPEBufferFormats* buildBufferFormats(EGLDisplay eglDisplay, WPEDRMDevice* device)
{
    typedef EGLBoolean (*QueryFormatsFn)(EGLDisplay, EGLint, EGLint*, EGLint*);
    typedef EGLBoolean (*QueryModifiersFn)(EGLDisplay, EGLint, EGLint, EGLuint64KHR*,
                                           EGLBoolean*, EGLint*);

    QueryFormatsFn queryFormats = (QueryFormatsFn)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    QueryModifiersFn queryModifiers =
        (QueryModifiersFn)eglGetProcAddress("eglQueryDmaBufModifiersEXT");

    WPEBufferFormatsBuilder* builder = wpe_buffer_formats_builder_new(device);
    wpe_buffer_formats_builder_append_group(builder, device, WPE_BUFFER_FORMAT_USAGE_RENDERING);

    guint declared = 0;

    if (queryFormats) {
        EGLint count = 0;
        if (queryFormats(eglDisplay, 0, nullptr, &count) && count > 0) {
            EGLint* formats = g_new0(EGLint, count);
            if (queryFormats(eglDisplay, count, formats, &count)) {
                for (EGLint i = 0; i < count; i++) {
                    guint32 fourcc = (guint32)formats[i];
                    if (!isFormatWeCanDraw(fourcc))
                        continue;

                    gboolean addedAny = FALSE;
                    if (queryModifiers) {
                        EGLint modifierCount = 0;
                        if (queryModifiers(eglDisplay, formats[i], 0, nullptr, nullptr,
                                           &modifierCount) && modifierCount > 0) {
                            EGLuint64KHR* modifiers = g_new0(EGLuint64KHR, modifierCount);
                            EGLBoolean* external = g_new0(EGLBoolean, modifierCount);
                            if (queryModifiers(eglDisplay, formats[i], modifierCount, modifiers,
                                               external, &modifierCount)) {
                                for (EGLint m = 0; m < modifierCount; m++) {
                                    // Размещения, требующие особого рода полотна,
                                    // пропускаем: рисуем обычным.
                                    if (external[m])
                                        continue;
                                    wpe_buffer_formats_builder_append_format(builder, fourcc,
                                        (guint64)modifiers[m]);
                                    addedAny = TRUE;
                                    declared++;
                                }
                            }
                            g_free(modifiers);
                            g_free(external);
                        }
                    }

                    // Без перечня размещений — хотя бы «как получится».
                    if (!addedAny) {
                        wpe_buffer_formats_builder_append_format(builder, fourcc,
                            OHOS_MODIFIER_INVALID);
                        declared++;
                    }
                }
            }
            g_free(formats);
        }
    }

    // EGL промолчал — объявим пару самых обиходных видов. Если он их не
    // примет, движок получит отказ при разборе кадра и вернётся к общей
    // памяти; ничего не сломается.
    if (!declared) {
        wpe_buffer_formats_builder_append_format(builder, OHOS_FORMAT_ARGB8888,
            OHOS_MODIFIER_INVALID);
        wpe_buffer_formats_builder_append_format(builder, OHOS_FORMAT_XRGB8888,
            OHOS_MODIFIER_INVALID);
        declared = 2;
    }

    OHOS_LOG("WPEPlatformOHOS: объявлено видов кадров: %u\n", declared);
    return wpe_buffer_formats_builder_end(builder);
}

// Общая подготовка: оба вопроса движка (устройство и виды кадров) отвечают
// из одного и того же разбора, и делается он однажды.
static void ensureDMABufSupport(WPEDisplayOHOS* self)
{
    if (self->dmaBufChecked)
        return;
    self->dmaBufChecked = TRUE;

    if (g_getenv("WPE_OHOS_NO_DMABUF")) {
        OHOS_LOG("WPEPlatformOHOS: разделяемые буферы отключены вручную\n");
        return;
    }

    GError* error = nullptr;
    EGLDisplay eglDisplay = (EGLDisplay)wpeDisplayOHOSGetEGLDisplay(WPE_DISPLAY(self), &error);
    if (!eglDisplay) {
        OHOS_LOG("WPEPlatformOHOS: EGL недоступен (%s), остаёмся на общей памяти\n",
            error ? error->message : "без пояснения");
        g_clear_error(&error);
        return;
    }

    // Без этого расширения кадр из разделяемого буфера не превратить
    // в картинку — объявлять их нечего.
    const char* extensions = eglQueryString(eglDisplay, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_EXT_image_dma_buf_import")) {
        OHOS_LOG("WPEPlatformOHOS: EGL не умеет принимать разделяемые буферы\n");
        return;
    }

    char* node = nullptr;
    const char* forced = g_getenv("WPE_OHOS_DRM_DEVICE");
    if (forced && *forced)
        node = g_strdup(forced);
    if (!node)
        node = queryRenderNodeFromEGL(eglDisplay);
    if (!node)
        node = findAnyRenderNode();
    if (!node) {
        OHOS_LOG("WPEPlatformOHOS: узел отрисовки не найден, остаёмся на общей памяти\n");
        return;
    }

    // Первый довод — основной узел, второй — узел отрисовки. Основной нам
    // не нужен и не всегда доступен приложению; движку достаточно второго.
    self->drmDevice = wpe_drm_device_new(nullptr, node);
    if (!self->drmDevice) {
        OHOS_LOG("WPEPlatformOHOS: устройство отрисовки %s не принято\n", node);
        g_free(node);
        return;
    }

    self->bufferFormats = buildBufferFormats(eglDisplay, self->drmDevice);
    OHOS_LOG("WPEPlatformOHOS: разделяемые буферы через %s\n", node);
    g_free(node);
}

static WPEDRMDevice* wpeDisplayOHOSGetDRMDevice(WPEDisplay* display)
{
    WPEDisplayOHOS* self = WPE_DISPLAY_OHOS(display);
    ensureDMABufSupport(self);
    return self->drmDevice;
}

static WPEBufferFormats* wpeDisplayOHOSGetPreferredBufferFormats(WPEDisplay* display)
{
    WPEDisplayOHOS* self = WPE_DISPLAY_OHOS(display);
    ensureDMABufSupport(self);
    return self->bufferFormats;
}

// Заводим окно сами, через вспомогательную библиотеку из дерева OpenHarmony.
// Прямо отсюда к службе отрисовки не дотянуться: площадка собирается лишь
// по пакету для разработчиков. Подгружаем по имени.
static gboolean createOwnWindow(WPEDisplayOHOS* self, GError** error)
{
    typedef guint64 (*CreateFn)(int*, int*);

    OHOS_LOG("WPEPlatformOHOS: подгружаю libarkvm_ohos_window.z.so\n");
    // Именно без RTLD_GLOBAL. Помощник тянет за собой части ArkUI, собранные
    // со своей однодельной библиотекой (libc++_shared), а у нас уже открыта
    // своя (libc++.so.1). При общей видимости их имена сталкиваются, и
    // предпусковая часть skia падает ещё до первого нашего вызова.
    void* helper = dlopen("libarkvm_ohos_window.z.so", RTLD_NOW);
    if (!helper) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "не удалось подгрузить libarkvm_ohos_window.z.so: %s", dlerror());
        return FALSE;
    }

    OHOS_LOG("WPEPlatformOHOS: подгрузилась, ищу arkvm_window_create\n");
    CreateFn create = (CreateFn)dlsym(helper, "arkvm_window_create");
    if (!create) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "в libarkvm_ohos_window.z.so нет arkvm_window_create");
        return FALSE;
    }

    OHOS_LOG("WPEPlatformOHOS: завожу окно\n");
    int width = 0;
    int height = 0;
    self->surfaceId = create(&width, &height);
    if (!self->surfaceId) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "окно не завелось: служба отрисовки не отвечает?");
        return FALSE;
    }
    self->width = width;
    self->height = height;
    OHOS_LOG("WPEPlatformOHOS: завёл своё окно, поверхность %" G_GUINT64_FORMAT ", %dx%d\n",
        self->surfaceId, width, height);
    return TRUE;
}

static gboolean wpeDisplayOHOSConnect(WPEDisplay* display, GError** error)
{
    WPEDisplayOHOS* self = WPE_DISPLAY_OHOS(display);
    OHOS_LOG("WPEPlatformOHOS: подключаюсь\n");

    const char* widthText = g_getenv("WPE_OHOS_WIDTH");
    const char* heightText = g_getenv("WPE_OHOS_HEIGHT");
    self->width = widthText ? atoi(widthText) : 1920;
    self->height = heightText ? atoi(heightText) : 1080;

    // Если номер поверхности дали снаружи — берём его. Так будет в приложении
    // на ArkTS: там поверхность приходит от XComponent, и она в том же
    // процессе. Если не дали — заводим окно сами.
    const char* idText = g_getenv("WPE_OHOS_SURFACE_ID");
    if (idText && *idText) {
        self->surfaceId = g_ascii_strtoull(idText, nullptr, 10);
        OHOS_LOG("WPEPlatformOHOS: мне дали поверхность %" G_GUINT64_FORMAT "\n", self->surfaceId);
    } else if (!createOwnWindow(self, error))
        return FALSE;

    int ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId(self->surfaceId, &self->window);
    if (ret != 0 || !self->window) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "окно из поверхности %" G_GUINT64_FORMAT " не получилось: %d", self->surfaceId, ret);
        return FALSE;
    }

    // Размер буфера надо задать до того, как у окна попросят поверхность EGL,
    // иначе слой связи с Vulkan не заведёт ни одного образа для обмена.
    OH_NativeWindow_NativeWindowHandleOpt(self->window, SET_BUFFER_GEOMETRY, self->width, self->height);

    OHOS_LOG("WPEPlatformOHOS: окно %p готово, %dx%d\n",
        (void*)self->window, self->width, self->height);
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
    g_clear_object(&self->bufferFormats);
    if (self->drmDevice) {
        wpe_drm_device_unref(self->drmDevice);
        self->drmDevice = nullptr;
    }
    if (self->window) {
        OH_NativeWindow_DestroyNativeWindow(self->window);
        self->window = nullptr;
    }
    G_OBJECT_CLASS(wpe_display_ohos_parent_class)->dispose(object);
}

static void wpe_display_ohos_init(WPEDisplayOHOS* self)
{
    OHOS_LOG("WPEPlatformOHOS: объект площадки создан\n");
    self->eglDisplay = EGL_NO_DISPLAY;
}

static void wpe_display_ohos_class_init(WPEDisplayOHOSClass* klass)
{
    OHOS_LOG("WPEPlatformOHOS: вид площадки описан\n");
    G_OBJECT_CLASS(klass)->dispose = wpeDisplayOHOSDispose;

    WPEDisplayClass* displayClass = WPE_DISPLAY_CLASS(klass);
    displayClass->connect = wpeDisplayOHOSConnect;
    displayClass->create_view = wpeDisplayOHOSCreateView;
    displayClass->create_toplevel = wpeDisplayOHOSCreateToplevel;
    displayClass->get_egl_display = wpeDisplayOHOSGetEGLDisplay;
    displayClass->get_keymap = wpeDisplayOHOSGetKeymap;
    // Без этих двух движок считает, что разделяемые буферы нам не нужны,
    // и отдаёт кадры через общую память.
    displayClass->get_drm_device = wpeDisplayOHOSGetDRMDevice;
    displayClass->get_preferred_buffer_formats = wpeDisplayOHOSGetPreferredBufferFormats;
}

// Отчуждаемый вид обязан уметь прибираться, даже если прибирать нечего.
static void wpe_display_ohos_class_finalize(WPEDisplayOHOSClass*)
{
}

// ============================================================================
//  Верхний уровень: обязанности
// ============================================================================

static gboolean viewResizeHelper(WPEToplevel*, WPEView* view, gpointer data)
{
    const int* size = (const int*)data;
    wpe_view_resized(view, size[0], size[1]);
    return FALSE; // продолжать обход
}

static gboolean wpeToplevelOHOSResize(WPEToplevel* toplevel, int width, int height)
{
    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_toplevel_get_display(toplevel));
    if (width <= 0 || height <= 0)
        return FALSE;
    if (width == display->width && height == display->height)
        return TRUE;

    // Новый размер буфера окна.
    if (display->window)
        OH_NativeWindow_NativeWindowHandleOpt(display->window, SET_BUFFER_GEOMETRY, width, height);

    // Это читает отрисовка при задании области рисования.
    display->width = width;
    display->height = height;

    // Поверхность EGL за размером буфера сама не следует: её надо
    // пересоздать. Отрисовка заметит расхождение по этому счётчику.
    display->sizeSerial++;

    wpe_toplevel_resized(toplevel, width, height);

    // Движку надо сказать отдельно: сам он размер окон не пересчитывает.
    int size[2] = { width, height };
    wpe_toplevel_foreach_view(toplevel, viewResizeHelper, size);

    OHOS_LOG("WPEPlatformOHOS: размер стал %dx%d\n", width, height);
    return TRUE;
}

static void wpe_toplevel_ohos_init(WPEToplevelOHOS*)
{
}

static void wpe_toplevel_ohos_class_init(WPEToplevelOHOSClass* klass)
{
    WPE_TOPLEVEL_CLASS(klass)->resize = wpeToplevelOHOSResize;
}

static void wpe_toplevel_ohos_class_finalize(WPEToplevelOHOSClass*)
{
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

    self->eglConfig = config;
    self->surfaceSerial = display->sizeSerial;
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

    self->swapLocation = glGetUniformLocation(self->program, "swapRB");

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

// Размер окна изменился — поверхность EGL создана под прежний буфер и
// рисовать в неё больше нельзя: по краям останется пустота, а при сжатии
// картинку сплющит. Пересоздаём.
static gboolean viewSyncSurfaceSize(WPEViewOHOS* self, GError** error)
{
    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_view_get_display(WPE_VIEW(self)));
    if (self->surfaceSerial == display->sizeSerial)
        return TRUE;

    EGLDisplay eglDisplay = display->eglDisplay;
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (self->eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(eglDisplay, self->eglSurface);
        self->eglSurface = EGL_NO_SURFACE;
    }

    self->eglSurface = eglCreateWindowSurface(eglDisplay, self->eglConfig,
        (EGLNativeWindowType)display->window, nullptr);
    if (self->eglSurface == EGL_NO_SURFACE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "оконная поверхность EGL не пересоздалась: 0x%04x", eglGetError());
        return FALSE;
    }

    if (!eglMakeCurrent(eglDisplay, self->eglSurface, self->eglSurface, self->eglContext)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "окружение отрисовки не выбралось после изменения размера: 0x%04x", eglGetError());
        return FALSE;
    }

    self->surfaceSerial = display->sizeSerial;
    OHOS_LOG("WPEPlatformOHOS: поверхность пересоздана под %dx%d\n",
        display->width, display->height);
    return TRUE;
}

static gboolean wpeViewOHOSRenderBuffer(WPEView* view, WPEBuffer* buffer,
    const WPERectangle*, guint, GError** error)
{
    WPEViewOHOS* self = WPE_VIEW_OHOS(view);
    WPEDisplayOHOS* display = WPE_DISPLAY_OHOS(wpe_view_get_display(view));

    if (!viewEnsureGL(self, error))
        return FALSE;
    if (!viewSyncSurfaceSize(self, error))
        return FALSE;

    const int width = wpe_buffer_get_width(buffer);
    const int height = wpe_buffer_get_height(buffer);

    eglMakeCurrent(display->eglDisplay, self->eglSurface, self->eglSurface, self->eglContext);
    // Рисуем во всё окно: кадр может прийти другого размера, тогда он просто
    // растянется, а не прижмётся в угол. Начало отсчёта в OpenGL — левый
    // нижний угол, поэтому меньшая область рисования уводит картинку вниз.
    glViewport(0, 0, display->width, display->height);
    glBindTexture(GL_TEXTURE_2D, self->texture);

    // Два пути. Кадр в dma-buf превращается в картинку для EGL без
    // переписывания — это даром. Кадр в общей памяти приходится заливать
    // в полотно; так выходит медленнее, зато работает всегда.
    float swapRB = 0.0f;
    GError* imageError = nullptr;
    EGLImage image = (EGLImage)wpe_buffer_import_to_egl_image(buffer, &imageError);

    // Один раз скажем, каким путём пошло: это первое, что хочется знать,
    // когда отрисовка кажется медленной.
    static gboolean pathReported = FALSE;
    if (!pathReported) {
        pathReported = TRUE;
        OHOS_LOG("WPEPlatformOHOS: кадры идут %s\n",
            image ? "разделяемым буфером" : "через общую память");
    }

    if (image) {
        imageTargetTexture2D(GL_TEXTURE_2D, (GLeglImageOES)image);
    } else {
        g_clear_error(&imageError);

        GBytes* pixels = wpe_buffer_import_to_pixels(buffer, error);
        if (!pixels)
            return FALSE;

        gsize size = 0;
        const guint8* data = (const guint8*)g_bytes_get_data(pixels, &size);
        guint stride = (guint)width * 4;
        if (WPE_IS_BUFFER_SHM(buffer))
            stride = wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer));

        if (stride == (guint)width * 4) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, data);
        } else {
            // Строки лежат с запасом: GLES2 не умеет про это слышать,
            // поэтому заливаем по одной.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            for (int y = 0; y < height; ++y) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, data + (gsize)y * stride);
            }
        }
        swapRB = 1.0f;
    }

    glUseProgram(self->program);
    if (self->swapLocation >= 0)
        glUniform1f(self->swapLocation, swapRB);

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
    // Размер задаём мы, а не движок: у него своё умолчание 1024x768,
    // и кадр приходил бы меньше окна.
    const int width = display->width;
    const int height = display->height;
    wpe_toplevel_resize(toplevel, width, height);
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

static void wpe_view_ohos_class_finalize(WPEViewOHOSClass*)
{
}

// ============================================================================
//  Объявление себя
// ============================================================================

extern "C" {

G_MODULE_EXPORT void g_io_module_load(GIOModule* module)
{
    // Привязка видов к самой части обязательна: иначе GIO выгрузит нас сразу
    // после просмотра каталога, а зарегистрированные виды будут указывать
    // в освобождённую память — и первое же создание объекта площадки
    // кончится падением.
    wpe_display_ohos_register_type(G_TYPE_MODULE(module));
    wpe_toplevel_ohos_register_type(G_TYPE_MODULE(module));
    wpe_view_ohos_register_type(G_TYPE_MODULE(module));

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
