// Подставка вместо вендорской библиотеки Bluetooth.
// На нашем стенде радиомодуль поднимает ядро Linux: оно же заливает прошивку
// и держит связь с чипом. Наружу ядро отдаёт сырой HCI через сокет
// AF_BLUETOOTH в пользовательском режиме. Поэтому «питание» и «подготовка»
// здесь пустые, а канал — это единственный разъём с обрамлением H4.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <chrono>
#include <thread>
#include "ohos_bt_vendor_lib.h"

namespace {
constexpr int BT_AF = 31;             // AF_BLUETOOTH
constexpr int BT_PROTO_HCI = 1;       // BTPROTO_HCI
constexpr unsigned short CHANNEL_USER = 1;  // HCI_CHANNEL_USER

#define HCI_DEV_UP   _IOW('H', 201, int)
#define HCI_DEV_DOWN _IOW('H', 202, int)

struct SockaddrHci {
    unsigned short family;
    unsigned short dev;
    unsigned short channel;
};

const BtVendorCallbacksT *g_cb = nullptr;
int g_fd = -1;
int g_dev = 0;

void DevCtl(unsigned long request, const char *what)
{
    int ctl = socket(BT_AF, SOCK_RAW | SOCK_CLOEXEC, BT_PROTO_HCI);
    if (ctl < 0) {
        fprintf(stderr, "bt_vendor: управляющий разъём не открылся: %s\n", strerror(errno));
        return;
    }
    if (ioctl(ctl, request, g_dev) < 0) {
        fprintf(stderr, "bt_vendor: %s для hci%d не прошло: %s\n", what, g_dev, strerror(errno));
    }
    close(ctl);
}

int VendorInit(const BtVendorCallbacksT *cb, unsigned char *localBdaddr)
{
    (void)localBdaddr;
    g_cb = cb;
    const char *env = getenv("ARKVM_BT_DEV");
    g_dev = (env != nullptr) ? atoi(env) : 0;
    fprintf(stderr, "bt_vendor: подготовка, устройство hci%d\n", g_dev);
    return 0;
}

int VendorOp(BtOpcodeT opcode, void *param)
{
    switch (opcode) {
        case BT_OP_POWER_ON:
        case BT_OP_POWER_OFF:
            return 0;

        case BT_OP_HCI_CHANNEL_OPEN: {
            if (param == nullptr) {
                return -1;
            }
            // В пользовательском режиме ядро отдаёт адаптер целиком одному
            // хозяину, но лишь когда тот опущен.
            DevCtl(HCI_DEV_DOWN, "опускание");

            int fd = socket(BT_AF, SOCK_RAW | SOCK_CLOEXEC, BT_PROTO_HCI);
            if (fd < 0) {
                fprintf(stderr, "bt_vendor: разъём не открылся: %s\n", strerror(errno));
                return -1;
            }
            SockaddrHci addr {};
            addr.family = BT_AF;
            addr.dev = static_cast<unsigned short>(g_dev);
            addr.channel = CHANNEL_USER;
            if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
                fprintf(stderr, "bt_vendor: привязка к hci%d не удалась: %s\n", g_dev, strerror(errno));
                close(fd);
                return -1;
            }
            g_fd = fd;
            static_cast<int *>(param)[0] = fd;
            fprintf(stderr, "bt_vendor: канал открыт, разъём %d\n", fd);
            

            // ВРЕМЕННО: пробная запись команды сброса управляющего модуля.
            // Нужна, чтобы увидеть, принимает ли ядро запись в разъём и с какой
            // ошибкой отказывает. Убрать, когда разберёмся.
            {
                const unsigned char reset[] = { 0x01, 0x03, 0x0C, 0x00 };
                ssize_t n = write(fd, reset, sizeof(reset));
                if (n != static_cast<ssize_t>(sizeof(reset))) {
                    fprintf(stderr, "BTPROBE: The record failed: %zd, error %d (%s)",
                            n, errno, strerror(errno));
                } else {
                    fprintf(stderr, "BTPROBE: bt_vendor: пробная запись прошла, %zd байт\n", n);
                }
            }



            return 1;  // один разъём — дальше работает разбор H4
        }

        case BT_OP_HCI_CHANNEL_CLOSE:
            if (g_fd >= 0) {
                close(g_fd);
                g_fd = -1;
            }
            return 0;

        case BT_OP_INIT:
                        // Отчитываемся из отдельного потока. Прошивку залило ядро, готовить
            // нечего, но отвечать прямо здесь нельзя: этот вызов идёт внутри
            // посылки, на которую служба ждёт ответа, и встречный оклик
            // упрётся в неё же. Настоящая вендорская библиотека тоже окликает
            // позже, из своего потока.
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (g_cb != nullptr && g_cb->initCb != nullptr) {
                    g_cb->initCb(BTC_OP_RESULT_SUCCESS);
                }
            }).detach();
            return 0;

        case BT_OP_GET_LPM_TIMER:
            //if (param != nullptr) {
            //    *static_cast<unsigned int *>(param) = 0;
            //}
            //return 0;

            // Сбережением питания у нас никто не занимается, но нулевой срок
            // слой драйверов принимает за «немедленно». Отдаём заведомо
            // большой, чтобы наблюдатель не дёргался попусту.
            if (param != nullptr) {
                *static_cast<unsigned int *>(param) = 30000;  // мс
            }
            return 0;

        default:
            // Сбережение питания и удержание пробуждения нам не нужны.
            return 0;
    }
}

void VendorClose()
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    // Возвращаем адаптер системе, чтобы им снова могла пользоваться Kali.
    DevCtl(HCI_DEV_UP, "поднятие");
    g_cb = nullptr;
}
}  // namespace

extern "C" __attribute__((visibility("default")))
BtVendorInterfaceT BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(BtVendorInterfaceT),
    VendorInit,
    VendorOp,
    VendorClose,
};
