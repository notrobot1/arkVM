#!/usr/bin/env bash
# Замена init в части управления службами.
#
# На устройстве процессы служб запускает init. Диспетчер служб просит его об
# этом через системные свойства и ждёт отметки о состоянии:
#
#   ohos.ctl.start = "имя"  либо  "имя|довод|довод"   — запустить
#   ohos.ctl.stop  = "имя"                            — остановить
#   ohos.ctl.term  = "имя"                            — прекратить
#   startup.service.ctl.<имя> = 1 запускается, 2 работает, 5 остановлен
#
# init у нас нет, поэтому просьбы уходили в пустоту: служба, выгруженная
# диспетчером по бездействию, обратно уже не поднималась, и обращение к ней
# оборачивалось пятисекундным ожиданием впустую. Этот присмотрщик слушает те
# же свойства и делает то же, что сделал бы init.
#
# Запускается из start.sh сразу после диспетчера служб, в своём сеансе,
# чтобы ни он, ни поднятые им службы не ушли вместе с окном терминала:
#     setsid "$SCRIPT_DIR/svc_ctl.sh" </dev/null >>"$LOGDIR/svc_ctl.out" 2>&1 &

set -u

BIN=${BIN:-/system/bin}
LOGDIR=${LOGDIR:-/tmp/arkvm}
LOG="$LOGDIR/svc_ctl.log"

PARAM=$BIN/param
SA_MAIN=$BIN/sa_main
SAMGR_CLIENT=$BIN/samgr_client
# Службы, которые поднимает start.sh сам. Присмотрщик их не трогает: иначе
# на просьбу диспетчера он поднимет второй такой же процесс, и два
# одинаковых будут мешать друг другу — подписываться на одни события,
# спорить за объявление способности.
SKIP_NAMES=${SKIP_NAMES:-"foundation wifi_manager_service device_manager deviceauth_service softbus_server"}


# Сколько раз подряд пробовать поднять службу, прежде чем оставить её в покое
# до новой просьбы. Без этого предела служба, которая не может подняться в
# принципе, поднимается вечно: свойство с просьбой не очищается, и каждая
# неудача оборачивается новым падением и новым слепком памяти в /tmp.
MAX_FAILS=${MAX_FAILS:-3}

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/system/lib64:/system/lib64/platformsdk:/system/lib64/chipset-sdk:/system/lib64/chipset-sdk-sp:/system/lib64/ndk:/vendor/lib64:/vendor/lib64/passthrough}

# Тело обёртки маркера доступа — то же, что в start.sh: ядро маркеров не
# хранит, поэтому процесс сам кладёт файл по своему номеру и подменяется
# на службу, сохраняя номер.
TOKEN_WRAPPER='
    tok=$(cat "/data/service/el0/access_token/byname/$1" 2>/dev/null)
    if [ -n "$tok" ]; then
        printf "%s" "$tok" > "/data/service/el0/access_token/bypid/$$"
        chmod 644 "/data/service/el0/access_token/bypid/$$" 2>/dev/null
    fi
    shift
    exec "$@"
'

mkdir -p "$LOGDIR"

declare -A LAUNCHED   # имя процесса → номер, который мы запустили
declare -A FAILS      # имя процесса → сколько раз подряд не поднялся

say() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; }

trap 'say "присмотрщик завершается, код $?"' EXIT

# param get при отсутствии свойства печатает не пустоту, а жалобу — отсеиваем.
get_param() {
    local v
    v=$("$PARAM" get "$1" 2>/dev/null)
    case "$v" in
        ""|"(null)"|*fail!*) return 0 ;;
    esac
    printf '%s' "$v"
}

set_status() { "$PARAM" set "startup.service.ctl.$1" "$2" >/dev/null 2>&1; }

# Номера способностей из описания службы — тот же признак, которым
# пользуется сам диспетчер.
sa_ids() {
    grep -o '"name"[[:space:]]*:[[:space:]]*[0-9]\+' "/system/profile/$1.json" 2>/dev/null |
        grep -o '[0-9]\+'
}

# Жива ли служба. Сперва дешёвая проверка по нашему же номеру процесса,
# затем — по перечню диспетчера. По имени процесса проверять нельзя: ядро
# хранит его в пятнадцати знаках, а sa_main вдобавок переписывает доводы.
alive() {
    local name=$1
    local pid=${LAUNCHED[$name]:-}
    local id list

    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    list=$("$SAMGR_CLIENT" 2>/dev/null)
    for id in $(sa_ids "$name"); do
        printf '%s\n' "$list" | grep -qx "  $id" && return 0
    done
    return 1
}

