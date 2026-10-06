#!/usr/bin/env python3
"""Запуск netsysnative без init.

Служба 1158 не создаёт свои сокеты сама: по описанию netsysnative.cfg их
готовит init, а службе передаёт уже открытыми. Имя каждого сокета ищется
в переменной окружения OHOS_SOCKET_<имя>, значение — номер описателя.

Сокетов четыре:
    dnsproxyd    — через него musl спрашивает разрешение имён;
    fwmarkd      — пометки для правил обхода;
    tunfd        — передача описателя устройства туннеля;
    multivpnfd   — то же для нескольких туннелей сразу.

Нам по существу нужен только первый, но служба при запуске ждёт все четыре.
"""

import os
import socket
import sys

SOCKET_DIR = "/dev/unix/socket"
NAMES = ["dnsproxyd", "fwmarkd", "tunfd", "multivpnfd"]
SA_MAIN = "/system/bin/sa_main"
PROFILE = "/system/profile/netsysnative.json"

os.makedirs(SOCKET_DIR, exist_ok=True)

# Хранилище настроек разрешения имён, которое init создаёт заранее.
os.makedirs("/data/service/el1/public/netsysnative", mode=0o711, exist_ok=True)

env = dict(os.environ)
held = []  # держим ссылки, иначе сборщик мусора закроет сокеты до запуска

for name in NAMES:
    path = os.path.join(SOCKET_DIR, name)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    s.listen(16)
    # В описании стоит 0660 с отдельной группой netsys_socket. У нас все
    # службы работают от одного пользователя, поэтому проще дать общий доступ.
    os.chmod(path, 0o666)
    os.set_inheritable(s.fileno(), True)
    held.append(s)
    env["OHOS_SOCKET_" + name] = str(s.fileno())
    print("сокет %s -> описатель %d" % (path, s.fileno()), file=sys.stderr)

os.execve(SA_MAIN, [SA_MAIN, PROFILE], env)
