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

target_link_libraries(wpe PUBLIC
    libace_napi.z.so            # связь с ArkTS
    libace_ndk.z.so             # XComponent
    libnative_window.so         # номер поверхности у окна
    libhilog_ndk.z.so           # записи в общий журнал

)

# А движок и его окружение живут в /system/lib64 и в пакет не идут.
# Передаём их прямыми доводами, иначе hvigor попытается их упаковать.
target_link_options(wpe PRIVATE
    -L${WPE_ROOT}/lib64
    -lWPEWebKit-2.0
    -lglib-2.0
    -lgobject-2.0
)
