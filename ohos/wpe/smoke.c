/*
 * Проба WPE WebKit под OpenHarmony.
 *
 * Создаёт окно, загружает в него страницу и печатает заголовок. Если это
 * работает, значит живы: сам движок, отдельный процесс отрисовки,
 * межпроцессное взаимодействие, разбор разметки, шрифты и — если дать
 * ссылку в сеть — обмен по сети с защищёнными соединениями.
 *
 * Площадку выбирает переменная окружения WPE_PLATFORM:
 *
 *   WPE_PLATFORM=headless   рисовать в память, ничего не показывая;
 *   WPE_PLATFORM=ohos       рисовать в окно OpenHarmony.
 *
 * Во втором случае окно заводит сама проба, подгрузив вспомогательную
 * библиотеку из дерева OpenHarmony, и передаёт площадке номер поверхности.
 * Так и должно быть: номер внутрипроцессный, и заводить окно обязан тот,
 * кто в нём живёт. В приложении на ArkTS это будет делать его родная часть.
 *
 * Сборка и запуск — см. smoke.sh
 */

#include <wpe/webkit.h>

#include <dlfcn.h>
#include <stdlib.h>

static GMainLoop* loop = NULL;
static int exitCode = 1;

/*
 * Завести окно во весь экран и сказать площадке его номер.
 * Площадка прочтёт эти переменные при подключении.
 */
static gboolean prepareWindow(void)
{
    typedef guint64 (*CreateFn)(int*, int*);

    void* helper = dlopen("libarkvm_ohos_window.z.so", RTLD_NOW);
    if (!helper) {
        g_printerr("не удалось подгрузить libarkvm_ohos_window.z.so: %s\n", dlerror());
        return FALSE;
    }
    CreateFn create = (CreateFn)dlsym(helper, "arkvm_window_create");
    if (!create) {
        g_printerr("в libarkvm_ohos_window.z.so нет arkvm_window_create\n");
        return FALSE;
    }

    int width = 0;
    int height = 0;
    guint64 surfaceId = create(&width, &height);
    if (!surfaceId) {
        g_printerr("окно не завелось: служба отрисовки не отвечает?\n");
        return FALSE;
    }

    char buffer[32];
    g_snprintf(buffer, sizeof(buffer), "%" G_GUINT64_FORMAT, surfaceId);
    g_setenv("WPE_OHOS_SURFACE_ID", buffer, TRUE);
    g_snprintf(buffer, sizeof(buffer), "%d", width);
    g_setenv("WPE_OHOS_WIDTH", buffer, TRUE);
    g_snprintf(buffer, sizeof(buffer), "%d", height);
    g_setenv("WPE_OHOS_HEIGHT", buffer, TRUE);

    g_print("окно заведено: поверхность %" G_GUINT64_FORMAT ", %dx%d\n",
        surfaceId, width, height);
    return TRUE;
}

/* Движок сообщает о ходе загрузки через этот сигнал. */
static void onLoadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer)
{
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        g_print("  начата загрузка\n");
        break;
    case WEBKIT_LOAD_COMMITTED:
        g_print("  получен ответ\n");
        break;
    case WEBKIT_LOAD_FINISHED: {
        const char* title = webkit_web_view_get_title(view);
        const char* uri = webkit_web_view_get_uri(view);
        g_print("  готово\n");
        g_print("\nссылка:   %s\n", uri ? uri : "(нет)");
        g_print("заголовок: %s\n", title ? title : "(нет)");
        exitCode = 0;
        /* При выводе на экран выходить сразу незачем — пусть страница
           повисит, чтобы на неё можно было посмотреть. */
        if (!g_getenv("WPE_OHOS_SURFACE_ID"))
            g_main_loop_quit(loop);
        break;
    }
    default:
        break;
    }
}

static void onLoadFailed(WebKitWebView*, WebKitLoadEvent, const char* uri, GError* error, gpointer)
{
    g_printerr("не удалось загрузить %s: %s\n", uri, error->message);
    g_main_loop_quit(loop);
}

/* Страховка на случай, если движок вообще не отзовётся. Когда рисуем
   на экран, она не нужна: там мы висим нарочно. */
static gboolean onTimeout(gpointer)
{
    g_printerr("\nвремя вышло: движок не доложил о завершении\n");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    const char* target = argc > 1 ? argv[1] : NULL;
    const char* platform = g_getenv("WPE_PLATFORM");

    /* Для вывода на экран сперва заводим окно, потом берём площадку:
       она прочтёт номер поверхности при подключении. */
    if (platform && !g_strcmp0(platform, "ohos")) {
        if (!prepareWindow())
            return 1;
    }

    g_print("беру площадку %s\n", platform ? platform : "(по умолчанию)");
    WPEDisplay* display = wpe_display_get_default();
    if (!display) {
        g_printerr("площадка не нашлась\n");
        return 1;
    }
    g_print("площадка готова: %s\n", G_OBJECT_TYPE_NAME(display));

    g_print("создаю обозревательное окно...\n");
    WebKitWebView* view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display, NULL));
    if (!view) {
        g_printerr("окно не создалось\n");
        return 1;
    }


  /* Жалобы страницы — в обычный вывод. Без этого пустая страница
       выглядит просто пустой, без объяснений. */
    WebKitSettings* settings = webkit_web_view_get_settings(view);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);


    /* Движок не рисует, пока окно не объявлено видимым, и берёт размер
       у верхнего уровня, а не у площадки. Скажем и то, и другое. */
    WPEView* wpeView = webkit_web_view_get_wpe_view(view);
    if (wpeView) {
        WPEToplevel* toplevel = wpe_view_get_toplevel(wpeView);
        if (toplevel) {
            const char* w = g_getenv("WPE_OHOS_WIDTH");
            const char* h = g_getenv("WPE_OHOS_HEIGHT");
            wpe_toplevel_resize(toplevel, w ? atoi(w) : 1920, h ? atoi(h) : 1080);
        }
        wpe_view_set_visible(wpeView, TRUE);
        g_print("окно объявлено видимым\n");
    }

    loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect(view, "load-changed", G_CALLBACK(onLoadChanged), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(onLoadFailed), NULL);
    if (!g_getenv("WPE_OHOS_SURFACE_ID"))
        g_timeout_add_seconds(10, onTimeout, NULL);

    if (target) {
        g_print("загружаю %s\n", target);
        webkit_web_view_load_uri(view, target);
    } else {
        g_print("загружаю страницу из памяти\n");
        webkit_web_view_load_html(view,
            "<!DOCTYPE html><html><head><title>Проба пройдена</title></head>"
            "<body style='background:#204080;color:#fff;font-size:64px;"
            "font-family:sans-serif;padding:40px'>"
            "<h1>Привет из WPE WebKit</h1>"
            "<p>OpenHarmony, вывод на экран</p>"
            "</body></html>", NULL);
    }

    g_main_loop_run(loop);
    return exitCode;
}
