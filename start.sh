#!/usr/bin/env bash
# Запуск системного слоя arkVM в правильном порядке.
#   sudo ./start.sh          — поднять всё
#   sudo ./start.sh stop     — погасить
#   sudo ./start.sh status   — что зарегистрировано в samgr
#   sudo ./start.sh install  — разложить собранное по системным каталогам
#   sudo ./start.sh tokens   — стереть удостоверения и состояние пакетов
#   sudo ./start.sh reset    — стереть всё изменяемое состояние
#
# ВНИМАНИЕ: tokens и reset стирают хранилище удостоверений вместе с
# удостоверениями установленных приложений, поэтому заодно стирается и
# состояние распорядителя пакетов — иначе он сочтёт пакеты установленными,
# а их удостоверений уже не будет, и оболочка не запустится вовсе.
# Первый запуск после сброса заметно дольше: пакеты ставятся заново.

set -u

BIN=${BIN:-/system/bin}
LIB=${LIB:-/system/lib64}
VLIB=${VLIB:-/vendor/lib64}
LOGDIR=${LOGDIR:-/tmp/arkvm}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="$(dirname "$SCRIPT_DIR")"
OUT=${OUT:-$TREE/out/arkvm}

export LD_LIBRARY_PATH=/system/lib64:/system/lib64/platformsdk:/system/lib64/chipset-sdk:/system/lib64/chipset-sdk-sp:/system/lib64/ndk:/vendor/lib64:/vendor/lib64/passthrough

SAMGR_CLIENT=$BIN/samgr_client
SA_MAIN=$BIN/sa_main

TOKEN_BYNAME=/data/service/el0/access_token/byname
TOKEN_BYPID=/data/service/el0/access_token/bypid

# Владельцы из base/startup/init/services/etc/passwd:
UID_ACCOUNT=3058      # менеджер учётных записей
UID_INSTALLS=3060     # установщик пакетов
UID_FOUNDATION=5523   # foundation, в нём же распорядитель пакетов
UID_DDATA=3012        # служба распределённых данных

