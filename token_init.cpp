#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "nativetoken_kit.h"

namespace {

// Разрешения, нужные системным процессам нашего стенда. На устройстве
// у каждого сервиса свой список, заданный в его исходниках; здесь мы
// играем роль того, кто наполняет реестр нативных токенов при сборке образа.
const char *PERMS[] = {
    "ohos.permission.GET_BUNDLE_INFO_PRIVILEGED",
    "ohos.permission.INSTALL_BUNDLE",
    "ohos.permission.UNINSTALL_BUNDLE",
    "ohos.permission.GET_INSTALLED_BUNDLE_LIST",
    "ohos.permission.LISTEN_BUNDLE_CHANGE",
    "ohos.permission.REMOVE_CACHE_FILES",
    "ohos.permission.CHANGE_ABILITY_ENABLED_STATE",
    "ohos.permission.STORAGE_MANAGER",
    "ohos.permission.STORAGE_MANAGER_CRYPT",
    "ohos.permission.MOUNT_UNMOUNT_MANAGER",
    "ohos.permission.MOUNT_FORMAT_MANAGER",
    "ohos.permission.MANAGE_LOCAL_ACCOUNTS",
    "ohos.permission.INTERACT_ACROSS_LOCAL_ACCOUNTS",
    "ohos.permission.GET_LOCAL_ACCOUNTS",
    "ohos.permission.INTERACT_ACROSS_LOCAL_ACCOUNTS_EXTENSION",
    "ohos.permission.MANAGE_HAP_TOKENID",
    "ohos.permission.GRANT_SENSITIVE_PERMISSIONS",
    "ohos.permission.REVOKE_SENSITIVE_PERMISSIONS",
    "ohos.permission.GET_SENSITIVE_PERMISSIONS",
    "ohos.permission.GET_RUNNING_INFO",
    "ohos.permission.START_ABILITIES_FROM_BACKGROUND",
    "ohos.permission.START_INVISIBLE_ABILITY",
    "ohos.permission.ABILITY_BACKGROUND_COMMUNICATION",
    "ohos.permission.USE_USER_IDM",
    "ohos.permission.MANAGE_USER_IDM",
    "ohos.permission.ACCESS_USER_AUTH_INTERNAL",
};

// Имена процессов ровно те, под которыми они видны в /proc/<pid>/cmdline.
// Для сервисов под sa_main это имя профиля: каркас переименовывает процесс.
const char *PROCESSES[] = {
    "samgr",
    "accesstoken_service",
    "installs",
    "bms",
    "bm",
    "hilogd",
    "samgr_client",
    "ipc_test_client",
    "ipc_test_server",
    "accountmgr",
    "storage_daemon",
    "storage_manager",
    "acm",
    "foundation",
    "bundle_test_tool",
    "appspawn",
    "aa",
    "hdf_devmgr",
    "composer_host",
    "allocator_host",
    "render_service",
    "multimodalinput",
    "param_watcher",
    "inputmethod_service",
    "distributeddata",
    "screenlock_server",
    "useriam",
    "powermgr",
    "audio_server",
    "bluetooth_service",
};

constexpr const char *BYNAME_DIR = "/data/service/el0/access_token/byname";

bool WriteToken(const char *name, uint64_t token)
{
    std::string path = std::string(BYNAME_DIR) + "/" + name;
    FILE *f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        printf("cannot write %s\n", path.c_str());
        return false;
    }
    fprintf(f, "%llu\n", static_cast<unsigned long long>(token));
    fclose(f);
    return true;
}

} // namespace

int main()
{
    int permsNum = static_cast<int>(sizeof(PERMS) / sizeof(PERMS[0]));

    for (const char *name : PROCESSES) {
        NativeTokenInfoParams info = {
            .dcapsNum = 0,
            .permsNum = permsNum,
            .aclsNum = 0,
            .dcaps = nullptr,
            .perms = PERMS,
            .acls = nullptr,
            .processName = name,
            .aplStr = "system_core",
            .uid = 0,
        };

        uint64_t token = GetAccessTokenId(&info);
        printf("%-22s -> %llu\n", name, static_cast<unsigned long long>(token));
        WriteToken(name, token);
    }
    return 0;
}
