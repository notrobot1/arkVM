/*
 * Проба WPE WebKit под OpenHarmony: безэкранная площадка.
 *
 * Создаёт площадку без вывода на экран, загружает в неё страницу и печатает
 * заголовок. Если это работает, значит живы: сам движок, отдельный процесс
 * отрисовки, межпроцессное взаимодействие, разбор разметки, шрифты и —
 * если дать ссылку в сеть — обмен по сети с защищёнными соединениями.
 *
 * Сборка и запуск — см. smoke.sh
 */

#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>

static GMainLoop* loop = NULL;
static int exitCode = 1;

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

/* Страховка: если за десять секунд ничего не произошло — выходим,
   иначе при неполадке программа повиснет навсегда. */
static gboolean onTimeout(gpointer)
{
    g_printerr("\nвремя вышло: движок не доложил о завершении\n");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    const char* target = argc > 1 ? argv[1] : NULL;

    g_print("создаю безэкранную площадку...\n");
    WPEDisplay* display = wpe_display_headless_new();
    if (!display) {
        g_printerr("площадка не создалась\n");
        return 1;
    }

    GError* error = NULL;
    if (!wpe_display_connect(display, &error)) {
        g_printerr("площадка не подключилась: %s\n", error->message);
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

    loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect(view, "load-changed", G_CALLBACK(onLoadChanged), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(onLoadFailed), NULL);
    g_timeout_add_seconds(10, onTimeout, NULL);

    if (target) {
        g_print("загружаю %s\n", target);
        webkit_web_view_load_uri(view, target);
    } else {
        g_print("загружаю страницу из памяти\n");
        webkit_web_view_load_html(view,
            "<!DOCTYPE html><html><head><title>Проба пройдена</title></head>"
            "<body><h1>Привет из WPE WebKit</h1>"
            "<script>document.title = 'Проба пройдена, JavaScript работает';</script>"
            "</body></html>", NULL);
    }

    g_main_loop_run(loop);
    return exitCode;
}
