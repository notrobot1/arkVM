#include <cstdio>

#include "init_param.h"

int main()
{
    int ret = InitParamService();
    if (ret != 0) {
        printf("InitParamService failed: %d\n", ret);
        return 1;
    }

    // Порядок как в init: сначала неизменяемые константы, потом остальное.
    LoadDefaultParams("/system/etc/param/ohos_const", LOAD_PARAM_NORMAL);
    LoadDefaultParams("/system/etc/param", LOAD_PARAM_ONLY_ADD);
    LoadSpecialParam();

    printf("param service ready\n");
    return StartParamService();
}
