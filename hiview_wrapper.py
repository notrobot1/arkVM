#!/usr/bin/env python3
"""Запуск приёмника системных событий без init.

Разъёмы hisysevent и hisysevent_fast на устройстве заводит init по описанию
службы и передаёт ей готовыми: число открытого описателя кладётся в
переменную окружения OHOS_SOCKET_<имя>. Сама hiview их не создаёт, поэтому
без init не поднимается, а все желающие писать события стучатся в пустоту —
около восьми тысяч неудачных попыток в минуту.

Эта обёртка делает работу init: заводит разъёмы, привязывает к путям,
оставляет наследуемыми и подменяется на hiview с тем же номером процесса.
"""
import os
import socket
import sys

SOCK_DIR = "/dev/unix/socket"
NAMES = ("hisysevent", "hisysevent_fast")
HIVIEW = "/system/bin/hiview"

os.makedirs(SOCK_DIR, exist_ok=True)
env = dict(os.environ)
keep = []          # держим ссылки, иначе разъёмы закроются сборщиком мусора

for name in NAMES:
    path = os.path.join(SOCK_DIR, name)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
    s.bind(path)
    s.setblocking(False)
    os.chmod(path, 0o662)
    os.set_inheritable(s.fileno(), True)
    env["OHOS_SOCKET_" + name] = str(s.fileno())
    keep.append(s)
    print(f"{name}: описатель {s.fileno()}, путь {path}")

os.execve(HIVIEW, [HIVIEW], env)
