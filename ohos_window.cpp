/*
 * Заведение окна для площадки WPE.
 *
 * Зачем отдельная библиотека. Номер поверхности в OpenHarmony —
 * внутрипроцессный: SurfaceUtils хранит их в обычном списке в памяти
 * процесса, и другой процесс по такому номеру ничего не найдёт. Значит тот,
 * кто заводит окно, и тот, кто в него рисует, должны быть одним процессом.
 *
 * Площадка WPE собирается в стороне, лишь по пакету для разработчиков, и
 * до службы отрисовки не дотягивается. Поэтому окно заводит эта библиотека,
 * собранная в дереве OpenHarmony, а площадка подгружает её при запуске и
 * спрашивает номер.
 *
 * Наружу выставлены только простые вызовы на C: через эту границу не
 * проходит ни одного составного значения, и разные выпуски стандартной
 * библиотеки C++ по обе стороны друг другу не мешают.
 */

#include <cstdint>
#include <memory>

#include "display_manager.h"
#include "transaction/rs_transaction.h"
#include "ui/rs_surface_node.h"

using namespace OHOS;
using namespace OHOS::Rosen;

namespace {
std::shared_ptr<RSSurfaceNode> g_node;
uint64_t g_displayId = 0;
}

extern "C" {

/*
 * Завести окно во весь экран и вернуть номер его поверхности.
 * Возвращает 0 при неудаче. Размеры записываются по указателям,
 * если они не пустые.
 */
uint64_t arkvm_window_create(int* width, int* height)
{
    auto display = DisplayManager::GetInstance().GetDefaultDisplay();
    if (!display)
        return 0;

    g_displayId = display->GetId();
    const int w = display->GetWidth();
    const int h = display->GetHeight();

    // Самостоятельный узел: рисует прямо в своё окно, минуя разметку.
    RSSurfaceNodeConfig config = { "WPEWebKit" };
    g_node = RSSurfaceNode::Create(config, RSSurfaceNodeType::SELF_DRAWING_WINDOW_NODE);
    if (!g_node)
        return 0;

    g_node->SetFrameGravity(Gravity::RESIZE_ASPECT_FILL);
    g_node->SetPositionZ(RSSurfaceNode::POINTER_WINDOW_POSITION_Z);
    g_node->SetBounds(0, 0, w, h);
    g_node->AttachToDisplay(g_displayId);
    RSTransaction::FlushImplicitTransaction();

    sptr<Surface> surface = g_node->GetSurface();
    if (surface == nullptr) {
        g_node.reset();
        return 0;
    }

    if (width)
        *width = w;
    if (height)
        *height = h;
    return surface->GetUniqueId();
}

/* Убрать окно. */
void arkvm_window_destroy(void)
{
    if (!g_node)
        return;
    g_node->DetachToDisplay(g_displayId);
    RSTransaction::FlushImplicitTransaction();
    g_node.reset();
}

} // extern "C"
