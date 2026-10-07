/*
 * Родная часть обозревателя: связь между XComponent и WPE WebKit.
 *
 * Что здесь происходит.
 *
 * ArkUI создаёт поверхность для XComponent и зовёт OnSurfaceCreated, передавая
 * готовое окно. Мы спрашиваем у окна номер поверхности, кладём его в
 * переменную окружения и запускаем движок — площадка WPEPlatformOHOS подхватит
 * этот номер при подключении и будет рисовать прямо в окно приложения.
 *
 * Движок живёт в отдельном потоке. Это не прихоть: у ArkUI своя череда
 * событий, у GLib своя, и смешивать их нельзя.
 *
 * ВАЖНО: сам движок не связан с этой библиотекой наглухо, а подтягивается
 * вызовом dlopen уже из потока движка. Причина простая: libWPEWebKit весит
 * полторы сотни мегабайт, и её загрузка в потоке разметки занимает больше
 * времени, чем система отводит приложению на запуск, — оно объявляется
 * зависшим и его снимают. Поэтому наружу библиотека лёгкая, а тяжёлое
 * грузится в стороне. Нужных вызовов всего восемь, их берём по именам.
 *
 * glib и gobject остаются связанными обычным образом: они небольшие,
 * а пользуемся мы ими много.
 */

#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>
#include <hilog/log.h>

#include <glib.h>
#include <glib-object.h>
#include <wpe/webkit.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

#define LOG_TAG "WPEBrowser"
#define LOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO,  0x3200, LOG_TAG, __VA_ARGS__)
#define LOGE(...) OH_LOG_Print(LOG_APP, LOG_ERROR, 0x3200, LOG_TAG, __VA_ARGS__)

namespace {

// ---------------------------------------------------------------------------
//  Вызовы движка, которые берём по именам
// ---------------------------------------------------------------------------

struct Engine {
    void* handle = nullptr;

    GType       (*webkit_web_view_get_type)(void) = nullptr;
    void        (*webkit_web_view_load_uri)(WebKitWebView*, const char*) = nullptr;
    const char* (*webkit_web_view_get_title)(WebKitWebView*) = nullptr;
    WPEView*    (*webkit_web_view_get_wpe_view)(WebKitWebView*) = nullptr;

    WPEDisplay*  (*wpe_display_get_default)(void) = nullptr;
    WPEToplevel* (*wpe_view_get_toplevel)(WPEView*) = nullptr;
    void         (*wpe_view_set_visible)(WPEView*, gboolean) = nullptr;
    gboolean     (*wpe_toplevel_resize)(WPEToplevel*, int, int) = nullptr;
};

Engine g_engine;

bool loadEngine()
{
    if (g_engine.handle)
        return true;

    g_engine.handle = dlopen("libWPEWebKit-2.0.so.1", RTLD_NOW);
    if (!g_engine.handle) {
        LOGE("движок не подтянулся: %{public}s", dlerror());
        return false;
    }

#define TAKE(name) \
    *(void**)(&g_engine.name) = dlsym(g_engine.handle, #name); \
    if (!g_engine.name) { LOGE("в движке нет %{public}s", #name); return false; }

    TAKE(webkit_web_view_get_type)
    TAKE(webkit_web_view_load_uri)
    TAKE(webkit_web_view_get_title)
    TAKE(webkit_web_view_get_wpe_view)
    TAKE(wpe_display_get_default)
    TAKE(wpe_view_get_toplevel)
    TAKE(wpe_view_set_visible)
    TAKE(wpe_toplevel_resize)
#undef TAKE

    LOGI("движок подтянут");
    return true;
}

// ---------------------------------------------------------------------------
//  Поток движка
// ---------------------------------------------------------------------------

struct Browser {
    GMainContext* context = nullptr;
    GMainLoop* loop = nullptr;
    WebKitWebView* view = nullptr;
    char* pendingUrl = nullptr;
    pthread_t thread = 0;
    bool started = false;
};

Browser g_browser;

const char* kDefaultUrl = "https://wpewebkit.org";

void onLoadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer)
{
    if (event != WEBKIT_LOAD_FINISHED)
        return;
    const char* title = g_engine.webkit_web_view_get_title(view);
    LOGI("загружено: %{public}s", title ? title : "(без заголовка)");
}

void onLoadFailed(WebKitWebView*, WebKitLoadEvent, const char* uri, GError* error, gpointer)
{
    LOGE("не удалось загрузить %{public}s: %{public}s", uri, error->message);
}

void* engineThread(void*)
{
    LOGI("поток движка пошёл");

    if (!loadEngine())
        return nullptr;

    // Своя череда событий для этого потока.
    g_browser.context = g_main_context_new();
    g_main_context_push_thread_default(g_browser.context);
    g_browser.loop = g_main_loop_new(g_browser.context, FALSE);

    WPEDisplay* display = g_engine.wpe_display_get_default();
    if (!display) {
        LOGE("площадка не нашлась: WPE_PLATFORM=%{public}s", g_getenv("WPE_PLATFORM"));
        return nullptr;
    }
    LOGI("площадка: %{public}s", G_OBJECT_TYPE_NAME(display));

    g_browser.view = (WebKitWebView*)g_object_new(g_engine.webkit_web_view_get_type(),
        "display", display, nullptr);
    if (!g_browser.view) {
        LOGE("обозревательное окно не создалось");
        return nullptr;
    }

    // Движок не рисует, пока окно не объявлено видимым, и берёт размер
    // у верхнего уровня, а не у площадки.
    WPEView* wpeView = g_engine.webkit_web_view_get_wpe_view(g_browser.view);
    if (wpeView) {
        WPEToplevel* toplevel = g_engine.wpe_view_get_toplevel(wpeView);
        if (toplevel) {
            const char* w = g_getenv("WPE_OHOS_WIDTH");
            const char* h = g_getenv("WPE_OHOS_HEIGHT");
            g_engine.wpe_toplevel_resize(toplevel, w ? atoi(w) : 1920, h ? atoi(h) : 1080);
        }
        g_engine.wpe_view_set_visible(wpeView, TRUE);
    }

    g_signal_connect(g_browser.view, "load-changed", G_CALLBACK(onLoadChanged), nullptr);
    g_signal_connect(g_browser.view, "load-failed", G_CALLBACK(onLoadFailed), nullptr);

    const char* url = g_browser.pendingUrl ? g_browser.pendingUrl : kDefaultUrl;
    LOGI("открываю %{public}s", url);
    g_engine.webkit_web_view_load_uri(g_browser.view, url);

    g_main_loop_run(g_browser.loop);
    LOGI("поток движка закончил");
    return nullptr;
}

// ---------------------------------------------------------------------------
//  Отклики XComponent
// ---------------------------------------------------------------------------

void onSurfaceCreated(OH_NativeXComponent* component, void* window)
{
    LOGI("поверхность создана");
    if (g_browser.started)
        return;

    uint64_t width = 0;
    uint64_t height = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);

    uint64_t surfaceId = 0;
    int ret = OH_NativeWindow_GetSurfaceId((OHNativeWindow*)window, &surfaceId);
    if (ret != 0 || !surfaceId) {
        LOGE("не удалось узнать номер поверхности: %{public}d", ret);
        return;
    }
    LOGI("поверхность %{public}llu, размер %{public}llux%{public}llu",
        (unsigned long long)surfaceId,
        (unsigned long long)width, (unsigned long long)height);

    // Площадка прочтёт это при подключении.
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)surfaceId);
    setenv("WPE_OHOS_SURFACE_ID", buffer, 1);
    snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)width);
    setenv("WPE_OHOS_WIDTH", buffer, 1);
    snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)height);
    setenv("WPE_OHOS_HEIGHT", buffer, 1);
    setenv("WPE_PLATFORM", "ohos", 1);

    g_browser.started = true;
    if (pthread_create(&g_browser.thread, nullptr, engineThread, nullptr) != 0) {
        LOGE("поток движка не запустился");
        g_browser.started = false;
    }
}

