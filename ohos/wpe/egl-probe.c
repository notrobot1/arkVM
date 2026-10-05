/*
 * Разведка EGL в OpenHarmony.
 *
 * Выясняет, какие способы подключения к видеоподсистеме доступны и какие
 * расширения есть для работы с буферами системы — от последнего зависит,
 * как мы будем отдавать нарисованное на экран.
 *
 * Запросы с пустым подключением (EGL_NO_DISPLAY) намеренно не делаются:
 * в OpenHarmony они возвращают мусор и роняют программу.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

static void printList(const char* what, const char* list)
{
    printf("  %s:\n", what);
    if (!list || !*list) {
        printf("    (пусто)\n");
        return;
    }
    for (const char* p = list; *p; ) {
        const char* s = p;
        while (*p && *p != ' ') p++;
        if (p > s)
            printf("    %.*s\n", (int)(p - s), s);
        while (*p == ' ') p++;
    }
}

static void describe(const char* name, EGLDisplay d)
{
    EGLint major = 0, minor = 0;
    if (!eglInitialize(d, &major, &minor)) {
        printf("  %-14s — подключение есть, но не запускается: 0x%04x\n",
            name, eglGetError());
        return;
    }
    printf("  %-14s — РАБОТАЕТ, EGL %d.%d, поставщик: %s\n",
        name, major, minor, eglQueryString(d, EGL_VENDOR));
    eglTerminate(d);
}

static void tryPlatform(const char* name, EGLenum platform)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get) {
        printf("  %-14s — нет самого eglGetPlatformDisplayEXT\n", name);
        return;
    }
    eglGetError();
    EGLDisplay d = get(platform, EGL_DEFAULT_DISPLAY, NULL);
    if (d == EGL_NO_DISPLAY) {
        printf("  %-14s — отказ, ошибка 0x%04x\n", name, eglGetError());
        return;
    }
    describe(name, d);
}

int main(void)
{
    printf("способы подключения:\n");

    eglGetError();
    EGLDisplay def = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (def == EGL_NO_DISPLAY) {
        printf("  %-14s — отказ, ошибка 0x%04x\n", "по умолчанию", eglGetError());
        return 1;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(def, &major, &minor)) {
        printf("  %-14s — не запускается: 0x%04x\n", "по умолчанию", eglGetError());
        return 1;
    }
    printf("  %-14s — РАБОТАЕТ, EGL %d.%d\n", "по умолчанию", major, minor);

    tryPlatform("безэкранный", EGL_PLATFORM_SURFACELESS_MESA);
    tryPlatform("gbm", EGL_PLATFORM_GBM_KHR);

    printf("\nподключение по умолчанию:\n");
    printf("  поставщик: %s\n", eglQueryString(def, EGL_VENDOR));
    printf("  версия:    %s\n", eglQueryString(def, EGL_VERSION));
    printf("  наборы:    %s\n", eglQueryString(def, EGL_CLIENT_APIS));
    printList("расширения", eglQueryString(def, EGL_EXTENSIONS));

    eglTerminate(def);
    return 0;
}
