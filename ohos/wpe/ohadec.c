/*
 * Расшифровщик звука для GStreamer поверх службы расшифровки OpenHarmony.
 *
 * Кладётся в /system/lib64/gstreamer-1.0/libgstohadec.so и подхватывается
 * сам. Имя части — ohadec.
 *
 * ЗАЧЕМ. Без него не играет ни одна настоящая запись: дорожка AAC или MP3
 * останавливает цепочку целиком, и молчит не только звук, но и картинка
 * рядом с ним. Играл до сих пор только WAV — тот, что расшифровывать не
 * надо вовсе.
 *
 * УСТРОЙСТВО. То же, что у ohavdec: две очереди между толкающим GStreamer и
 * спрашивающей службой, плюс отдельная нить отдачи на исходящем выводе.
 * Пояснения к этому устройству — в ohavdec.c, здесь они не повторяются.
 *
 * ОТЛИЧИЯ ОТ КАРТИНКИ. Их три, и все в нашу пользу: нет выравнивания строк
 * (образцы лежат подряд), нет переупорядочивания (звук выходит в том же
 * порядке, в каком вошёл), и ответ всегда один и тот же вид — чередующиеся
 * образцы. Зато есть своё: один элемент должен понимать несколько видов
 * записи, и какой именно расшифровщик заводить, решается по входному виду.
 *
 * ЧТО ОХВАЧЕНО. AAC и MP3 — на них держится почти всё сетевое. Служба
 * умеет ещё Vorbis и FLAC, но тем нужны отдельные заголовки потока, и
 * добавлять их стоит, увидев настоящий случай, а не наугад.
 */

#ifndef PACKAGE
#define PACKAGE "ohadec"
#endif

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiodecoder.h>

#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_averrors.h>

#include <string.h>

GST_DEBUG_CATEGORY_STATIC(ohadec_debug);
#define GST_CAT_DEFAULT ohadec_debug

#define GST_TYPE_OHADEC (gst_ohadec_get_type())
G_DECLARE_FINAL_TYPE(GstOhADec, gst_ohadec, GST, OHADEC, GstAudioDecoder)

typedef struct {
    uint32_t index;
    OH_AVBuffer* buffer;
} Slot;

struct _GstOhADec {
    GstAudioDecoder parent;

    OH_AVCodec* codec;

    GMutex lock;
    GCond cond;
    GQueue* freeIn;
    GQueue* readyOut;

    gboolean flushing;
    gboolean started;
    gboolean sawEos;
    gboolean outputSet;

    gint rate;
    gint channels;
    GstAudioFormat format;

    GstFlowReturn lastFlow;
};

G_DEFINE_TYPE(GstOhADec, gst_ohadec, GST_TYPE_AUDIO_DECODER)

/* Берём AAC в обоих обличьях и MP3. У AAC «adts» — это когда у каждого
   куска свой заголовок, а «raw» — когда вид записи описан отдельно, в
   codec_data; службе надо сказать, что именно ей дают. */
static GstStaticPadTemplate sinkTemplate = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/mpeg, mpegversion = (int) 4, "
            "stream-format = (string) { adts, raw }, "
            "rate = (int) [ 8000, 192000 ], channels = (int) [ 1, 8 ]; "
        "audio/mpeg, mpegversion = (int) 1, layer = (int) 3, "
            "rate = (int) [ 8000, 48000 ], channels = (int) [ 1, 2 ]"));

static GstStaticPadTemplate srcTemplate = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
        "format = (string) { S16LE, F32LE }, "
        "rate = (int) [ 8000, 192000 ], "
        "channels = (int) [ 1, 8 ], "
        "layout = (string) interleaved"));

static void outputLoop(GstOhADec* self);

// ============================================================================
//  Очереди
// ============================================================================

static void queuesClear(GstOhADec* self)
{
    Slot* s;
    while ((s = g_queue_pop_head(self->freeIn)))
        g_free(s);
    while ((s = g_queue_pop_head(self->readyOut)))
        g_free(s);
}

static Slot* takeFreeInput(GstOhADec* self)
{
    Slot* slot = NULL;

    g_mutex_lock(&self->lock);
    while (!self->flushing && g_queue_is_empty(self->freeIn))
        g_cond_wait(&self->cond, &self->lock);
    if (!self->flushing)
        slot = g_queue_pop_head(self->freeIn);
    g_mutex_unlock(&self->lock);

    return slot;
}

// ============================================================================
//  Отклики службы
// ============================================================================