void onSurfaceChanged(OH_NativeXComponent* component, void* window)
{
    uint64_t width = 0;
    uint64_t height = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);
    LOGI("размер поверхности стал %{public}llux%{public}llu",
        (unsigned long long)width, (unsigned long long)height);
}

void onSurfaceDestroyed(OH_NativeXComponent*, void*)
{
    LOGI("поверхность уходит");
    if (g_browser.loop)
        g_main_loop_quit(g_browser.loop);
}

void onDispatchTouchEvent(OH_NativeXComponent*, void*)
{
    // Передача прикосновений в движок — следующий шаг.
}

OH_NativeXComponent_Callback g_callback = {
    .OnSurfaceCreated = onSurfaceCreated,
    .OnSurfaceChanged = onSurfaceChanged,
    .OnSurfaceDestroyed = onSurfaceDestroyed,
    .DispatchTouchEvent = onDispatchTouchEvent,
};

// ---------------------------------------------------------------------------
//  Связь с ArkTS
// ---------------------------------------------------------------------------

struct UrlTask { char* url; };

napi_value LoadUrl(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1)
        return nullptr;

    size_t length = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length);
    char* url = (char*)g_malloc(length + 1);
    napi_get_value_string_utf8(env, argv[0], url, length + 1, &length);

    if (g_browser.view && g_browser.context) {
        // Движок уже работает — перекладываем вызов в его череду.
        UrlTask* task = g_new0(UrlTask, 1);
        task->url = url;
        g_main_context_invoke_full(g_browser.context, G_PRIORITY_DEFAULT,
            [](gpointer data) -> gboolean {
                UrlTask* t = (UrlTask*)data;
                g_engine.webkit_web_view_load_uri(g_browser.view, t->url);
                return G_SOURCE_REMOVE;
            },
            task,
            [](gpointer data) {
                UrlTask* t = (UrlTask*)data;
                g_free(t->url);
                g_free(t);
            });
    } else {
        g_free(g_browser.pendingUrl);
        g_browser.pendingUrl = url;
    }
    return nullptr;
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    LOGI("родная часть загружена");

    napi_property_descriptor desc[] = {
        { "loadUrl", nullptr, LoadUrl, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // ArkUI кладёт сюда наш XComponent, когда в разметке указан libraryname.
    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) != napi_ok) {
        LOGE("XComponent не передан: проверьте libraryname в разметке");
        return exports;
    }
    OH_NativeXComponent* component = nullptr;
    if (napi_unwrap(env, exportInstance, (void**)&component) != napi_ok || !component) {
        LOGE("XComponent не разворачивается");
        return exports;
    }
    OH_NativeXComponent_RegisterCallback(component, &g_callback);
    LOGI("отклики XComponent подключены");
    return exports;
}
EXTERN_C_END

static napi_module wpeModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "wpe",
    .nm_priv = nullptr,
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterWpeModule(void)
{
    napi_module_register(&wpeModule);
}
