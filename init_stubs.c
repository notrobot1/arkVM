/*
 * Части init, которых у нас нет по существу.
 *
 * Хранилище системных параметров умеет исполнять команды: запись в
 * ctl.start запускает сервис, в ohos.startup.powerctrl — перезагружает
 * машину. В нашей системе сервисами управляет start.sh, а перезагружать
 * рабочую машину мы точно не хотим. Поэтому списки команд пусты, а сами
 * команды честно ничего не делают и сообщают об этом в журнал.
 */
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include "beget_ext.h"
#include "bootstage.h"
#include "hookmgr.h"
#include "init_cmds.h"
#include "init_hook.h"
#include "init_utils.h"

// ---- команды управления сервисами через параметры ----

const ParamCmdInfo *GetServiceStartCtrl(size_t *size)
{
    if (size != NULL) {
        *size = 0;
    }
    return NULL;
}

const ParamCmdInfo *GetServiceCtl(size_t *size)
{
    if (size != NULL) {
        *size = 0;
    }
    return NULL;
}

const ParamCmdInfo *GetStartupPowerCtl(size_t *size)
{
    if (size != NULL) {
        *size = 0;
    }
    return NULL;
}

const ParamCmdInfo *GetOtherSpecial(size_t *size)
{
    if (size != NULL) {
        *size = 0;
    }
    return NULL;
}

// ---- команды init из *.cfg ----

const char *GetMatchCmd(const char *cmdStr, int *index)
{
    (void)cmdStr;
    if (index != NULL) {
        *index = -1;
    }
    return NULL;
}

const char *GetCmdKey(int index)
{
    (void)index;
    return "unsupported";
}

void DoCmdByIndex(int index, const char *cmdContent, const ConfigContext *context)
{
    (void)context;
    BEGET_LOGW("init command %d (%s) is not supported here", index,
        cmdContent != NULL ? cmdContent : "");
}

// Подстановка значений вида ${name} при импорте файлов параметров.
// Без движка команд init подставляем исходную строку как есть.
int GetParamValue(const char *symValue, unsigned int symLen, char *paramValue, unsigned int paramLen)
{
    if (symValue == NULL || paramValue == NULL || paramLen == 0) {
        return -1;
    }
    unsigned int len = (symLen < paramLen - 1) ? symLen : paramLen - 1;
    (void)memcpy(paramValue, symValue, len);
    paramValue[len] = '\0';
    return 0;
}

void ExecReboot(const char *value)
{
    BEGET_LOGW("reboot request ignored: %s", value != NULL ? value : "");
}

// ---- прочее ----

int GetServiceGroupIdByPid(pid_t pid, gid_t *gids, uint32_t gidSize)
{
    (void)pid;
    (void)gids;
    (void)gidSize;
    return 0;
}

void PluginExecCmdByName(const char *name, const char *cmdContent)
{
    BEGET_LOGW("plugin command %s is not supported here", name != NULL ? name : "");
    (void)cmdContent;
}

HOOK_MGR *GetBootStageHookMgr(void)
{
    static HOOK_MGR *mgr = NULL;
    if (mgr == NULL) {
        mgr = HookMgrCreate("bootstage");
    }
    return mgr;
}