static void onError(OH_AVCodec* codec, int32_t errorCode, void* userData)
{
    (void)codec;
    GstOhADec* self = GST_OHADEC(userData);
    GST_ERROR_OBJECT(self, "служба расшифровки сообщила об отказе: %d", errorCode);

    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);
}

static void onStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData)
{
    (void)codec;
    (void)format;
    GstOhADec* self = GST_OHADEC(userData);
    GST_INFO_OBJECT(self, "вид потока изменился");

    g_mutex_lock(&self->lock);
    self->outputSet = FALSE;
    g_mutex_unlock(&self->lock);
}

static void onNeedInputBuffer(OH_AVCodec* codec, uint32_t index,
                              OH_AVBuffer* buffer, void* userData)
{
    (void)codec;
    GstOhADec* self = GST_OHADEC(userData);

    Slot* slot = g_new0(Slot, 1);
    slot->index = index;
    slot->buffer = buffer;

    g_mutex_lock(&self->lock);
    g_queue_push_tail(self->freeIn, slot);
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);
}

static void onNewOutputBuffer(OH_AVCodec* codec, uint32_t index,
                              OH_AVBuffer* buffer, void* userData)
{
    (void)codec;
    GstOhADec* self = GST_OHADEC(userData);

    Slot* slot = g_new0(Slot, 1);
    slot->index = index;
    slot->buffer = buffer;

    g_mutex_lock(&self->lock);
    g_queue_push_tail(self->readyOut, slot);
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);
}

// ============================================================================
//  Заведение и остановка службы
// ============================================================================

static void stopCodec(GstOhADec* self)
{
    if (!self->codec)
        return;

    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);

    gst_pad_stop_task(GST_AUDIO_DECODER_SRC_PAD(self));

    if (self->started) {
        OH_AudioCodec_Stop(self->codec);
        self->started = FALSE;
    }
    OH_AudioCodec_Destroy(self->codec);
    self->codec = NULL;

    queuesClear(self);
}

/* Какой расшифровщик заводить — решает входной вид. */
static const char* mimeFromCaps(GstOhADec* self, GstStructure* s)
{
    gint version = 0;
    if (!gst_structure_get_int(s, "mpegversion", &version))
        return NULL;

    if (version == 4)
        return OH_AVCODEC_MIMETYPE_AUDIO_AAC;

    if (version == 1) {
        gint layer = 0;
        gst_structure_get_int(s, "layer", &layer);
        if (layer == 3)
            return OH_AVCODEC_MIMETYPE_AUDIO_MPEG;
    }

    GST_ERROR_OBJECT(self, "вид записи не поддержан: mpegversion=%d", version);
    return NULL;
}

