/*
 * Разведка EGL в OpenHarmony.
 *
 * Выясняет, какие способы подключения к видеоподсистеме доступны: безэкранный
 * (нужен нынешней площадке WPE), обычный по умолчанию, и какие расширения
 * есть для работы с буферами системы — от последнего зависит, как мы будем
 * отдавать нарисованное на экран.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>

/* В заголовках OpenHarmony этих обозначений может не быть. */
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

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
    EGLint err = eglGetError();
    if (d == EGL_NO_DISPLAY) {
        printf("  %-14s — отказ, ошибка 0x%04x\n", name, err);
        return;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(d, &major, &minor)) {
        printf("  %-14s — подключение есть, но не запускается: 0x%04x\n", name, eglGetError());
        return;
    }
    printf("  %-14s — РАБОТАЕТ, EGL %d.%d, поставщик: %s\n",
        name, major, minor, eglQueryString(d, EGL_VENDOR));
    eglTerminate(d);
}

int main(void)
{
    const char* clientExt = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("расширения до подключения:\n  %s\n\n", clientExt ? clientExt : "(пусто)");

    printf("способы подключения:\n");
    tryPlatform("безэкранный", EGL_PLATFORM_SURFACELESS_MESA);
    tryPlatform("gbm", EGL_PLATFORM_GBM_KHR);

    eglGetError();
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY) {
        printf("  %-14s — отказ, ошибка 0x%04x\n", "по умолчанию", eglGetError());
        return 1;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(d, &major, &minor)) {
        printf("  %-14s — не запускается: 0x%04x\n", "по умолчанию", eglGetError());
        return 1;
    }
    printf("  %-14s — РАБОТАЕТ, EGL %d.%d\n", "по умолчанию", major, minor);

    printf("\nподробности подключения по умолчанию:\n");
    printf("  поставщик: %s\n", eglQueryString(d, EGL_VENDOR));
    printf("  версия:    %s\n", eglQueryString(d, EGL_VERSION));
    printf("  наборы:    %s\n", eglQueryString(d, EGL_CLIENT_APIS));
    printf("  расширения:\n");
    const char* ext = eglQueryString(d, EGL_EXTENSIONS);
    for (const char* p = ext; p && *p; ) {
        const char* s = p;
        while (*p && *p != ' ') p++;
        printf("    %.*s\n", (int)(p - s), s);
        while (*p == ' ') p++;
    }
    eglTerminate(d);
    return 0;
}
