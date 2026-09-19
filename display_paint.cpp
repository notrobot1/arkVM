/*
 * Заливка экрана через слой HDI дисплея.
 *
 * Порядок действий тот же, что у любого композитора:
 *   выбрать режим экрана → включить питание → выделить буфер кадра →
 *   заполнить его → отдать композитору как клиентский буфер → зафиксировать кадр.
 *
 * Если на мониторе появятся цвета, значит работает вся цепочка вывода:
 * распределитель буферов, композитор, эталонная реализация и DRM.
 */

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "v1_5/include/idisplay_composer_interface.h"
#include "v1_0/include/idisplay_buffer.h"
#include "v1_0/display_composer_type.h"

using namespace OHOS::HDI::Display::Composer;
using OHOS::HDI::Display::Buffer::V1_0::IDisplayBuffer;

using OHOS::HDI::Display::Buffer::V1_0::IDisplayBuffer;
namespace BufferV1_0 = OHOS::HDI::Display::Buffer::V1_0;

namespace {
constexpr int SHOW_SECONDS = 3;

std::vector<uint32_t> g_displays;

void OnHotPlug(uint32_t devId, bool connected, void *data)
{
    (void)data;
    if (connected) {
        g_displays.push_back(devId);
    }
}

// Заполнить буфер одним цветом. Идём построчно: длина строки в байтах
// задаётся полем stride, оно может быть больше, чем ширина на четыре байта.
void FillBuffer(void *addr, const BufferHandle &handle, uint32_t argb)
{
    auto *base = static_cast<uint8_t *>(addr);
    for (int32_t y = 0; y < handle.height; y++) {
        auto *row = reinterpret_cast<uint32_t *>(base + static_cast<size_t>(y) * handle.stride);
        for (int32_t x = 0; x < handle.width; x++) {
            row[x] = argb;
        }
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
    IDisplayBuffer *alloc = IDisplayBuffer::Get();
    if (alloc == nullptr) {
        printf("распределитель буферов недоступен\n");
        return 1;
    }
    printf("службы получены\n");

    dev->RegHotPlugCallback(OnHotPlug, nullptr);
    sleep(2);
    if (g_displays.empty()) {
        printf("экранов не обнаружено\n");
        return 1;
    }
    uint32_t devId = g_displays[0];

    // Выбираем режим с наибольшей частотой обновления.
    std::vector<V1_0::DisplayModeInfo> modes;
    int32_t ret = dev->GetDisplaySupportedModes(devId, modes);
    if (ret != 0 || modes.empty()) {
        printf("не удалось получить режимы, ret=%d\n", ret);
        return 1;
    }
    const V1_0::DisplayModeInfo *best = &modes[0];
    for (const auto &m : modes) {
        if (m.freshRate > best->freshRate) {
            best = &m;
        }
    }
    printf("выбран режим %d: %dx%d, частота %u\n", best->id, best->width, best->height, best->freshRate);

    ret = dev->SetDisplayMode(devId, static_cast<uint32_t>(best->id));
    printf("установка режима: ret=%d\n", ret);

    ret = dev->SetDisplayPowerStatus(devId, V1_0::POWER_STATUS_ON);
    printf("включение экрана: ret=%d\n", ret);

    // Буфер кадра: доступен процессору на запись и пригоден для вывода.
    BufferV1_0::AllocInfo info {};
    info.width = static_cast<uint32_t>(best->width);
    info.height = static_cast<uint32_t>(best->height);
    info.usage = V1_0::HBM_USE_MEM_DMA | V1_0::HBM_USE_CPU_WRITE | V1_0::HBM_USE_MEM_FB;
    info.format = V1_0::PIXEL_FMT_RGBA_8888;
    info.expectedSize = info.width * info.height * 4;

    BufferHandle *handle = nullptr;
    ret = alloc->AllocMem(info, handle);
    if (ret != 0 || handle == nullptr) {
        printf("не удалось выделить буфер, ret=%d\n", ret);
        return 1;
    }
    printf("буфер выделен: %dx%d, строка %d байт, размер %d\n",
           handle->width, handle->height, handle->stride, handle->size);

    void *addr = alloc->Mmap(*handle);
    if (addr == nullptr) {
        printf("не удалось отобразить буфер в память\n");
        return 1;
    }

    std::vector<V1_0::IRect> damage;
    V1_0::IRect full { 0, 0, handle->width, handle->height };
    damage.push_back(full);

    struct { const char *name; uint32_t argb; } colors[] = {
        { "красный", 0xFFFF0000 },
        { "зелёный", 0xFF00FF00 },
        { "синий",   0xFF0000FF },
    };

    for (const auto &c : colors) {
        FillBuffer(addr, *handle, c.argb);
        alloc->FlushCache(*handle);

        ret = dev->SetDisplayClientBuffer(devId, handle, 0, -1);
        if (ret != 0) {
            printf("%s: SetDisplayClientBuffer ret=%d\n", c.name, ret);
        }
        ret = dev->SetDisplayClientDamage(devId, damage);
        if (ret != 0) {
            printf("%s: SetDisplayClientDamage ret=%d\n", c.name, ret);
        }

        bool needFlush = false;
        ret = dev->PrepareDisplayLayers(devId, needFlush);
        if (ret != 0) {
            printf("%s: PrepareDisplayLayers ret=%d\n", c.name, ret);
        }

        int32_t fence = -1;
        ret = dev->Commit(devId, fence);
        printf("%s: кадр отправлен, ret=%d\n", c.name, ret);

        sleep(SHOW_SECONDS);
    }

    alloc->Unmap(*handle);
    alloc->FreeMem(*handle);
    printf("готово\n");
    return 0;
}