static gboolean startCodec(GstOhADec* self, GstCaps* caps)
{
    GstStructure* s = gst_caps_get_structure(caps, 0);

    const char* mime = mimeFromCaps(self, s);
    if (!mime)
        return FALSE;

    gint rate = 0, channels = 0;
    gst_structure_get_int(s, "rate", &rate);
    gst_structure_get_int(s, "channels", &channels);
    if (rate <= 0 || channels <= 0) {
        GST_ERROR_OBJECT(self, "частота опроса или число каналов неизвестны");
        return FALSE;
    }

    self->codec = OH_AudioCodec_CreateByMime(mime, false);
    if (!self->codec) {
        GST_ERROR_OBJECT(self, "расшифровщик %s не создался", mime);
        return FALSE;
    }

    OH_AVCodecCallback callback;
    memset(&callback, 0, sizeof(callback));
    callback.onError = onError;
    callback.onStreamChanged = onStreamChanged;
    callback.onNeedInputBuffer = onNeedInputBuffer;
    callback.onNewOutputBuffer = onNewOutputBuffer;

    OH_AVErrCode ret = OH_AudioCodec_RegisterCallback(self->codec, callback, self);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "отклики не приняты: %d", ret);
        return FALSE;
    }

    OH_AVFormat* format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, rate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, channels);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_FORMAT, SAMPLE_S16LE);

    if (g_strcmp0(mime, OH_AVCODEC_MIMETYPE_AUDIO_AAC) == 0) {
        const gchar* streamFormat = gst_structure_get_string(s, "stream-format");
        gboolean isAdts = (g_strcmp0(streamFormat, "adts") == 0);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_AAC_IS_ADTS, isAdts ? 1 : 0);
        GST_INFO_OBJECT(self, "AAC в виде %s", isAdts ? "adts" : "raw");

        /* Когда заголовка при каждом куске нет, вид записи описан отдельно,
           в codec_data: без него расшифровщик не поймёт ни числа каналов,
           ни способа упаковки. */
        if (!isAdts) {
            const GValue* v = gst_structure_get_value(s, "codec_data");
            if (v && GST_VALUE_HOLDS_BUFFER(v)) {
                GstBuffer* cd = gst_value_get_buffer(v);
                GstMapInfo map;
                if (gst_buffer_map(cd, &map, GST_MAP_READ)) {
                    OH_AVFormat_SetBuffer(format, OH_MD_KEY_CODEC_CONFIG,
                        map.data, map.size);
                    gst_buffer_unmap(cd, &map);
                    GST_INFO_OBJECT(self, "описание вида записи передано, %"
                        G_GSIZE_FORMAT " байт", map.size);
                }
            } else {
                GST_WARNING_OBJECT(self, "AAC без заголовков и без описания —"
                    " расшифровщик, скорее всего, откажется");
            }
        }
    }

    ret = OH_AudioCodec_Configure(self->codec, format);
    OH_AVFormat_Destroy(format);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "настройка не принята: %d", ret);
        return FALSE;
    }

    ret = OH_AudioCodec_Prepare(self->codec);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "подготовка не прошла: %d", ret);
        return FALSE;
    }

    g_mutex_lock(&self->lock);
    self->flushing = FALSE;
    self->sawEos = FALSE;
    self->outputSet = FALSE;
    self->lastFlow = GST_FLOW_OK;
    self->rate = rate;
    self->channels = channels;
    self->format = GST_AUDIO_FORMAT_S16LE;
    g_mutex_unlock(&self->lock);

    ret = OH_AudioCodec_Start(self->codec);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "расшифровщик не пошёл: %d", ret);
        return FALSE;
    }
    self->started = TRUE;

    gst_pad_start_task(GST_AUDIO_DECODER_SRC_PAD(self),
        (GstTaskFunction)outputLoop, self, NULL);

    GST_INFO_OBJECT(self, "расшифровщик пошёл: %s, %d Гц, %d каналов",
        mime, rate, channels);
    return TRUE;
}

// ============================================================================
//  Отдача наружу
// ============================================================================

/* Служба вправе отдать не то, что мы просили: иные расшифровщики считают
   в дробных числах и не умеют иначе. Спрашиваем её прямо. */
static void readOutputFormat(GstOhADec* self)
{
    OH_AVFormat* format = OH_AudioCodec_GetOutputDescription(self->codec);
    if (!format)
        return;

    int32_t rate = 0, channels = 0, sampleFormat = -1;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, &rate);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, &channels);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_SAMPLE_FORMAT, &sampleFormat);

    if (rate > 0) self->rate = rate;
    if (channels > 0) self->channels = channels;
    if (sampleFormat == SAMPLE_F32LE)
        self->format = GST_AUDIO_FORMAT_F32LE;
    else
        self->format = GST_AUDIO_FORMAT_S16LE;

    GST_INFO_OBJECT(self, "вид выхода: %d Гц, %d каналов, образцы %s",
        self->rate, self->channels,
        self->format == GST_AUDIO_FORMAT_F32LE ? "дробные" : "целые");

    OH_AVFormat_Destroy(format);
}

