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
 *   WPE_PLATFORM=headless            рисовать в память, ничего не показывая;
 *   WPE_PLATFORM=ohos                рисовать в окно OpenHarmony. Тогда нужен
 *   WPE_OHOS_SURFACE_ID=<номер>      номер поверхности от хозяина окна
 *                                    (его называет arkvm_surface_probe hold).
 *
 * Сборка и запуск — см. smoke.sh
 */

#include <wpe/webkit.h>

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

/* Страховка: если за десять секунд ничего не произошло — выходим,
   иначе при неполадке программа повиснет навсегда. Когда рисуем на экран,
   страховка не нужна: там мы висим нарочно. */
static gboolean onTimeout(gpointer)
{
    g_printerr("\nвремя вышло: движок не доложил о завершении\n");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    const char* target = argc > 1 ? argv[1] : NULL;

    /* ИЗМЕНЕНО: раньше площадка задавалась прямо в коде (безэкранная).
       Теперь берём ту, что выбрана переменной WPE_PLATFORM: так одна и та же
       проба годится и для отрисовки в память, и для вывода на экран. */
    g_print("беру площадку %s\n", g_getenv("WPE_PLATFORM") ?: "(по умолчанию)");
    WPEDisplay* display = wpe_display_get_primary();
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