# ---------------------------------------------------------------------------
# Каталоги, которые на устройстве создаёт init по описаниям в *.cfg.
# ---------------------------------------------------------------------------
ensure_dirs() {
    local u=100 lvl

    mkdir -p "$LOGDIR"
    mkdir -p /dev/unix/socket
    mkdir -p /system/profile
    mkdir -p /system/etc/{app,account,storage_daemon,sandbox}
    mkdir -p /system/etc/param/ohos_const

    # хранилище удостоверений
    mkdir -p "$TOKEN_BYNAME" "$TOKEN_BYPID"
    mkdir -p /data/service/el1/public/access_token
    chmod 777 "$TOKEN_BYPID"

    # распорядитель пакетов
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

    fix_owners

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

    ln -sf libdisplay_buffer_vdi_impl_default.z.so "$VLIB"/libdisplay_buffer_vdi_impl.z.so

    # Права для эмуляции и чтения устройств ввода MMI
    mkdir -p /dev/char /dev/v4l
    mkdir -p /data/service/el1/public/multimodalinput
    mkdir -p /data/service/el1/public/udev
    chmod 755 /data/service/el1/public/multimodalinput /data/service/el1/public/udev
    chmod 666 /dev/input/* /dev/uinput 2>/dev/null

    mkdir -p "$LIB"/module/{multimedia,account,multimodalinput}
    mkdir -p "$LIB"/module/{bundle,data,resourceschedule,events,application,telephony,file}
    mkdir -p "$LIB"/module/app/form

    # Код ArkUI читает наборы по корневому пути /etc/abc, а на устройстве
    # /etc — ссылка на /system/etc; у нас это каталог хозяйской системы.
    ln -sfn /system/etc/abc /etc/abc
    ln -sfn /system/etc/audio /etc/audio

    mkdir -p /data/local/tmp
    chmod 777 /data/local/tmp

    # справочник службы распределённых данных (на устройстве это делает init)
    mkdir -p /data/service/el1/public/database/distributeddata/meta/backup
    mkdir -p /data/service/el1/public/database/distributeddata/kvdb
    mkdir -p /data/service/el1/public/database/distributeddata/rdb
    chown -R $UID_DDATA:$UID_DDATA /data/service/el1/public/database/distributeddata
    chmod -R 2770 /data/service/el1/public/database/distributeddata

    cp -f "$OUT"/obj/base/startup/init/services/etc/ohos.para/ohos.para \
          /system/etc/param/ohos.para

    mkdir -p /data/service/el1/public/bluetooth
    chmod 770 /data/service/el1/public/bluetooth
}

# Владельцы и права каталогов, которые проверяет installd. Вынесено отдельно:
# после сброса состояния их надо восстанавливать, иначе распорядитель пакетов
# ругается «mode not same» и «uid or gid are not same».
fix_owners() {
    chown -R $UID_ACCOUNT:$UID_ACCOUNT /data/service/el1/public/account
    chown -R $UID_FOUNDATION:$UID_FOUNDATION /data/service/el1/public/bms /data/bundlemgr
    chmod 0755 /data/service/el1/public/bms/bundle_manager_service
    chown -R $UID_FOUNDATION:$UID_FOUNDATION /data/app/el1/bundle
    chmod 711 /data/app/el1/bundle/public
}

# ---------------------------------------------------------------------------
# Разложить собранное по системным каталогам.
# Установщик читает описания установки из сборки, но не знает про наши
# собственные цели и про готовые библиотеки из prebuilts — их копируем сами.
# ---------------------------------------------------------------------------

# find_out <имя файла> — найти в дереве сборки готовый файл, минуя
# промежуточные, неочищенные и хозяйские сборки.
find_out() {
    find "$OUT" -name "$1" -type f \
         -not -path "*/obj/*" -not -path "*unstripped*" \
         -not -path "*/clang_x64/*" -not -path "*/ohos_clang_x86_64/*" \
         2>/dev/null | head -1
}

# copy_out <каталог назначения> <имя файла> [имя файла ...]
copy_out() {
    local dst=$1; shift
    local name p
    mkdir -p "$dst"
    for name in "$@"; do
        p=$(find_out "$name")
        if [ -n "$p" ]; then
            install -m 644 "$p" "$dst/" && echo "  $name"
        else
            echo "  не найдено: $name"
        fi
    done
}

# install_hap <каталог> <исходный файл> <итоговое имя>
# Каталог очищается: распорядитель пакетов отказывается разбирать каталог,
# в котором лежит больше одного основного пакета («more than one entry hap»).
install_hap() {
    local dir=$1 src=$2 name=$3
    if [ ! -s "$src" ] || [ "$(stat -c %s "$src")" -lt 100000 ]; then
        echo "  нет или заглушка: $src"
        return 1
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    install -m 644 "$src" "$dir/$name" && echo "  $name"
}

install_fonts() {
    echo "шрифты"
    local f src
    mkdir -p /system/fonts
    grep -oE '"[A-Za-z0-9 _-]+\.tt[fc]"' /system/etc/fontconfig.json 2>/dev/null |
    tr -d '"' | sort -u | while read -r f; do
        [ -f "/system/fonts/$f" ] && continue
        src=$(find "$TREE/third_party" "$TREE/base" -name "$f" \
                   -not -path "*/test/*" -not -path "*/out/*" 2>/dev/null | head -1)
        [ -n "$src" ] || continue
        # В дереве часть начертаний — указатели git-lfs по сотне байт.
        [ "$(stat -c %s "$src")" -lt 10000 ] && continue
        install -m 644 "$src" /system/fonts/ && echo "  $f"
    done
    # Убираем пустышки, оставшиеся от прежних заходов: подбор начертаний
    # на них спотыкается.
    find /system/fonts -size -10k -delete 2>/dev/null
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
            install -m 755 "$p" "$BIN/$t" && echo "  $t"
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
        install -m 644 "$SCRIPT_DIR/hdf/hdf_default.hcb" /vendor/etc/hdfconfig/
        echo "  hdf_default.hcb"
    else
        echo "  нет hdf_default.hcb (соберите hc-gen)"
    fi

    echo "эталонные реализации HDI дисплея"
    mkdir -p "$VLIB"
    cp -f "$OUT"/hdf/drivers_peripheral_display/libdisplay_composer_vdi_impl_default.z.so \
          "$OUT"/hdf/drivers_peripheral_display/libdisplay_buffer_vdi_impl_default.z.so \
          "$VLIB"/ 2>/dev/null && chmod 644 "$VLIB"/*vdi_impl_default*
    # Имя без приставки "_default" — место для реализации производителя.
    # Своей у нас нет, подставляем эталонную.
    ln -sf libdisplay_buffer_vdi_impl_default.z.so   "$VLIB"/libdisplay_buffer_vdi_impl.z.so
    ln -sf libdisplay_composer_vdi_impl_default.z.so "$VLIB"/libdisplay_composer_vdi_impl.z.so
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
    local SRC="$OUT/arkui/ace_engine" m
    mkdir -p "$LIB"/module/{arkui,graphics,file,util,app/ability}
    for m in componentutils componentsnapshot dragcontroller focuscontroller \
             smartgesturecontroller inspector observer performancemonitor \
             textmenucontroller uimaterial containerutils colorsampler; do
        [ -f "$SRC/lib$m.z.so" ] && install -m 644 "$SRC/lib$m.z.so" "$LIB"/module/arkui/
    done
    [ -f "$SRC/libdisplaysync.z.so" ] && install -m 644 "$SRC/libdisplaysync.z.so" "$LIB"/module/graphics/
    for m in configuration device font grid measure mediaquery overlay \
             prompt promptaction router animator atomicservicebar luminancesampler; do
        [ -f "$SRC/lib$m.z.so" ] && install -m 644 "$SRC/lib$m.z.so" "$LIB"/module/
    done

    echo "модули NAPI: общие"
    copy_out "$LIB/module" \
        libprocess.z.so libhitracemeter_napi.z.so libsystemdatetime.z.so \
        libscenesessionmanager_napi.z.so libsystemparameter.z.so \
        libsystemparameterenhance.z.so libdeviceinfo.z.so \
        libutil.z.so libjson.z.so libstream.z.so libcollections.z.so \
        libtaskpool.z.so libworker.z.so libutils.z.so \
        libtimer.z.so libconsole.z.so libdfx.z.so \
        liburi.z.so liburl.z.so libbuffer.z.so libxml.z.so libconvertxml.z.so \
        libfileio.z.so \
        libconfigpolicy.z.so libeffectkit.z.so libwallpaper.z.so \
        libbatteryinfo.z.so libinputmethod.z.so libintl.z.so libi18n.z.so \
        libnotificationsubscribe.z.so libnotificationmanager.z.so \
        libsystemtimer.z.so libvibrator.z.so libhgmnapi.z.so \
        libhitracechain_napi.z.so libhisysevent_napi.z.so \
        libsettings.z.so libscreenlock.z.so \
        libcommoneventmanager.z.so libcommonevent.z.so \
        libsessionmanagerservice_napi.z.so libtransactionmanager_napi.z.so \
        libpower.z.so libthermal.z.so libbundle.z.so

    echo "модули NAPI: файлы"
    copy_out "$LIB/module/file" libfs.z.so libfileuri.z.so

    echo "модули NAPI: контейнеры"
    copy_out "$LIB/module/util" \
        libarraylist.z.so libdeque.z.so libqueue.z.so libvector.z.so \
        liblinkedlist.z.so liblist.z.so libstack.z.so libstruct.z.so \
        libtreemap.z.so libtreeset.z.so libhashmap.z.so libhashset.z.so \
        liblightweightmap.z.so liblightweightset.z.so libplainarray.z.so

    echo "модули NAPI: звук, ввод, события, отрисовка"
    copy_out "$LIB/module/multimedia"      libaudio.z.so
    copy_out "$LIB/module/multimodalinput" libinputmonitor.z.so libkeycode.z.so libkeyevent.z.so
    copy_out "$LIB/module/events"          libemitter.z.so
    copy_out "$LIB/module/graphics"        libdisplaysync.z.so libdrawing_napi.z.so

    echo "модули NAPI: пакеты, данные, карточки, расписание"
    copy_out "$LIB/module/bundle" \
        libbundlemanager.z.so libbundleresourcemanager.z.so \
        libbundlemonitor.z.so libinstaller.z.so \
        liblauncherbundlemanager.z.so libshortcutmanager.z.so
    copy_out "$LIB/module/data" \
        libpreferences.z.so librelationalstore.z.so libuniformtypedescriptor_napi.z.so
    copy_out "$LIB/module/app/form"        libformhost.z.so libforminfo.z.so
    copy_out "$LIB/module/resourceschedule" libworkscheduler.z.so

    echo "модули NAPI: расширения способностей"
    copy_out "$LIB/module/app/ability" \
        libserviceextensionability.z.so libserviceextensionability_napi.z.so

    echo "породители расширений"
    copy_out "$LIB/extensionability" \
        libservice_extension_module.z.so libui_extension_module.z.so \
        libform_extension_module.z.so

    # подчищаем неверно разложенные копии прошлых заходов
    rm -f "$LIB"/module/multimedia/libimage_napi.z.so

    chmod 644 "$LIB"/module/*.so "$LIB"/module/*/*.so 2>/dev/null
    chmod 644 "$LIB"/module/app/ability/*.so "$LIB"/extensionability/*.so 2>/dev/null

    # -----------------------------------------------------------------------
    # Службы
    # -----------------------------------------------------------------------
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
    mkdir -p /system/etc/distributeddata/conf
    cp -f "$TREE"/foundation/distributeddatamgr/datamgr_service/conf/config.json \
          /system/etc/distributeddata/conf/ && echo "  config.json"

    echo "оконная схема сцены"
    copy_out "$LIB" \
        libsms.z.so libscreen_session_manager.z.so libscene_session_manager.z.so \
        libsession_manager.z.so libsession_manager_lite.z.so libsession_manager_service.z.so

    echo "менеджер пакетов"
    copy_out "$LIB" libbms.z.so

    echo "служба блокировки экрана"
    copy_out "$LIB" libscreenlock_server.z.so
    cp -f "$TREE"/base/theme/screenlock_mgr/sa_profile/3704.json \
          /system/profile/screenlock_server.json 2>/dev/null

    echo "служба проверки подлинности"
    copy_out "$LIB" libuserauthservice.z.so
    python3 - "$TREE" <<'EOF'
import json, sys
tree = sys.argv[1]
sa = []
for n in (901, 921, 931):
    with open(f"{tree}/base/useriam/user_auth_framework/sa_profile/default/{n}.json") as f:
        sa += json.load(f)["systemability"]
json.dump({"process": "useriam", "systemability": sa},
          open("/system/profile/useriam.json", "w"), indent=4)
EOF

    echo "служба управления питанием"
    copy_out "$LIB" libpowermgrservice.z.so libdisplaymgrservice.z.so
    copy_out "$BIN" power-shell
    # 3301 (питание) и 3308 (состояние экрана) живут в одном процессе.
    python3 - "$TREE" <<'EOF'
import json, sys
tree = sys.argv[1]
sa = []
for p in (f"{tree}/base/powermgr/power_manager/sa_profile/3301.json",
          f"{tree}/base/powermgr/display_manager/state_manager/sa_profile/3308.json"):
    with open(p) as f:
        sa += json.load(f)["systemability"]
json.dump({"process": "powermgr", "systemability": sa},
          open("/system/profile/powermgr.json", "w"), indent=4)
EOF

    echo "звук"
    copy_out "$LIB" libaudio_policy_service.z.so libaudio_service.z.so \
                    libaudio_proxy_6.1.z.so libeffect_proxy_1.0.z.so libdaudio_proxy_1.0.z.so
    # 3001 (вывод) и 3009 (правила) тоже в одном процессе.
    python3 - "$TREE" <<'EOF'
import json, sys
tree = sys.argv[1]
sa = []
for p in ("pulseaudio.json", "audio_policy.json"):
    with open(f"{tree}/foundation/multimedia/audio_framework/sa_profile/{p}") as f:
        sa += json.load(f)["systemability"]
json.dump({"process": "audio_server", "systemability": sa},
          open("/system/profile/audio_server.json", "w"), indent=4)
EOF

    mkdir -p /system/etc/audio
    install -m 644 "$TREE"/foundation/multimedia/audio_framework/services/audio_policy/server/infra/config/file/*.xml \
            /system/etc/audio/

    mkdir -p /vendor/etc/audio /vendor/etc/hdfconfig /chip_prod/etc/hdfconfig /chip_prod/etc/audio
    local V="$TREE/vendor/ohemu/virt/hals/audio"
    install -m 644 "$TREE"/vendor/ohemu/virt/hals/audio/config/x86_64/*.xml /vendor/etc/audio/
    install -m 644 "$V"/audio_adapter.json "$V"/audio_paths.json \
                   "$V"/alsa_adapter.json "$V"/alsa_paths.json /vendor/etc/hdfconfig/
    install -m 644 "$V"/audio_effect.json /chip_prod/etc/hdfconfig/
    install -m 644 "$V"/config/audio_policy_config_new.xml \
                   /chip_prod/etc/audio/audio_policy_config.xml

    install_bluetooth

    # -----------------------------------------------------------------------
    # Настройки, признаки, пакеты
    # -----------------------------------------------------------------------
    echo "настройки оконной подсистемы"
    mkdir -p /system/etc/window/resources
    cp -f "$TREE"/foundation/window/window_manager/resources/config/other/window_manager_config.xml \
          "$TREE"/foundation/window/window_manager/resources/config/other/display_manager_config.xml \
          /system/etc/window/resources/ 2>/dev/null
    # Настольная настройка окон вместо заводской заглушки. Обрамление окна
    # (полоса с кнопками) рисует ArkUI, и в настольном наборе оно выключено —
    # включаем обратно, иначе у окон пропадают кнопки.
    install -m 644 "$TREE"/vendor/hihope/2in1_core_system/window_config/window_manager_config.xml \
            /system/etc/window/resources/window_manager_config.xml
    sed -i 's/<decor enable="false">/<decor enable="true">/' \
            /system/etc/window/resources/window_manager_config.xml
    echo "  window/display config"

    echo "признак режима сцены"
    cp -f "$OUT"/obj/foundation/window/window_manager/etc/sceneboard.config /system/etc/ 2>/dev/null
    # Код читает файл по корневому пути /etc/sceneboard.config.
    ln -sf /system/etc/sceneboard.config /etc/sceneboard.config
    echo "  $(cat /etc/sceneboard.config 2>/dev/null)"

    echo "наши системные параметры"
    cat > /system/etc/param/arkvm.para <<'EOF'
# свободные окна вместо телефонных
const.window.multiWindowUIType=FreeFormMultiWindow
# Распорядитель пакетов сверяет тип устройства с перечнем внутри пакета.
# Стенд у нас разом и настольный, и планшетный, и телефонный, поэтому
# перечисляем всё — иначе пакеты для иного типа не установятся.
const.bms.supportAppTypes=default,phone,tablet,2in1
const.global.language=en-Latn-US
const.global.locale=en-Latn-US
const.global.region=US
EOF
    chmod 644 /system/etc/param/arkvm.para

    echo "наши системные файлы"
    mkdir -p /system/etc/app /system/etc/sandbox
    cp -f "$SCRIPT_DIR"/etc/install_list.json \
          "$SCRIPT_DIR"/etc/install_list_capability.json \
          "$SCRIPT_DIR"/etc/install_list_permissions.json \
          /system/etc/app/ 2>/dev/null && echo "  списки предустановки"
    cp -f "$SCRIPT_DIR"/etc/appdata-sandbox.json /system/etc/sandbox/ \
          2>/dev/null && echo "  песочница приложений"

    install_fonts

    echo "системные ресурсы"
    local RES="$OUT/obj/base/global/system_resources/systemres/SystemResources.hap"
    install_hap /system/app/ohos.global.systemres "$RES" SystemResources.hap
    install_hap /system/app/SystemResources       "$RES" SystemResources.hap
    mkdir -p /data/global/systemResources
    [ -f "$RES" ] && install -m 644 "$RES" /data/global/systemResources/

    echo "рабочий стол"
    install_hap /system/app/SceneBoard \
        "$TREE/foundation/window/window_scene_board/product/pc/build/default/outputs/default/pc_sceneboard-default-signed.hap" \
        SceneBoard.hap

    echo "настройки"
    install_hap /system/app/Settings \
        "$TREE/applications/standard/hap/sceneboard/Settings.hap" Settings.hap
    install_hap /system/app/SettingsData \
        "$TREE/applications/standard/hap/SettingsData.hap" SettingsData.hap

    fix_owners
    echo "готово"
}

install_bluetooth() {
    echo "Bluetooth"
    local lib src
    # Служба и обе половины связи с драйвером. Без обёртки
    # libbluetooth_hci_proxy_1.0.z.so служба не стартует вовсе:
    # её описание требует наличия этого файла (min_hdi_proxy_version).
    for lib in libbluetooth_server.z.so libbluetooth_hci_proxy_1.0.z.so \
               libbluetooth_hci_stub_1.0.z.so; do
        src=$(find_out "$lib")
        if [ -n "$src" ]; then install -m 644 "$src" "$LIB"/ && echo "  $lib"
        else echo "  не найдено: $lib"; fi
    done

    # Слой драйверов, его загружаемая часть и наша подставка вместо
    # вендорской библиотеки. Кладём и в /vendor/lib64 (там их ищет хост),
    # и в /system/lib64 (оттуда подставка открывается обычным поиском).
    for lib in libhci_interface_service_1.0.z.so libbluetooth_hci_hdi_driver.z.so \
               libbt_vendor.z.so; do
        src=$(find_out "$lib")
        if [ -n "$src" ]; then
            install -m 644 "$src" "$VLIB"/
            install -m 644 "$src" "$LIB"/
            echo "  $lib"
        else
            echo "  не найдено: $lib"
        fi
    done

    install -m 644 "$TREE/foundation/communication/bluetooth_service/sa_profile/1130.json" \
            /system/profile/bluetooth_service.json

    # Настройки службы лежат в изменяемом разделе: она их правит сама,
    # запоминая сопряжённые устройства. Поэтому кладём только недостающие.
    mkdir -p /data/service/el1/public/bluetooth
    chmod 770 /data/service/el1/public/bluetooth
    local f
    for f in bt_config.xml bt_device_config.xml bt_profile_config.xml bt_device_info.xml; do
        [ -f "/data/service/el1/public/bluetooth/$f" ] && continue
        install -m 660 \
            "$TREE/foundation/communication/bluetooth_service/services/bluetooth/etc/init/$f" \
            /data/service/el1/public/bluetooth/
    done

    src=$(find_out libbluetooth_hdi_adapter.z.so)
    if [ -n "$src" ]; then
        install -m 644 "$src" "$LIB"/
        # Стек ищет переходник под именем без окончания «.z».
        ln -sf libbluetooth_hdi_adapter.z.so "$LIB"/libbluetooth_hdi_adapter.so
        echo "  libbluetooth_hdi_adapter.z.so"
    else
        echo "  не найдено: libbluetooth_hdi_adapter.z.so"
    fi



}

# ---------------------------------------------------------------------------
# Останов и сброс состояния
# ---------------------------------------------------------------------------
stop_all() {
    local p
    pkill -f "$BIN/" 2>/dev/null

    for p in accesstoken_service installs storage_manager accountmgr foundation bms \
             appspawn composer_host allocator_host param_watcher inputmethod_service \
             distributeddata screenlock_server useriam powermgr audio_server \
             bluetooth_service com.ohos.sceneboard; do
        pkill -f "^$p" 2>/dev/null
    done

    sleep 1
    pkill -9 -f "$BIN/" 2>/dev/null
    rm -f /dev/unix/socket/hilog*
    sed -i 's/"isVerified":true/"isVerified":false/' \
        /data/service/el1/public/account/100/account_info.json 2>/dev/null
    rm -f "$TOKEN_BYPID"/*

    systemctl stop ohos-bluetooth_host ohos-audio_host ohos-power_host \
                   ohos-composer_host ohos-allocator_host ohos-useriam_host \
                   ohos-render_service 2>/dev/null
    pkill -f multimodalinput
    pkill -f sa_main
    pkill -x audio_server
    pkill -f "svc_ctl.sh" 
    #pkill -x bluetooth_service
}

# Стирать после любого изменения списка процессов или прав в token_init.cpp.
reset_tokens() {
    rm -f  /data/service/el1/public/access_token/*.db*
    rm -f  /data/service/el0/access_token/nativetoken.json*
    rm -rf "$TOKEN_BYNAME"

    # Удостоверения приложений выдаёт распорядитель пакетов при установке
    # и хранит их в том же хранилище. Стерев хранилище, обязаны стереть и
    # его состояние, иначе он сочтёт пакеты установленными, а их
    # удостоверений уже не будет — оболочка не запустится вовсе.
    rm -rf /data/service/el1/public/bms/bundle_manager_service/*
    rm -rf /data/app/el1/bundle/public/*
    rm -rf /data/app/el1/100/base/*
    rm -rf /data/app/el1/100/database/*
    fix_owners
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
    local id=$1 tries=${2:-150}
    for _ in $(seq 1 "$tries"); do
        "$SAMGR_CLIENT" 2>/dev/null | grep -qx "  $id" && return 0
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

# Работаем от корня: службы читают свои настройки по относительным путям.
start_sa() {
    local name=$1 id=$2 tries=${3:-150}
    echo "$name ($id)"
    ( cd / && exec /bin/bash -c "$TOKEN_WRAPPER" _ "$name" "$SA_MAIN" "/system/profile/$name.json" ) \
        > "$LOGDIR/$name.log" 2>&1 &
    echo "  pid $!"
    wait_sa "$id" "$tries" || { echo "  не поднялся, см. $LOGDIR/$name.log"; exit 1; }
}

# Запуск службы под своим пользователем, группами и возможностями —
# то, что на устройстве делает init по *.cfg.
start_sa_as() {
    local name=$1 id=$2 uid=$3 groups=${4:-} caps=${5:-} tries=${6:-150}
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
    ( cd / && exec /bin/bash -c "$TOKEN_WRAPPER" _ "$name" setpriv "${opts[@]}" \
        "$SA_MAIN" "/system/profile/$name.json" ) \
        > "$LOGDIR/$name.log" 2>&1 &
    echo "  pid $!"
    wait_sa "$id" "$tries" || { echo "  не поднялся, см. $LOGDIR/$name.log"; exit 1; }
}

# ---------------------------------------------------------------------------
case "${1:-start}" in
stop)   stop_all; echo "остановлено"; exit 0 ;;
status) "$SAMGR_CLIENT" 2>/dev/null; exit 0 ;;
tokens) [ "$(id -u)" = 0 ] || { echo "нужен root"; exit 1; }
        stop_all; reset_tokens
        echo "удостоверения и состояние пакетов стёрты; следующий запуск будет долгим"
        exit 0 ;;
reset)  [ "$(id -u)" = 0 ] || { echo "нужен root"; exit 1; }
        stop_all; reset_state; echo "состояние стёрто"; exit 0 ;;
install) [ "$(id -u)" = 0 ] || { echo "нужен root"; exit 1; }
         install_all; exit 0 ;;
esac

[ "$(id -u)" = 0 ] || { echo "нужен root"; exit 1; }

ensure_dirs
stop_all

# Радиомодуль может обслуживать только одного хозяина: пользовательский
# режим HCI не откроется, пока адаптер держит BlueZ.
systemctl stop bluetooth 2>/dev/null

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
"$BIN/hilog" -G 16M

echo "samgr"
start_bg samgr "$BIN/samgr"
wait_samgr || { echo "  samgr не отвечает, см. $LOGDIR/samgr.log"; exit 1; }

echo "присмотрщик служб (замена init)"
#"$SCRIPT_DIR/svc_ctl.sh" &

setsid "$SCRIPT_DIR/svc_ctl.sh" </dev/null >>"$LOGDIR/svc_ctl.out" 2>&1 &

echo "  pid $!"


start_sa    accesstoken_service 3503
start_sa_as installs 511 $UID_INSTALLS 1000 chown,dac_override,fowner,sys_admin

echo "storage_daemon"
start_bg storage_daemon "$BIN/storage_daemon"
sleep 1

start_sa    storage_manager 5003

# Распределённое хранилище поднимаем до менеджера учётных записей: тот при
# первой же активации записи пишет её состояние через это хранилище и без
# него встаёт в вечные повторы, так и не сообщив менеджеру способностей
# о запуске пользователя.
start_sa    distributeddata 1301

start_sa_as accountmgr 200 $UID_ACCOUNT 1000

sleep 1
set_token multimodalinput

# Подготовка среды ArkUI до ветвления: без неё набор stateMgmt
# не попадает в рабочие потоки приложений.
"$BIN/param" set persist.appspawn.preload false
"$BIN/param" set persist.appspawn.preloadets false

start_sa param_watcher 3901
start_sa inputmethod_service 3703
start_sa screenlock_server 3704
start_sa useriam 901

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

# Хосты драйверов. На устройстве их запускает init по hdf_devhost.cfg.
# Номер -i — это порядковый номер хоста в arkvm/hdf/device_info.hcs,
# считая с нуля. Меняя описание, не забывайте пересобрать свод hc-gen
# и разложить его: иначе номера разъедутся.
echo "драйверы"
start_hdf allocator_host  /vendor/bin/hdf_devhost -i 1 -n allocator_host
start_hdf composer_host   /vendor/bin/hdf_devhost -i 0 -n composer_host
start_hdf useriam_host    /vendor/bin/hdf_devhost -i 2 -n useriam_host
start_hdf power_host      /vendor/bin/hdf_devhost -i 3 -n power_host
start_hdf audio_host      /vendor/bin/hdf_devhost -i 4 -n audio_host
start_hdf bluetooth_host  /vendor/bin/hdf_devhost -i 5 -n bluetooth_host

sleep 2

# Звук ждёт своего слоя драйверов и первый раз поднимается долго.
start_sa audio_server 3009 900

# Служба отрисовки обязана подняться раньше оконного менеджера: тот при
# старте спрашивает у неё список экранов. Сама она требует композитора.
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

start_sa multimodalinput 3101
start_sa powermgr 3301
start_sa bluetooth_service 1130 300

# foundation держит менеджеры способностей, приложений, пакетов, а в режиме
# сцены ещё и службу окон (4607) с посредником сеансов (4606).
# После сброса он ставит пакеты заново — ждём дольше обычного.
start_sa_as foundation 180 $UID_FOUNDATION 1000 "" 600
sleep 1
set_token foundation


# Распорядитель пакетов обходит /system/app один раз, при своём запуске.
# После полного сброса учётная запись 100 создаётся позже этого мига, и обход
# отвергает все пакеты: непривилегированные нельзя ставить нулевому
# пользователю. Дожидаемся записи и повторяем обход ещё раз.
ACCOUNT_INFO=/data/service/el1/public/account/100/account_info.json
if [ "$("$BIN/param" get bootevent.bms.main.bundles.ready 2>/dev/null)" != "true" ]; then
    echo "пакеты не установлены, ждём учётную запись 100"
    for _ in $(seq 1 120); do
        [ -f "$ACCOUNT_INFO" ] && break
        sleep 0.5
    done
    if [ -f "$ACCOUNT_INFO" ]; then
        sleep 2
        echo "повторный обход пакетов"
        pkill -f "^foundation" 2>/dev/null
        sleep 2
        start_sa_as foundation 180 $UID_FOUNDATION 1000 "" 600
        sleep 1
        set_token foundation
    else
        echo "учётная запись 100 так и не появилась"
    fi
fi



chmod 666 /dev/unix/socket/AppSpawn 2>/dev/null

"$BIN/param" set bootevent.boot.completed true
echo
echo "готово, реестр:"
"$SAMGR_CLIENT" 2>/dev/null
echo
echo "признаки готовности:"
"$BIN/param" get | grep -iE "bms.main.bundles.ready|wms.fullscreen.ready"
echo
echo "журнал:  sudo env LD_LIBRARY_PATH=$LIB $BIN/hilog"
