# Родная часть обозревателя.
#
# Собирается средствами пакета для приложений (clang 15), но пользуется
# движком, собранным отдельно (clang 18). Это допустимо: наружу WebKit
# выставляет только вызовы на C.
#
# Сам движок здесь НЕ связывается: он подтягивается во время работы, уже
# из потока движка. Полторы сотни мегабайт, загруженные в потоке разметки,
# не укладываются в отведённое приложению время запуска, и система считает
# его зависшим. Поэтому наружу — лёгкая библиотека, тяжёлое грузится в
# стороне. Заголовки движка всё равно нужны: из них берутся описания видов.

cmake_minimum_required(VERSION 3.5.0)
project(wpebrowser)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

# Набор, собранный отдельно. На устройстве то же лежит в /system/lib64.
set(WPE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../wpe-sdk)

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
    ${WPE_ROOT}/include
    ${WPE_ROOT}/include/wpe-webkit-2.0
    ${WPE_ROOT}/include/wpe-webkit-2.0/wpe-platform
    ${WPE_ROOT}/include/glib-2.0
    ${WPE_ROOT}/include/libsoup-3.0
    ${WPE_ROOT}/lib64/glib-2.0/include
)

add_library(wpe SHARED napi_init.cpp)

# Части приложения: их hvigor положит в пакет.
target_link_libraries(wpe PUBLIC
    libace_napi.z.so            # связь с ArkTS
    libace_ndk.z.so             # XComponent
    libnative_window.so         # номер поверхности у окна
    libhilog_ndk.z.so           # записи в общий журнал
)

# glib и gobject невелики, их связываем обычным образом, но из /system/lib64,
# а не из пакета — поэтому прямыми доводами, мимо учёта hvigor.
target_link_options(wpe PRIVATE
    -L${WPE_ROOT}/lib64
    -lglib-2.0
    -lgobject-2.0
)
