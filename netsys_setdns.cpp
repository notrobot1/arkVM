/*
 * Сообщить службе netsys, какими серверами имён пользоваться.
 *
 * В обычной системе это делает служба 1151, когда появляется сетевое
 * соединение: она заводит у netsys хранилище для сети и записывает туда
 * серверы, полученные от DHCP. У нас сетевыми соединениями ведает сама
 * Kali, и регистрировать их в OpenHarmony некому — поэтому говорим прямо.
 *
 * Номер сети: musl при обычном обращении посылает ноль, его и настраиваем.
 *
 *   arkvm_netsys_setdns [сервер ...]
 *
 * Без доводов берёт серверы из /etc/resolv.conf.
 */

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "netsys_controller.h"

namespace {

constexpr uint16_t NET_ID = 0;
constexpr uint16_t TIMEOUT_MS = 5000;
constexpr uint8_t RETRY_COUNT = 2;

// Запасной путь: читаем серверы оттуда же, откуда их берёт вся остальная
// система. Сам musl от OpenHarmony этот файл не читает, но нам он годится
// как источник сведений.
std::vector<std::string> ReadResolvConf()
{
    std::vector<std::string> servers;
    std::ifstream in("/etc/resolv.conf");
    std::string word, rest;
    while (in >> word) {
        if (word == "nameserver" && (in >> rest))
            servers.push_back(rest);
        else
            std::getline(in, rest);
    }
    return servers;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> servers;
    for (int i = 1; i < argc; ++i)
        servers.emplace_back(argv[i]);
    if (servers.empty())
        servers = ReadResolvConf();
    if (servers.empty()) {
        std::fprintf(stderr, "не задано ни одного сервера имён\n");
        return 1;
    }

    auto& netsys = OHOS::NetManagerStandard::NetsysController::GetInstance();

    int32_t ret = netsys.CreateNetworkCache(NET_ID);
    std::printf("завести хранилище для сети %u: %d\n", NET_ID, ret);

    std::vector<std::string> domains;
    ret = netsys.SetResolverConfig(NET_ID, TIMEOUT_MS, RETRY_COUNT, servers, domains);
    std::printf("задать серверы: %d\n", ret);
    for (const auto& s : servers)
        std::printf("    %s\n", s.c_str());

    // Проверяем, что служба их запомнила.
    std::vector<std::string> gotServers, gotDomains;
    uint16_t gotTimeout = 0;
    uint8_t gotRetry = 0;
    ret = netsys.GetResolverConfig(NET_ID, gotServers, gotDomains, gotTimeout, gotRetry);
    std::printf("проверка: %d, серверов %zu\n", ret, gotServers.size());
    for (const auto& s : gotServers)
        std::printf("    %s\n", s.c_str());

    return ret == 0 && !gotServers.empty() ? 0 : 1;
}
