#!/usr/bin/env bash
# Запуск системного слоя arkVM в правильном порядке.
#   sudo ./start.sh          — поднять всё
#   sudo ./start.sh stop     — погасить
#   sudo ./start.sh status   — что зарегистрировано в samgr
#   sudo ./start.sh install  — разложить собранное по системным каталогам
#   sudo ./start.sh tokens   — стереть реестр токенов и базу ATM
#   sudo ./start.sh reset    — стереть всё изменяемое состояние
#
# ВНИМАНИЕ: tokens и reset стирают базу маркеров доступа вместе с маркерами
# установленных приложений. После них приложения не запустятся, пока их не
# переустановишь (bm install -p ...). Для рабочего стола это обязательно.

set -u

#export OHOS_MESA_DRIVER=swrast
BIN=${BIN:-/system/bin}
LIB=${LIB:-/system/lib64}
LOGDIR=${LOGDIR:-/tmp/arkvm}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="$(dirname "$SCRIPT_DIR")"
OUT=${OUT:-$TREE/out/arkvm}

export LD_LIBRARY_PATH=/system/lib64:/system/lib64/platformsdk:/system/lib64/chipset-sdk:/system/lib64/chipset-sdk-sp:/system/lib64/ndk:/vendor/lib64:/vendor/lib64/passthrough

SAMGR_CLIENT=$BIN/samgr_client
SA_MAIN=$BIN/sa_main

TOKEN_BYNAME=/data/service/el0/access_token/byname
TOKEN_BYPID=/data/service/el0/access_token/bypid


