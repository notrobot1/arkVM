# Родная часть обозревателя.
#
# Собирается средствами пакета для приложений (clang 15), но связывается
# с нашим движком, собранным отдельно (clang 18). Это допустимо: наружу
# WebKit выставляет только вызовы на C, и разные выпуски стандартной
# библиотеки C++ по обе стороны друг другу не мешают.

cmake_minimum_required(VERSION 3.5.0)
project(wpebrowser)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

# Промежуточный корень, куда собран движок и всё его окружение.
# На устройстве те же библиотеки лежат в /system/lib64.
set(WPE_ROOT /mnt/ohos/wpe/stage/root/system)

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
    ${WPE_ROOT}/include/wpe-webkit-2.0
    ${WPE_ROOT}/include/wpe-webkit-2.0/wpe-platform
    ${WPE_ROOT}/include/glib-2.0
    ${WPE_ROOT}/lib64/glib-2.0/include
)

add_library(wpe SHARED napi_init.cpp)

target_link_libraries(wpe PUBLIC
    libace_napi.z.so            # связь с ArkTS
    libace_ndk.z.so             # XComponent
    libnative_window.so         # номер поверхности у окна
    libhilog_ndk.z.so           # записи в общий журнал

    ${WPE_ROOT}/lib64/libWPEWebKit-2.0.so
    ${WPE_ROOT}/lib64/libglib-2.0.so
    ${WPE_ROOT}/lib64/libgobject-2.0.so
)
