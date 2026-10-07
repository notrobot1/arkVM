# Обозреватель на ArkTS: сборка

Приложение с одним `XComponent`, в который рисует WPE WebKit.

## Что здесь есть

```
wpebrowser/
├── AppScope/app.json5                      имя пакета com.arkvm.browser
├── build-profile.json5                     подпись и уровни пакета
├── entry/build-profile.json5               сборка родной части
└── entry/src/main/
    ├── module.json5                        описание части, разрешения
    ├── ets/entryability/EntryAbility.ets    точка входа
    ├── ets/pages/Index.ets                  строка адреса + XComponent
    └── cpp/
        ├── CMakeLists.txt                   сборка libwpe.so
        ├── napi_init.cpp                    связь XComponent ↔ WPE
        └── types/libwpe/                    объявления для ArkTS
```

## Чего не хватает (взять у файлового управляющего)

Мелочь, которую проще скопировать, чем сочинять:

```sh
B=/путь/к/wpebrowser
F=/mnt/ohos/OpenHarmony-6.1-Release/applications/standard/filepicker/oh_filemanger_6.1/filemanager

# подпись: хранилище ключей и удостоверение — те же
mkdir -p $B/signature
cp -a $F/signature/OpenHarmony.cer $F/signature/OpenHarmony.p12 $F/signature/material $B/signature/

# значки и прочие ресурсы приложения
cp -a $F/AppScope/resources $B/AppScope/

# обвязка сборки
cp $F/hvigorfile.ts $F/oh-package.json5 $B/
cp -a $F/hvigor $B/ 2>/dev/null || true
```

Ещё нужны ресурсы части (`entry/src/main/resources`): строки `module_desc`,
`EntryAbility_desc`, `EntryAbility_label`, значки `layered_image`, `startIcon`,
цвет `start_window_background` и `profile/main_pages.json` со строкой
`"pages/Index"`. Их тоже проще взять у любого готового приложения и
подправить названия.

## Разрешение на имя пакета

Разрешение (`.p7b`) привязано к имени пакета, и чужое не подойдёт.
Выпускаем своё:

```sh
bash make-profile.sh com.arkvm.browser /путь/к/wpebrowser/signature/browser.p7b
```

Оно получит уровень `system_core` и признак `hos_system_app` — тот же, что
у файлового управляющего. Этого достаточно, чтобы приложение считалось
системным и получило нужные разрешения.

## Сборка

Так же, как собирается файловый управляющий (через hvigor, не через
`build.sh`). Родная часть соберётся вместе с приложением: `CMakeLists.txt`
указывает на промежуточный корень `/mnt/ohos/wpe/stage/root/system`, где
лежат заголовки и библиотеки движка.

## Как это работает

1. ArkUI создаёт поверхность для `XComponent` и зовёт `OnSurfaceCreated`.
2. Родная часть спрашивает у окна номер поверхности
   (`OH_NativeWindow_GetSurfaceId`) и кладёт его в `WPE_OHOS_SURFACE_ID`.
3. Запускает движок в **отдельном потоке** со своей чередой событий: у
   ArkUI своя, у GLib своя, смешивать нельзя.
4. Площадка `WPEPlatformOHOS` подхватывает номер при подключении и рисует
   прямо в окно приложения. Менять в ней ничего не потребовалось.

## Чего пока нет

**Ввода.** Прикосновения приходят в `DispatchTouchEvent`, но дальше не
идут. Следующий шаг: переложить их в `wpe_view_event`, перекинув вызов в
череду потока движка через `g_main_context_invoke`.

**Изменения размера на ходу.** Окно всегда во весь экран.

**Настоящей внешности.** Строка адреса здесь только для проверки. Готовая
внешность — в `applications/browser`, и опирается она на `webview` ровно в
одном файле: `feature/web/src/main/ets/BrowsePageView.ets`, строка 1430.