# ---------------------------------------------------------------------------
# Каталоги, которые на устройстве создаёт init по описаниям в *.cfg.
# ---------------------------------------------------------------------------
ensure_dirs() {
    local u=100

    mkdir -p "$LOGDIR"
    mkdir -p /dev/unix/socket
    mkdir -p /system/profile
    mkdir -p /system/etc/{app,account,storage_daemon}
    mkdir -p /system/etc/param/ohos_const

    # реестр токенов и база прав
    mkdir -p "$TOKEN_BYNAME" "$TOKEN_BYPID"
    mkdir -p /data/service/el1/public/access_token
    chmod 777 "$TOKEN_BYPID"

    # менеджер пакетов
    mkdir -p /data/service/el1/public/bms/bundle_manager_service
    mkdir -p /data/bundlemgr

    # учётные записи и хранилище
    mkdir -p /data/service/el1/public/account
    mkdir -p /data/service/el1/public/storage_daemon
    mkdir -p /data/service/el1/public/storage_manager/database

    # распределённое хранилище ключ-значение (нужно менеджеру учётных записей)
    mkdir -p /data/service/el1/public/database
    mkdir -p /data/service/el1/public/distributeddata
    mkdir -p /data/service/el1/public/config

    # уровни шифрования данных
    for lvl in el1 el2 el3 el4 el5; do
        mkdir -p /data/service/$lvl
        mkdir -p /data/chipset/$lvl
        mkdir -p /data/app/$lvl/$u
    done
    mkdir -p /data/service/el1/public

    # раскладка данных пользователя;
    # список из /system/etc/storage_daemon/storage_user_path.json
    mkdir -p /data/app/el1/$u/aot_compiler/ark_profile
    mkdir -p /data/app/el1/bundle/public
    mkdir -p /data/app/el1/public/shader_cache/cloud
    mkdir -p /data/app/el2/$u/base
    mkdir -p /data/app/el2/$u/database
    mkdir -p /data/app/el2/$u/sharefiles
    mkdir -p /data/chipset/el2/$u/multimedia
    mkdir -p /data/service/el1/$u/{backup,for-all-app}
    mkdir -p /data/service/el2/$u/{backup,database,findnetwork,fusion_awareness}
    mkdir -p /data/service/el2/$u/{gameservice_server,push_manager_service,virt_service}
    mkdir -p /data/service/el2/$u/hmdfs/{account,cache,cloud,non_account}
    mkdir -p /data/service/el2/$u/hmdfs/account/files/Docs
    mkdir -p /data/service/el4/$u
    mkdir -p /mnt/data/$u/media_fuse
    mkdir -p /mnt/hmdfs/$u
    mkdir -p /mnt/sandbox
    mkdir -p /mnt/share
    mkdir -p /mnt/user/$u/currentUser
    mkdir -p /mnt/user/$u/nosharefs/appdata/el{1,2,5}
    mkdir -p /mnt/user/$u/nosharefs/docs
    mkdir -p /mnt/user/$u/sharefs/docs
    mkdir -p /storage/cloud
    mkdir -p /storage/media/$u

    # то, что видит само приложение
    mkdir -p /data/storage/el1/bundle

    # Владельцы. Номера из base/startup/init/services/etc/passwd:
    # account 3058, installs 3060, foundation 5523, группа system 1000.
    chown -R 3058:3058 /data/service/el1/public/account
    chown -R 5523:5523 /data/service/el1/public/bms /data/bundlemgr
    chmod -R 775       /data/service/el1/public/bms
    chown -R 5523:5523 /data/app/el1/bundle
    chmod 711          /data/app/el1/bundle/public

    mkdir -p /data/service/el1/startup/appspawn
    mkdir -p /data/service/el1/startup/log
    chmod 700 /data/service/el1/startup/appspawn
    chmod 755 /data/service/el1/startup/log

    chmod 666 /dev/unix/socket/AppSpawn 2>/dev/null

    mkdir -p /data/service/el0/render_service
    chmod 777 /data/service/el0/render_service
    chmod 666 /dev/dri/card* /dev/dri/renderD* 2>/dev/null

    echo '/tmp/core.%e.%p' > /proc/sys/kernel/core_pattern
    ulimit -c unlimited

    ln -sf libdisplay_buffer_vdi_impl_default.z.so /vendor/lib64/libdisplay_buffer_vdi_impl.z.so

    # Права для эмуляции и чтения устройств ввода MMI
    mkdir -p /dev/char /dev/v4l
    mkdir -p /data/service/el1/public/multimodalinput
    mkdir -p /data/service/el1/public/udev
    chmod 755 /data/service/el1/public/multimodalinput /data/service/el1/public/udev
    chmod 666 /dev/input/* /dev/uinput 2>/dev/null
    mkdir -p "$LIB"/module/{multimedia,account,multimodalinput}
    mkdir -p "$LIB"/module/{bundle,data,resourceschedule}
}


# ---------------------------------------------------------------------------
# Разложить собранное по системным каталогам.
# Установщик читает описания установки из сборки, но не знает про наши
# собственные цели и про готовые библиотеки из prebuilts — их копируем сами.
# ---------------------------------------------------------------------------

# copy_out <каталог назначения> <имя файла> [имя файла ...]
# Ищет файл в дереве сборки (мимо промежуточных и неочищенных копий)
# и копирует в указанный каталог.
copy_out() {
    local dst=$1; shift
    local name p
    mkdir -p "$dst"
    for name in "$@"; do
        p=$(find "$OUT" -name "$name" -type f \
                 -not -path "*/obj/*" -not -path "*unstripped*" \
                 -not -path "*/clang_x64/*" -not -path "*/ohos_clang_x86_64/*" \
                 2>/dev/null | head -1)
        if [ -n "$p" ]; then
            cp -f "$p" "$dst/" && echo "  $name"
        else
            echo "  не найдено: $name"
        fi
    done
}

install_all() {
    [ -d "$OUT" ] || { echo "нет каталога сборки: $OUT"; exit 1; }

    echo "собранное из $OUT"
    "$SCRIPT_DIR/install-modules.py" "$OUT" / || exit 1

    echo "локальные цели arkvm"
    local t p
    for t in sa_main samgr_client arkvm_param_service arkvm_token_init; do
        p=$(find "$OUT" -name "$t" -type f -perm -111 \
                 -not -path "*/obj/*" -not -path "*unstripped*" \
                 -not -path "*/clang_x64/*" | head -1)
        if [ -n "$p" ]; then
            cp -f "$p" "$BIN/$t" && chmod 755 "$BIN/$t" && echo "  $t"
        else
            echo "  не найдено: $t"
        fi
    done

    echo "готовые библиотеки"
    cp -f "$TREE"/prebuilts/rustc/linux-x86_64/current/lib/rustlib/x86_64-unknown-linux-ohos/lib/libstd.dylib.so \
          "$LIB"/ 2>/dev/null && echo "  libstd.dylib.so"
    cp -fP "$TREE"/prebuilts/clang/ohos/linux-x86_64/llvm/lib/x86_64-linux-ohos/*.so* \
          "$LIB"/ 2>/dev/null && echo "  рантаймы clang"

    echo "конфигурация HDF"
    if [ -f "$SCRIPT_DIR/hdf/hdf_default.hcb" ]; then
        mkdir -p /vendor/etc/hdfconfig
        cp -f "$SCRIPT_DIR/hdf/hdf_default.hcb" /vendor/etc/hdfconfig/
        chmod 644 /vendor/etc/hdfconfig/hdf_default.hcb
        echo "  hdf_default.hcb"
    else
        echo "  нет hdf_default.hcb (соберите hc-gen)"
    fi

    echo "эталонные реализации HDI дисплея"
    mkdir -p /vendor/lib64
    cp -f "$OUT"/hdf/drivers_peripheral_display/libdisplay_composer_vdi_impl_default.z.so \
          "$OUT"/hdf/drivers_peripheral_display/libdisplay_buffer_vdi_impl_default.z.so \
          /vendor/lib64/ 2>/dev/null && chmod 644 /vendor/lib64/*vdi_impl_default*
    # Имя без приставки "_default" — место для реализации производителя.
    # Своей у нас нет, подставляем эталонную.
    ln -sf libdisplay_buffer_vdi_impl_default.z.so   /vendor/lib64/libdisplay_buffer_vdi_impl.z.so
    ln -sf libdisplay_composer_vdi_impl_default.z.so /vendor/lib64/libdisplay_composer_vdi_impl.z.so
    echo "  связи vdi_impl"

    echo "mesa"
    local M="$OUT/thirdparty/mesa3d/lib"
    cp -a "$M"/libEGL_mesa.so*      "$LIB"/ 2>/dev/null
    cp -a "$M"/libGLESv1_CM.so*     "$LIB"/ 2>/dev/null
    cp -a "$M"/libGLESv2.so*        "$LIB"/ 2>/dev/null
    cp -a "$M"/libvulkan_intel.so   "$LIB"/ 2>/dev/null
    cp -a "$OUT"/resourceschedule/qos_manager/libqos.so "$LIB"/ 2>/dev/null
    # Gallium-драйвер (zink) грузится по пути, зашитому при сборке.
    mkdir -p "$LIB"/dri
    cp -a "$M"/dri/*.so             "$LIB"/dri/ 2>/dev/null || \
    cp -a "$M"/libgallium*.so*      "$LIB"/dri/ 2>/dev/null
    # Загрузчик Vulkan.
    cp -a "$OUT"/thirdparty/vulkan-loader/libvulkan.so* "$LIB"/ 2>/dev/null
    # Описание драйвера для загрузчика.
    mkdir -p /system/etc/vulkan/icd.d
    cat > /system/etc/vulkan/icd.d/intel_icd.x86_64.json <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/system/lib64/libvulkan_intel.so",
        "api_version": "1.3.302"
    }
}
EOF

    echo "надстройки декодирования изображений"
    mkdir -p "$LIB"/multimediaplugin/image
    cp -f "$OUT"/multimedia/image_framework/lib*plugin.z.so "$LIB"/multimediaplugin/image/ 2>/dev/null
    chmod 644 "$LIB"/multimediaplugin/image/* 2>/dev/null

    # -----------------------------------------------------------------------
    # Модули NAPI. Загрузчик ищет их как lib<имя>.z.so, затем lib<имя>_napi.z.so
    # в /system/lib64/module; точка в имени модуля означает подкаталог.
    # -----------------------------------------------------------------------
    echo "модули NAPI: ArkUI"
    local SRC="$OUT/arkui/ace_engine"
    mkdir -p "$LIB"/module/{arkui,graphics,file,util,app/ability}
    for m in componentutils componentsnapshot dragcontroller focuscontroller \
             smartgesturecontroller inspector observer performancemonitor \
             textmenucontroller uimaterial containerutils colorsampler; do
        [ -f "$SRC/lib$m.z.so" ] && cp -f "$SRC/lib$m.z.so" "$LIB"/module/arkui/
    done
    [ -f "$SRC/libdisplaysync.z.so" ] && cp -f "$SRC/libdisplaysync.z.so" "$LIB"/module/graphics/
    for m in configuration device font grid measure mediaquery overlay \
             prompt promptaction router animator atomicservicebar luminancesampler; do
        [ -f "$SRC/lib$m.z.so" ] && cp -f "$SRC/lib$m.z.so" "$LIB"/module/
    done

    echo "модули NAPI: общие"
    copy_out "$LIB/module" \
        libprocess.z.so libhitracemeter_napi.z.so libsystemdatetime.z.so \
        libscenesessionmanager_napi.z.so libsystemparameter.z.so \
        libutil.z.so libjson.z.so libstream.z.so libcollections.z.so \
        libtaskpool.z.so libworker.z.so libutils.z.so \
        libtimer.z.so libconsole.z.so libdfx.z.so \
        liburi.z.so liburl.z.so libbuffer.z.so libxml.z.so libconvertxml.z.so \
        libfileio.z.so

    echo "модули NAPI: файлы"
    copy_out "$LIB/module/file" libfs.z.so libfileuri.z.so

    echo "модули NAPI: контейнеры"
    copy_out "$LIB/module/util" libarraylist.z.so libhashmap.z.so libhashset.z.so

    echo "модули NAPI: расширения способностей"
    copy_out "$LIB/module/app/ability" \
        libserviceextensionability.z.so libserviceextensionability_napi.z.so

    echo "породители расширений"
    copy_out "$LIB/extensionability" \
        libservice_extension_module.z.so libui_extension_module.z.so \
        libform_extension_module.z.so

    chmod 644 "$LIB"/module/*.so "$LIB"/module/*/*.so 2>/dev/null
    chmod 644 "$LIB"/module/app/ability/*.so "$LIB"/extensionability/*.so 2>/dev/null

    # -----------------------------------------------------------------------
    # Системные ресурсы, службы и рабочий стол
    # -----------------------------------------------------------------------
    echo "системные ресурсы"
    local RES="$OUT/obj/base/global/system_resources/systemres/SystemResources.hap"
    if [ -f "$RES" ]; then
        mkdir -p /system/app/ohos.global.systemres /system/app/SystemResources /data/global/systemResources
        cp -f "$RES" /system/app/ohos.global.systemres/
        cp -f "$RES" /system/app/SystemResources/
        cp -f "$RES" /data/global/systemResources/
        chmod 644 /system/app/ohos.global.systemres/SystemResources.hap \
                  /system/app/SystemResources/SystemResources.hap \
                  /data/global/systemResources/SystemResources.hap
        echo "  SystemResources.hap"
    else
        echo "  не найдено: SystemResources.hap"
    fi

    echo "служба наблюдения за параметрами"
    copy_out "$LIB" libparam_watcher.z.so
    cp -f "$TREE"/base/startup/init/services/param/watcher/sa_profile/param_watcher.json \
          /system/profile/param_watcher.json 2>/dev/null

    echo "служба способов ввода"
    copy_out "$LIB" libinputmethod_service.z.so
    cp -f "$TREE"/base/inputmethod/imf/profile/3703.json \
          /system/profile/inputmethod_service.json 2>/dev/null

    echo "распределённое хранилище"
    copy_out "$LIB" libdistributeddataservice.z.so
    cp -f "$TREE"/foundation/distributeddatamgr/datamgr_service/services/distributeddataservice/sa_profile/1301.json \
          /system/profile/distributeddata.json 2>/dev/null

    echo "оконная схема сцены"
    copy_out "$LIB" \
        libsms.z.so libscreen_session_manager.z.so libscene_session_manager.z.so \
        libsession_manager.z.so libsession_manager_lite.z.so libsession_manager_service.z.so

    echo "признак режима сцены"
    cp -f "$OUT"/obj/foundation/window/window_manager/etc/sceneboard.config /system/etc/ 2>/dev/null
    # Код читает файл по корневому пути /etc/sceneboard.config. На устройстве
    # /etc — ссылка на /system/etc, у нас это каталог хозяйской системы,
    # поэтому связываем явно.
    ln -sf /system/etc/sceneboard.config /etc/sceneboard.config
    echo "  $(cat /etc/sceneboard.config 2>/dev/null)"

    echo "рабочий стол"
    local SCB="$TREE/applications/standard/hap/sceneboard/SceneBoard.hap"
    if [ -s "$SCB" ] && [ "$(stat -c %s "$SCB")" -gt 100000 ]; then
        mkdir -p /system/app/SceneBoard
        cp -f "$SCB" /system/app/SceneBoard/
        chmod 644 /system/app/SceneBoard/SceneBoard.hap
        echo "  SceneBoard.hap"
    else
        echo "  SceneBoard.hap отсутствует или это заглушка git-lfs (нужен git lfs pull)"
    fi




    echo "модули NAPI: общие"
    copy_out "$LIB/module" \
        libdeviceinfo.z.so libsystemparameterenhance.z.so \
        libi18n.z.so libhitracechain_napi.z.so \
        libsettings.z.so libcommoneventmanager.z.so libscreenlock.z.so\
        libcommonevent.z.so libhisysevent_napi.z.so\
        libconfigpolicy.z.so libeffectkit.z.so libwallpaper.z.so

    echo "модули NAPI: звук, учётные записи, ввод"
    copy_out "$LIB/module/multimedia"      libaudio.z.so libimage_napi.z.so
    copy_out "$LIB/module/account"         libosaccount.z.so
    copy_out "$LIB/module/multimodalinput" libinputmonitor.z.so libkeycode.z.so libkeyevent.z.so

    echo "модули NAPI: контейнеры"
    copy_out "$LIB/module/util" \
        libarraylist.z.so libdeque.z.so libqueue.z.so libvector.z.so \
        liblinkedlist.z.so liblist.z.so libstack.z.so libstruct.z.so \
        libtreemap.z.so libtreeset.z.so libhashmap.z.so libhashset.z.so \
        liblightweightmap.z.so liblightweightset.z.so libplainarray.z.so


    #copy_out "$LIB/module/bundle"          libbundlemanager.z.so
    #copy_out "$LIB/module/data"            libpreferences.z.so
    #copy_out "$LIB/module/multimedia"      libimage.z.so
    copy_out "$LIB/module/resourceschedule" libworkscheduler.z.so
    copy_out "$LIB/module/bundle" libbundlemanager.z.so libbundleresourcemanager.z.so
    copy_out "$LIB/module/data"   libpreferences.z.so librelationalstore.z.so
    echo "готово"
}


# ---------------------------------------------------------------------------
# Останов и сброс состояния
# ---------------------------------------------------------------------------
stop_all() {
    pkill -f "$BIN/" 2>/dev/null

    for p in accesstoken_service installs storage_manager accountmgr foundation bms \
             appspawn composer_host allocator_host param_watcher inputmethod_service \
             distributeddata com.ohos.sceneboard; do
        pkill -f "^$p" 2>/dev/null
    done

    sleep 1
    pkill -9 -f "$BIN/" 2>/dev/null
    rm -f /dev/unix/socket/hilog*
    sed -i 's/"isVerified":true/"isVerified":false/' \
        /data/service/el1/public/account/100/account_info.json 2>/dev/null
    rm -f "$TOKEN_BYPID"/*

    systemctl stop ohos-composer_host ohos-allocator_host 2>/dev/null
    systemctl stop ohos-render_service 2>/dev/null

    pkill -f multimodalinput
    pkill -f sa_main
}

# Стирать после любого изменения списка процессов или прав в token_init.cpp.
# Вместе с базой пропадают маркеры установленных приложений — их придётся
# выдать заново переустановкой пакетов.
reset_tokens() {
    rm -f  /data/service/el1/public/access_token/*.db*
    rm -f  /data/service/el0/access_token/nativetoken.json*
    rm -rf "$TOKEN_BYNAME"
}

# Полный сброс. Учтите: учётная запись 100 создастся заново уже после
# старта BMS, поэтому после reset нужен второй запуск.
reset_state() {
    reset_tokens
    rm -rf /data/service/el1/public/account
    rm -rf /dev/__parameters__
}


# ---------------------------------------------------------------------------
# Маркеры доступа
#
# Ядро их не хранит, поэтому роль маркера играет файл с именем по номеру
# процесса. Список «имя процесса → маркер» готовит arkvm_token_init в каталоге
# byname. Номер процесса известен только после запуска, поэтому службы
# запускаются через короткую обёртку: она от корня записывает файл по своему
# номеру и подменяет себя на службу. Номер процесса при замене сохраняется,
# и служба с первого мгновения читает готовый маркер.
# ---------------------------------------------------------------------------
set_token() {           # set_token <имя процесса> — запасной вариант, после старта
    local name=$1 tok pid
    tok=$(cat "$TOKEN_BYNAME/$name" 2>/dev/null) || return 0
    [ -n "$tok" ] || return 0
    for pid in $(pgrep -x "$name"); do
        printf '%s' "$tok" > "$TOKEN_BYPID/$pid"
        chmod 644 "$TOKEN_BYPID/$pid"
    done
}

# Тело обёртки. $1 — имя службы, дальше команда с аргументами.
TOKEN_WRAPPER='
    tok=$(cat "/data/service/el0/access_token/byname/$1" 2>/dev/null)
    if [ -n "$tok" ]; then
        printf "%s" "$tok" > "/data/service/el0/access_token/bypid/$$"
        chmod 644 "/data/service/el0/access_token/bypid/$$" 2>/dev/null
    fi
    shift
    exec "$@"
'


# ---------------------------------------------------------------------------
# Ожидания и запуск
# ---------------------------------------------------------------------------
wait_samgr() {
    for _ in $(seq 1 50); do
        "$SAMGR_CLIENT" 2>/dev/null | grep -q "samgr ok" && return 0
        sleep 0.2
    done
    return 1
}

wait_sa() {
    for _ in $(seq 1 150); do
        "$SAMGR_CLIENT" 2>/dev/null | grep -qx "  $1" && return 0
        sleep 0.2
    done
    return 1
}

start_bg() {
    local name=$1; shift
    "$@" > "$LOGDIR/$name.log" 2>&1 &
    echo "  $name pid $!"
}

# hdf_devhost выставляет PR_SET_PDEATHSIG и умирает вместе с родителем.
# На устройстве его держит init; у нас — systemd.
start_hdf() {
    local name=$1; shift
    systemctl reset-failed "ohos-$name" 2>/dev/null
    systemctl stop "ohos-$name" 2>/dev/null
    systemd-run --unit="ohos-$name" --quiet \
        --setenv=LD_LIBRARY_PATH="$LD_LIBRARY_PATH" "$@" \
        && echo "  $name" || echo "  $name не запустился"
}

start_sa() {
    local name=$1 id=$2
    echo "$name ($id)"
    /bin/bash -c "$TOKEN_WRAPPER" _ "$name" "$SA_MAIN" "/system/profile/$name.json" \
        > "$LOGDIR/$name.log" 2>&1 &
    echo "  pid $!"
    wait_sa "$id" || { echo "  не поднялся, см. $LOGDIR/$name.log"; exit 1; }
}

# Запуск сервиса под своим пользователем, группами и возможностями —
# то, что на устройстве делает init по *.cfg.
start_sa_as() {
    local name=$1 id=$2 uid=$3 groups=${4:-} caps=${5:-}
    echo "$name ($id) uid=$uid"
    local opts=(--reuid "$uid" --regid "$uid")
    if [ -n "$groups" ]; then
        opts+=(--groups "$groups")
    else
        opts+=(--clear-groups)
    fi
    if [ -n "$caps" ]; then
        local signed="+${caps//,/,+}"
        opts+=(--inh-caps "$signed" --ambient-caps "$signed")
    fi
    # Обёртка работает от корня: кладёт маркер, затем подменяется на setpriv,
    # а тот — на sa_main. Номер процесса один на все замены.
    /bin/bash -c "$TOKEN_WRAPPER" _ "$name" setpriv "${opts[@]}" \
        "$SA_MAIN" "/system/profile/$name.json" \
        > "$LOGDIR/$name.log" 2>&1 &
    echo "  pid $!"
    wait_sa "$id" || { echo "  не поднялся, см. $LOGDIR/$name.log"; exit 1; }
}


# ---------------------------------------------------------------------------
case "${1:-start}" in
stop)   stop_all; echo "остановлено"; exit 0 ;;
status) "$SAMGR_CLIENT" 2>/dev/null; exit 0 ;;
tokens) stop_all; reset_tokens
        echo "токены стёрты; приложения надо переустановить (bm install -p ...)"
        exit 0 ;;
reset)  stop_all; reset_state;  echo "состояние стёрто"; exit 0 ;;
install) [ "$(id -u)" = 0 ] || { echo "нужен root"; exit 1; }
         install_all; exit 0 ;;
esac

[ "$(id -u)" = 0 ] || { echo "нужен root"; exit 1; }

ensure_dirs
stop_all

sysctl -w kernel.pid_max=32768 >/dev/null


echo "параметры"
start_bg param "$BIN/arkvm_param_service"
for _ in $(seq 1 25); do
    [ -d /dev/__parameters__ ] && break
    sleep 0.2
done

echo "токены"
"$BIN/arkvm_token_init"

echo "hilogd"
start_bg hilogd "$BIN/hilogd"
for _ in $(seq 1 25); do
    [ -S /dev/unix/socket/hilogInput ] && break
    sleep 0.2
done
# К журналу пишут и процессы под чужими uid.
chmod 666 /dev/unix/socket/hilog* 2>/dev/null
# 16 МБ — верхний предел; при холостом шуме это около часа записи,
# чего хватает, чтобы снять момент загрузки уже после её окончания.
"$BIN/hilog" -G 16M >/dev/null 2>&1

echo "samgr"
start_bg samgr "$BIN/samgr"
wait_samgr || { echo "  samgr не отвечает, см. $LOGDIR/samgr.log"; exit 1; }

start_sa    accesstoken_service 3503
start_sa_as installs 511 3060 1000 chown,dac_override,fowner,sys_admin

echo "storage_daemon"
start_bg storage_daemon "$BIN/storage_daemon"
sleep 1

start_sa    storage_manager 5003

# Распределённое хранилище поднимаем до менеджера учётных записей: тот при
# первой же активации записи пишет её состояние через это хранилище и без
# него встаёт в вечные повторы, так и не сообщив менеджеру способностей
# о запуске пользователя.
start_sa    distributeddata 1301

start_sa_as accountmgr 200 3058 1000

start_sa    multimodalinput 3101
sleep 1
set_token multimodalinput

#echo "appspawn"
#start_bg appspawn "$BIN/appspawn" -mode appspawn \
#    --process-name com.ohos.appspawn.startup --start-flags daemon --type standard \
#    --sandbox-switch on --bundle-name com.ohos.appspawn.startup --app-operate-type operate \
#    --render-command command --app-launch-type singleton --app-visible true
#sleep 4


( cd / && exec "$BIN/appspawn" -mode appspawn \
    --process-name com.ohos.appspawn.startup --start-flags daemon --type standard \
    --sandbox-switch on --bundle-name com.ohos.appspawn.startup --app-operate-type operate \
    --render-command command --app-launch-type singleton --app-visible true ) \
    > "$LOGDIR/appspawn.log" 2>&1 &
echo "  appspawn pid $!"
sleep 4


echo "hdf_devmgr"
start_bg hdf_devmgr "$BIN/hdf_devmgr"
wait_sa 5100 || { echo "  не поднялся, см. $LOGDIR/hdf_devmgr.log"; exit 1; }

# Контейнеры драйверов. На устройстве их запускает init по hdf_devhost.cfg.
echo "драйверы дисплея"
start_hdf allocator_host /vendor/bin/hdf_devhost -i 1 -n allocator_host
start_hdf composer_host  /vendor/bin/hdf_devhost -i 0 -n composer_host
sleep 2

# Render service обязан подняться раньше оконного менеджера: тот при старте
# спрашивает у него список экранов. Сам он требует работающего композитора.
echo "render_service"
systemctl reset-failed ohos-render_service 2>/dev/null
systemctl stop ohos-render_service 2>/dev/null
systemd-run --unit=ohos-render_service --quiet \
    -p LimitCORE=infinity \
    --setenv=LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    --setenv=WSI_OHOS_DEBUG=1 \
    --setenv=MESA_VK_WSI_DEBUG=buffer \
    --setenv=ZINK_DEBUG=flushsync \
    --setenv=WSI_OHOS_COPY=0 \
    "$BIN/render_service"
wait_sa 10 || { echo "  render_service не поднялся"; exit 1; }

# foundation держит менеджеры способностей, приложений, пакетов, а в режиме
# сцены ещё и службу экранов (4607) с посредником сеансов (4606).
start_sa_as foundation 180 5523 1000
sleep 1
set_token foundation

chmod 666 /dev/unix/socket/AppSpawn 2>/dev/null

# Служба параметров нужна приложениям для подписки на системные параметры:
# без неё каждое обращение стоит секунды ожидания на главном потоке.
start_sa param_watcher 3901

# Служба способов ввода: текстовые поля запрашивают у неё сеанс ввода.
start_sa inputmethod_service 3703

echo
echo "готово, реестр:"
"$SAMGR_CLIENT" 2>/dev/null
echo
echo "журнал:  sudo env LD_LIBRARY_PATH=$LIB $BIN/hilog"