static void outputLoop(GstOhADec* self)
{
    GstAudioDecoder* decoder = GST_AUDIO_DECODER(self);
    Slot* slot = NULL;

    g_mutex_lock(&self->lock);
    while (!self->flushing && g_queue_is_empty(self->readyOut))
        g_cond_wait(&self->cond, &self->lock);
    if (!self->flushing)
        slot = g_queue_pop_head(self->readyOut);
    g_mutex_unlock(&self->lock);

    if (!slot) {
        gst_pad_pause_task(GST_AUDIO_DECODER_SRC_PAD(self));
        return;
    }

    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    OH_AVBuffer_GetBufferAttr(slot->buffer, &attr);

    if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        GST_INFO_OBJECT(self, "расшифровано до конца");
        g_mutex_lock(&self->lock);
        self->sawEos = TRUE;
        g_cond_broadcast(&self->cond);
        g_mutex_unlock(&self->lock);
        OH_AudioCodec_FreeOutputBuffer(self->codec, slot->index);
        g_free(slot);
        gst_pad_pause_task(GST_AUDIO_DECODER_SRC_PAD(self));
        return;
    }

    GstBuffer* out = NULL;
    if (attr.size > 0) {
        uint8_t* src = OH_AVBuffer_GetAddr(slot->buffer);
        if (src) {
            out = gst_buffer_new_allocate(NULL, attr.size, NULL);
            gst_buffer_fill(out, 0, src + attr.offset, attr.size);
        }
    }

    OH_AudioCodec_FreeOutputBuffer(self->codec, slot->index);
    g_free(slot);

    GST_AUDIO_DECODER_STREAM_LOCK(decoder);

    if (!self->outputSet) {
        readOutputFormat(self);

        GstAudioInfo info;
        gst_audio_info_init(&info);
        gst_audio_info_set_format(&info, self->format, self->rate,
            self->channels, NULL);

        if (!gst_audio_decoder_set_output_format(decoder, &info)) {
            GST_ERROR_OBJECT(self, "вид выхода не согласован");
            GST_AUDIO_DECODER_STREAM_UNLOCK(decoder);
            if (out)
                gst_buffer_unref(out);
            gst_pad_pause_task(GST_AUDIO_DECODER_SRC_PAD(self));
            return;
        }
        self->outputSet = TRUE;
    }

    /* Единица здесь — не число образцов, а число входных кусков, которые
       этим ответом закрыты. У звука соответствие один к одному. */
    GstFlowReturn flow = gst_audio_decoder_finish_frame(decoder, out, 1);

    self->lastFlow = flow;
    GST_AUDIO_DECODER_STREAM_UNLOCK(decoder);

    if (flow != GST_FLOW_OK) {
        GST_INFO_OBJECT(self, "нижние по цепочке ответили: %s",
            gst_flow_get_name(flow));
        gst_pad_pause_task(GST_AUDIO_DECODER_SRC_PAD(self));
    }
}

// ============================================================================
//  Обязанности расшифровщика
// ============================================================================

static gboolean gstOhADecStart(GstAudioDecoder* decoder)
{
    GstOhADec* self = GST_OHADEC(decoder);
    self->flushing = FALSE;
    self->started = FALSE;
    self->lastFlow = GST_FLOW_OK;
    return TRUE;
}

static gboolean gstOhADecStop(GstAudioDecoder* decoder)
{
    stopCodec(GST_OHADEC(decoder));
    return TRUE;
}

static gboolean gstOhADecSetFormat(GstAudioDecoder* decoder, GstCaps* caps)
{
    GstOhADec* self = GST_OHADEC(decoder);

    /* Менять настройку на ходу служба не обещает — заводим заново. */
    stopCodec(self);
    return startCodec(self, caps);
}

static GstFlowReturn gstOhADecHandleFrame(GstAudioDecoder* decoder,
                                          GstBuffer* buffer)
{
    GstOhADec* self = GST_OHADEC(decoder);

    /* Пустой кусок означает «подбей остатки» — у нас это делает finish. */
    if (!buffer)
        return GST_FLOW_OK;

    if (!self->codec || !self->started)
        return GST_FLOW_NOT_NEGOTIATED;

    if (self->lastFlow != GST_FLOW_OK)
        return self->lastFlow;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_FLOW_ERROR;

    GST_AUDIO_DECODER_STREAM_UNLOCK(decoder);
    Slot* slot = takeFreeInput(self);
    GST_AUDIO_DECODER_STREAM_LOCK(decoder);

    if (!slot) {
        gst_buffer_unmap(buffer, &map);
        return GST_FLOW_FLUSHING;
    }

    gint32 capacity = OH_AVBuffer_GetCapacity(slot->buffer);
    if ((gsize)capacity < map.size) {
        GST_ERROR_OBJECT(self, "кусок не влезает: %" G_GSIZE_FORMAT
            " при месте %d", map.size, capacity);
        gst_buffer_unmap(buffer, &map);
        g_free(slot);
        return GST_FLOW_ERROR;
    }

    memcpy(OH_AVBuffer_GetAddr(slot->buffer), map.data, map.size);

    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.offset = 0;
    attr.size = (int32_t)map.size;
    attr.pts = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))
        ? (int64_t)(GST_BUFFER_PTS(buffer) / GST_USECOND) : 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(slot->buffer, &attr);

    gst_buffer_unmap(buffer, &map);

    OH_AVErrCode ret = OH_AudioCodec_PushInputBuffer(self->codec, slot->index);
    g_free(slot);

    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "кусок не принят: %d", ret);
        return GST_FLOW_ERROR;
    }

    return self->lastFlow;
}

