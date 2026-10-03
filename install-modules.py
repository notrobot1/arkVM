#!/usr/bin/env python3
"""
Раскладывает собранные файлы по системным каталогам так, как это сделал бы
сборщик образа OpenHarmony.

При сборке для каждой цели с install_enable создаётся описание
out/<product>/obj/**/<цель>_module_info.json вида:

    {
      "install_enable": true,
      "source": "obj/base/startup/appspawn/etc/appdata-sandbox.json/appdata-sandbox.json",
      "dest":   [ "system/etc/sandbox/appdata-sandbox.json" ],
      "symlink": [ ... ],
      "type": "etc"
    }

Скрипт обходит все такие описания и копирует файлы по указанным путям,
создавая недостающие каталоги и символические ссылки.

Зачем это нужно: плоское копирование вида
    find out/... -name '*.so' -exec cp {} /system/lib64/ \\;
теряет структуру подкаталогов. А она существенна: модули appspawn грузятся
из lib64/appspawn/common и lib64/appspawn/appspawn, модули расширения ArkTS —
из lib64/module/app/ability и подобных, заглушки виртуальной машины — из
lib64/module/arkcompiler. При плоской раскладке всё это молча не находится.

Использование:
    sudo ./install-modules.py [каталог_сборки] [корень]
    sudo ./install-modules.py out/arkvm /
    sudo ./install-modules.py out/arkvm / --dry-run
"""

import json
import os
import shutil
import sys

EXEC_TYPES = {"bin", "exe", "executable"}


def is_executable(info, dest):
    if info.get("type") in EXEC_TYPES:
        return True
    return "/bin/" in dest or dest.endswith("/bin")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    dry_run = "--dry-run" in sys.argv
    verbose = "-v" in sys.argv or "--verbose" in sys.argv

    out_dir = args[0] if len(args) > 0 else "out/arkvm"
    root = args[1] if len(args) > 1 else "/"

    obj_dir = os.path.join(out_dir, "obj")
    if not os.path.isdir(obj_dir):
        sys.exit("не найден каталог %s" % obj_dir)

    installed = 0
    linked = 0
    missing = []
    disabled = 0

    for dirpath, _dirnames, filenames in os.walk(obj_dir):
        for name in filenames:
            if not name.endswith("_module_info.json"):
                continue

            try:
                with open(os.path.join(dirpath, name)) as handle:
                    info = json.load(handle)
            except (OSError, ValueError):
                continue

            if not info.get("install_enable", False):
                disabled += 1
                continue

            source = info.get("source")
            dests = info.get("dest") or []
            if not source or not dests:
                continue

            src_path = os.path.join(out_dir, source)
            if not os.path.exists(src_path):
                missing.append(source)
                continue

            for dest in dests:
                dst_path = os.path.join(root, dest.lstrip("/"))
                dst_dir = os.path.dirname(dst_path)

                if verbose or dry_run:
                    print("%s -> %s" % (source, dst_path))
                if dry_run:
                    installed += 1
                    continue

                os.makedirs(dst_dir, exist_ok=True)
                # Файл может быть занят работающим процессом ("Text file busy").
                # Отвязываем имя: запущенная программа продолжит жить со старым
                # образом, а по имени окажется новый.
                if os.path.islink(dst_path) or os.path.isfile(dst_path):
                    try:
                        os.remove(dst_path)
                    except OSError:
                        pass
                # У приложений, собираемых сборщиком приложений (ohos_app),
                # источником в описании значится каталог с готовым пакетом,
                # а не сам пакет. Раскладываем содержимое целиком.
                if os.path.isdir(src_path):
                    os.makedirs(dst_path, exist_ok=True)
                    for name in sorted(os.listdir(src_path)):
                        item = os.path.join(src_path, name)
                        if not os.path.isfile(item):
                            continue
                        target = os.path.join(dst_path, name)
                        if os.path.islink(target) or os.path.isfile(target):
                            try:
                                os.remove(target)
                            except OSError:
                                pass
                        shutil.copy2(item, target)
                        os.chmod(target, 0o644)
                    installed += 1
                    continue

                shutil.copy2(src_path, dst_path)
                os.chmod(dst_path, 0o755 if is_executable(info, dest) else 0o644)
                installed += 1

                # Символические ссылки: имена задаются без пути и создаются
                # рядом с основным файлом. Так, например, ставится пустая
                # заглушка libappspawn_module_engine.so.
                for link_name in info.get("symlink") or []:
                    link_path = os.path.join(dst_dir, os.path.basename(link_name))
                    if link_path == dst_path:
                        continue
                    if os.path.islink(link_path) or os.path.exists(link_path):
                        os.remove(link_path)
                    os.symlink(os.path.basename(dst_path), link_path)
                    linked += 1
                    if verbose:
                        print("    ссылка %s" % link_path)

    print()
    print("установлено файлов: %d" % installed)
    print("создано ссылок:     %d" % linked)
    print("целей без установки: %d" % disabled)
    if missing:
        print("нет исходных файлов: %d" % len(missing))
        for item in missing[:10]:
            print("    %s" % item)
        if len(missing) > 10:
            print("    ... и ещё %d" % (len(missing) - 10))


if __name__ == "__main__":
    main()
