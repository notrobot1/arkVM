/*
 * Окно без приложения.
 *
 * Создаёт у службы отрисовки самостоятельный узел поверхности, цепляет его
 * к изображению и достаёт оконный указатель. Дальше — два занятия на выбор:
 *
 *   arkvm_surface_probe [секунд]
 *       закрашивает окно переливающимся цветом через EGL. Проверка того,
 *       что путь к экрану вообще работает.
 *
 *   arkvm_surface_probe hold
 *       только создаёт окно, называет его номер и держит. Рисовать будет
 *       кто-то другой — например, обозреватель, которому этот номер
 *       передадут через переменную WPE_OHOS_SURFACE_ID.
 *
 * Последовательность взята у образца drawing_sample_replayer.
 */

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "display_manager.h"
#include "transaction/rs_transaction.h"
#include "ui/rs_surface_node.h"
#include "window.h"

using namespace OHOS;
using namespace OHOS::Rosen;

namespace {

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;

bool SetUpEGL(EGLNativeWindowType window)
{
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        std::printf("подключение к EGL не получено\n");
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_display, &major, &minor)) {
        std::printf("EGL не запускается: 0x%04x\n", eglGetError());
        return false;
    }
    std::printf("EGL %d.%d, поставщик: %s\n", major, minor,
        eglQueryString(g_display, EGL_VENDOR));

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(g_display, configAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        std::printf("подходящего набора свойств не нашлось: 0x%04x\n", eglGetError());
        return false;
    }

    g_surface = eglCreateWindowSurface(g_display, config, window, nullptr);
    if (g_surface == EGL_NO_SURFACE) {
        std::printf("поверхность не создалась: 0x%04x\n", eglGetError());
        return false;
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_context == EGL_NO_CONTEXT) {
        std::printf("не создалось окружение отрисовки: 0x%04x\n", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        std::printf("окружение не выбралось: 0x%04x\n", eglGetError());
        return false;
    }
    std::printf("рисуем через %s\n", glGetString(GL_RENDERER));
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    // ИЗМЕНЕНО: довод может быть словом «hold», поэтому читаем число
    // только когда это действительно число.
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool holdOnly = (mode == "hold");
    const int seconds = (!mode.empty() && !holdOnly) ? std::stoi(mode) : 10;

    auto defaultDisplay = DisplayManager::GetInstance().GetDefaultDisplay();
    if (!defaultDisplay) {
        std::printf("изображение не найдено: служба отрисовки не отвечает?\n");
        return 1;
    }
    uint64_t displayId = defaultDisplay->GetId();
    uint32_t width = static_cast<uint32_t>(defaultDisplay->GetWidth());
    uint32_t height = static_cast<uint32_t>(defaultDisplay->GetHeight());
    std::printf("изображение %llu, размер %ux%u\n",
        static_cast<unsigned long long>(displayId), width, height);

    // Самостоятельный узел: такой рисует сам в своё окно, минуя разметку.
    RSSurfaceNodeConfig nodeConfig = { "ArkvmSurfaceProbe" };
    auto surfaceNode = RSSurfaceNode::Create(nodeConfig, RSSurfaceNodeType::SELF_DRAWING_WINDOW_NODE);
    if (!surfaceNode) {
        std::printf("узел поверхности не создался\n");
        return 1;
    }

    surfaceNode->SetFrameGravity(Gravity::RESIZE_ASPECT_FILL);
    surfaceNode->SetPositionZ(RSSurfaceNode::POINTER_WINDOW_POSITION_Z);
    surfaceNode->SetBounds(0, 0, width, height);
    surfaceNode->AttachToDisplay(displayId);
    RSTransaction::FlushImplicitTransaction();
    std::printf("узел прицеплен к изображению\n");

    sptr<Surface> ohosSurface = surfaceNode->GetSurface();
    if (ohosSurface == nullptr) {
        std::printf("у узла нет поверхности\n");
        return 1;
    }

    // ДОБАВЛЕНО: номер поверхности. По нему другой процесс сможет получить
    // то же самое окно вызовом OH_NativeWindow_CreateNativeWindowFromSurfaceId.
    std::printf("номер поверхности: %llu\n",
        static_cast<unsigned long long>(ohosSurface->GetUniqueId()));
    std::fflush(stdout);

    // ДОБАВЛЕНО: режим «подержать». Окно создано и живо, рисует кто-то другой.
    // Узел поверхности существует, пока жив этот процесс, поэтому просто ждём.
    if (holdOnly) {
        std::printf("держу окно, прервите по Ctrl+C\n");
        std::fflush(stdout);
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    OHNativeWindow* nativeWindow = CreateNativeWindowFromSurface(&ohosSurface);
    if (nativeWindow == nullptr) {
        std::printf("окно из поверхности не получилось\n");
        return 1;
    }
    std::printf("окно получено\n");

    // Размер буфера задаётся ДО того, как у окна просят поверхность EGL:
    // иначе слой связи с Vulkan не заведёт ни одного образа для обмена.
    NativeWindowHandleOpt(nativeWindow, SET_BUFFER_GEOMETRY, width, height);

    if (!SetUpEGL(reinterpret_cast<EGLNativeWindowType>(nativeWindow)))
        return 1;

    // Плавно меняем цвет, чтобы было видно, что кадры действительно идут.
    std::printf("рисую %d секунд\n", seconds);
    const int fps = 30;
    for (int frame = 0; frame < seconds * fps; ++frame) {
        float phase = static_cast<float>(frame % (fps * 3)) / (fps * 3);
        glClearColor(phase, 0.3f, 1.0f - phase, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!eglSwapBuffers(g_display, g_surface)) {
            std::printf("кадр не отдался: 0x%04x\n", eglGetError());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps));
    }

    std::printf("готово\n");
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_display, g_surface);
    eglDestroyContext(g_display, g_context);
    eglTerminate(g_display);
    DestoryNativeWindow(nativeWindow);
    surfaceNode->DetachToDisplay(displayId);
    RSTransaction::FlushImplicitTransaction();
    return 0;
}