/* Конец потока: шлём пустой кусок с пометкой и ждём, пока служба отдаст
   накопленное. У звука задержка меньше, чем у картинки, но она есть. */
static void gstOhADecFinish(GstAudioDecoder* decoder)
{
    GstOhADec* self = GST_OHADEC(decoder);

    if (!self->codec || !self->started)
        return;

    GST_AUDIO_DECODER_STREAM_UNLOCK(decoder);

    Slot* slot = takeFreeInput(self);
    if (slot) {
        OH_AVCodecBufferAttr attr;
        memset(&attr, 0, sizeof(attr));
        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
        OH_AVBuffer_SetBufferAttr(slot->buffer, &attr);
        OH_AudioCodec_PushInputBuffer(self->codec, slot->index);
        g_free(slot);

        gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        g_mutex_lock(&self->lock);
        while (!self->sawEos && !self->flushing) {
            if (!g_cond_wait_until(&self->cond, &self->lock, deadline)) {
                GST_WARNING_OBJECT(self, "конца потока не дождались");
                break;
            }
        }
        g_mutex_unlock(&self->lock);
    }

    GST_AUDIO_DECODER_STREAM_LOCK(decoder);
}

static void gstOhADecFlush(GstAudioDecoder* decoder, gboolean hard)
{
    GstOhADec* self = GST_OHADEC(decoder);
    (void)hard;

    if (!self->codec || !self->started)
        return;

    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);

    gst_pad_stop_task(GST_AUDIO_DECODER_SRC_PAD(self));

    OH_AudioCodec_Flush(self->codec);
    queuesClear(self);

    g_mutex_lock(&self->lock);
    self->flushing = FALSE;
    self->sawEos = FALSE;
    self->lastFlow = GST_FLOW_OK;
    g_mutex_unlock(&self->lock);

    OH_AudioCodec_Start(self->codec);
    gst_pad_start_task(GST_AUDIO_DECODER_SRC_PAD(self),
        (GstTaskFunction)outputLoop, self, NULL);
}

// ============================================================================
//  Заведение вида
// ============================================================================

static void gstOhADecFinalize(GObject* object)
{
    GstOhADec* self = GST_OHADEC(object);

    queuesClear(self);
    g_queue_free(self->freeIn);
    g_queue_free(self->readyOut);
    g_mutex_clear(&self->lock);
    g_cond_clear(&self->cond);

    G_OBJECT_CLASS(gst_ohadec_parent_class)->finalize(object);
}

static void gst_ohadec_init(GstOhADec* self)
{
    g_mutex_init(&self->lock);
    g_cond_init(&self->cond);
    self->freeIn = g_queue_new();
    self->readyOut = g_queue_new();

    gst_audio_decoder_set_needs_format(GST_AUDIO_DECODER(self), TRUE);
    gst_audio_decoder_set_use_default_pad_acceptcaps(GST_AUDIO_DECODER(self), TRUE);
    GST_PAD_SET_ACCEPT_TEMPLATE(GST_AUDIO_DECODER_SINK_PAD(self));
}

static void gst_ohadec_class_init(GstOhADecClass* klass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(klass);
    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);
    GstAudioDecoderClass* decoderClass = GST_AUDIO_DECODER_CLASS(klass);

    objectClass->finalize = gstOhADecFinalize;

    gst_element_class_add_static_pad_template(elementClass, &sinkTemplate);
    gst_element_class_add_static_pad_template(elementClass, &srcTemplate);
    gst_element_class_set_static_metadata(elementClass,
        "OpenHarmony audio decoder", "Codec/Decoder/Audio",
        "Расшифровка звука через службу расшифровки OpenHarmony",
        "WPE на OpenHarmony");

    decoderClass->start = gstOhADecStart;
    decoderClass->stop = gstOhADecStop;
    decoderClass->set_format = gstOhADecSetFormat;
    decoderClass->handle_frame = gstOhADecHandleFrame;
    decoderClass->flush = gstOhADecFlush;
    decoderClass->finish = gstOhADecFinish;
}

// ============================================================================
//  Объявление себя
// ============================================================================

static gboolean pluginInit(GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT(ohadec_debug, "ohadec", 0,
        "расшифровка звука через OpenHarmony");

    return gst_element_register(plugin, "ohadec", GST_RANK_PRIMARY + 10,
        GST_TYPE_OHADEC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ohadec,
    "Расшифровка звука через службу расшифровки OpenHarmony",
    pluginInit, "1.0", "LGPL", "wpe-ohos", "https://gitee.com/openharmony")