# Запуск тем же способом, что и в start.sh: от корня, с маркером доступа,
# в своём сеансе. Службы способностей узнаём по описанию в /system/profile,
# всё прочее — по исполняемому файлу в /system/bin.
#
# Доводы передаём дальше обязательно: для служб с пометкой «не поднимать при
# создании» диспетчер сообщает в них номер нужной способности, и без него
# sa_main не поднимет ничего.
launch() {
    local name=$1; shift
    local profile="/system/profile/$name.json"

    if [ -f "$profile" ]; then
        say "поднимаю службу $name${*:+ (доводы: $*)}"
        setsid /bin/bash -c "cd / && $TOKEN_WRAPPER" _ "$name" "$SA_MAIN" "$profile" "$@" \
            >> "$LOGDIR/$name.log" 2>&1 &
        LAUNCHED[$name]=$!
        return 0
    fi

    if [ -x "$BIN/$name" ]; then
        say "поднимаю обычный процесс $name${*:+ (доводы: $*)}"
        setsid /bin/bash -c "cd / && $TOKEN_WRAPPER" _ "$name" "$BIN/$name" "$@" \
            >> "$LOGDIR/$name.log" 2>&1 &
        LAUNCHED[$name]=$!
        return 0
    fi

    say "не знаю, как поднять $name: нет ни описания, ни исполняемого файла"
    return 1
}

handle_start() {
    local value=$1
    local name=${value%%|*}
    local rest=${value#*|}
    local args=()
    local i




    [ -n "$name" ] || return

    case " $SKIP_NAMES " in
        *" $name "*) set_status "$name" 2; return ;;
    esac



    if alive "$name"; then
        # Уже работает — просто подтверждаем отметку: диспетчер мог потерять
        # её после своего перезапуска.
        set_status "$name" 2
        return
    fi

    if [ "${FAILS[$name]:-0}" -ge "$MAX_FAILS" ]; then
        return          # не поднимается — оставляем в покое до новой просьбы
    fi

    if [ "$rest" != "$value" ]; then
        IFS='|' read -r -a args <<< "$rest"
    fi

    set_status "$name" 1
    if ! launch "$name" "${args[@]+"${args[@]}"}"; then
        FAILS[$name]=$MAX_FAILS     # поднимать нечем, пробовать бессмысленно
        set_status "$name" 5
        return
    fi

    # Диспетчер отводит на запуск пять секунд — укладываемся быстрее.
    for i in $(seq 1 40); do
        sleep 0.1
        if alive "$name"; then
            set_status "$name" 2
            FAILS[$name]=0
            say "$name поднят"
            return
        fi
    done

    FAILS[$name]=$(( ${FAILS[$name]:-0} + 1 ))
    say "$name не появился (неудача ${FAILS[$name]} из $MAX_FAILS)"
    set_status "$name" 5
}

handle_stop() {
    local name=$1
    local signal=$2
    local pid=${LAUNCHED[$name]:-}

    [ -n "$name" ] || return
    say "останавливаю $name сигналом $signal"
    if [ -n "$pid" ]; then
        kill "-$signal" "$pid" 2>/dev/null
        unset "LAUNCHED[$name]"
    fi
    set_status "$name" 5
}

say "присмотрщик служб запущен"

last_start=""
last_stop=""
last_term=""
next_check=0

while true; do
    now=$(date +%s)

    v=$(get_param ohos.ctl.start)
    if [ -n "$v" ]; then
        if [ "$v" != "$last_start" ]; then
            # Новая просьба — исполняем сразу и прощаем прежние неудачи.
            last_start=$v
            FAILS[${v%%|*}]=0
            handle_start "$v"
            next_check=$((now + 2))
        elif [ "$now" -ge "$next_check" ]; then
            # Прежняя просьба в силе. Диспетчер повторно её не подаёт, а сам
            # же выгружает службу по бездействию, поэтому изредка проверяем,
            # на месте ли она. Редко: проверка спрашивает диспетчер служб.
            next_check=$((now + 2))
            alive "${v%%|*}" || handle_start "$v"
        fi
    fi

    v=$(get_param ohos.ctl.stop)
    if [ -n "$v" ] && [ "$v" != "$last_stop" ]; then
        last_stop=$v
        handle_stop "$v" TERM
    fi

    v=$(get_param ohos.ctl.term)
    if [ -n "$v" ] && [ "$v" != "$last_term" ]; then
        last_term=$v
        handle_stop "$v" KILL
    fi

    sleep 0.3
done
