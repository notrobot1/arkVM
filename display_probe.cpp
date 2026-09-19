/*
 * Проверка слоя HDI дисплея.
 *
 * Обращается к службе композитора, подписывается на события подключения
 * экранов и печатает то, что о них известно: имя, физический размер,
 * поддерживаемые режимы и текущий режим.
 *
 * Если программа доходит до вывода режимов, значит работает вся цепочка:
 * клиент → служба композитора → драйвер HDF → эталонная реализация → DRM.
 */

#include <cstdio>
#include <unistd.h>
#include <vector>

#include "v1_5/include/idisplay_composer_interface.h"
#include "v1_0/display_composer_type.h"

using namespace OHOS::HDI::Display::Composer;

namespace {
std::vector<uint32_t> g_displays;

void OnHotPlug(uint32_t devId, bool connected, void *data)
{
    (void)data;
    printf("событие: экран %u %s\n", devId, connected ? "подключён" : "отключён");
    if (connected) {
        g_displays.push_back(devId);
    }
}
} // namespace

int main()
{
    auto dev = V1_5::IDisplayComposerInterface::Get();
    if (dev == nullptr) {
        printf("служба композитора недоступна\n");
        return 1;
    }
    printf("служба композитора получена\n");

    int32_t ret = dev->RegHotPlugCallback(OnHotPlug, nullptr);
    printf("подписка на события подключения: ret=%d\n", ret);

    // Реализация сообщает об уже подключённых экранах сразу после подписки,
    // но делает это из своего потока — дадим ей время.
    sleep(2);

    if (g_displays.empty()) {
        printf("экранов не обнаружено\n");
        return 1;
    }

    for (uint32_t id : g_displays) {
        V1_0::DisplayCapability cap;
        ret = dev->GetDisplayCapability(id, cap);
        if (ret == 0) {
            printf("экран %u: имя=%s тип=%d физический размер=%ux%u мм, слоёв=%u\n",
                   id, cap.name.c_str(), static_cast<int>(cap.type),
                   cap.phyWidth, cap.phyHeight, cap.supportLayers);
        } else {
            printf("экран %u: не удалось получить сведения, ret=%d\n", id, ret);
        }

        std::vector<V1_0::DisplayModeInfo> modes;
        ret = dev->GetDisplaySupportedModes(id, modes);
        if (ret == 0) {
            printf("  режимов: %zu\n", modes.size());
            for (const auto &m : modes) {
                printf("    режим %d: %dx%d, частота %u\n", m.id, m.width, m.height, m.freshRate);
            }
        } else {
            printf("  не удалось получить список режимов, ret=%d\n", ret);
        }

        uint32_t modeId = 0;
        ret = dev->GetDisplayMode(id, modeId);
        printf("  текущий режим: %u (ret=%d)\n", modeId, ret);
    }

    return 0;
}
