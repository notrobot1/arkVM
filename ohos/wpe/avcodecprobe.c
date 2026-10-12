/*
 * Проба расшифровщиков OpenHarmony.
 *
 * Спрашивает у системы, какие виды записи она берётся расшифровывать, и
 * пробует завести расшифровщик картинки. Нужна до того, как писать элемент
 * GStreamer поверх OH_AVCodec: если служба не отвечает или в сборке нет ни
 * одного расшифровщика, писать нечего — сперва надо добрать части в состав
 * изделия.
 *
 * Сборка (в контейнере):
 *   NDK=/mnt/ohos/OpenHarmony-6.1-Release/prebuilts/ohos-sdk/linux/26.0.0/native
 *   clang-18 --target=x86_64-linux-ohos --sysroot=$NDK/sysroot \
 *     -resource-dir=$WPE/clang18-rt -B$WPE/clang18-b \
 *     -o /mnt/ohos/wpe/avcodecprobe /mnt/ohos/wpe/avcodecprobe.c \
 *     -fuse-ld=lld-18 --unwindlib=none \
 *     -L$NDK/sysroot/usr/lib/x86_64-linux-ohos \
 *     -lnative_media_codecbase -lnative_media_core -lnative_media_vdec \
 *     -lnative_media_adec
 *
 * Запуск (на машине, при поднятой системе):
 *   sudo ./avcodecprobe
 */

#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_audiodecoder.h>
#include <multimedia/player_framework/native_averrors.h>

#include <stdio.h>

static void askAbout(const char* mime)
{
    printf("\n%s\n", mime);

    OH_AVCapability* cap = OH_AVCodec_GetCapability(mime, false);
    if (!cap) {
        printf("  расшифровщика нет\n");
        return;
    }

    const char* name = OH_AVCapability_GetName(cap);
    printf("  имя:       %s\n", name ? name : "(без имени)");
    printf("  на железе: %s\n", OH_AVCapability_IsHardware(cap) ? "да" : "нет");

    /* Ещё спросим про кодировщик — по нему видно, насколько полно
       подключён набор, но для нас он не обязателен. */
    OH_AVCapability* enc = OH_AVCodec_GetCapability(mime, true);
    printf("  кодировщик: %s\n", enc ? "есть" : "нет");
}

int main(void)
{
    printf("Опрос расшифровщиков OpenHarmony\n");
    printf("================================\n");

    static const char* mimes[] = {
        OH_AVCODEC_MIMETYPE_VIDEO_AVC,      /* H.264 — обиходнее всего */
        OH_AVCODEC_MIMETYPE_VIDEO_HEVC,
        OH_AVCODEC_MIMETYPE_VIDEO_VP9,
        OH_AVCODEC_MIMETYPE_AUDIO_AAC,
        OH_AVCODEC_MIMETYPE_AUDIO_MPEG,     /* MP3 */
        OH_AVCODEC_MIMETYPE_AUDIO_OPUS,
        OH_AVCODEC_MIMETYPE_AUDIO_VORBIS,
        OH_AVCODEC_MIMETYPE_AUDIO_FLAC,
        NULL
    };
    for (const char** m = mimes; *m; m++)
        askAbout(*m);

    /* Теперь не на словах, а на деле: заводим расшифровщик картинки.
       Опрос способностей может отвечать из описаний, а создание уже
       требует живой службы. */
    printf("\nсоздание расшифровщика H.264:\n");
    OH_AVCodec* dec = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (!dec) {
        printf("  не создался — служба расшифровки недоступна\n");
        return 1;
    }
    printf("  создался\n");
    OH_AVErrCode ret = OH_VideoDecoder_Destroy(dec);
    printf("  освобождён: %d\n", ret);

    printf("\nсоздание расшифровщика звука AAC:\n");
    OH_AVCodec* adec = OH_AudioDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC);
    if (!adec) {
        printf("  не создался\n");
        return 1;
    }
    printf("  создался\n");
    OH_AudioDecoder_Destroy(adec);

    printf("\nготово\n");
    return 0;
}
