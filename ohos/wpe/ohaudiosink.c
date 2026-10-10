/*
 * Приёмник звука GStreamer поверх звуковой подсистемы OpenHarmony.
 *
 * Кладётся в /system/lib64/gstreamer-1.0/libgstohaudio.so и подхватывается
 * сам. Имя части — ohaudiosink; вес выставлен высоким, так что autoaudiosink
 * выберет её прежде alsasink.
 *
 * ЗАЧЕМ. Движок просит приёмник звука по имени autoaudiosink, а тот
 * подбирает подходящий. До сих пор подходящим был alsasink — вывод мимо
 * системы, напрямую в звуковую карту. На настоящем устройстве так нельзя:
 * громкостью, перебиванием и выбором выхода заведует сама система, и
 * обращаться к ней надо через её свод вызовов.
 *
 * УСТРОЙСТВО. Главная тонкость — направление. GStreamer отдаёт звук сам,
 * когда тот готов: зовёт write и ждёт, пока мы примем. OpenHarmony наоборот
 * зовёт нас, когда ему нужен очередной кусок. Между этими двумя порядками
 * ставим кольцо: write кладёт в него и ждёт места, отклик забирает и ждать
 * не может — если нечего отдать, отдаёт тишину, иначе собьётся ход.
 *
 * Размер кольца берём из того, что задаёт GStreamer (segsize × segtotal),
 * но не меньше полусекунды: слишком тесное кольцо даёт треск на любой
 * заминке, слишком широкое — задержку.
 */

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiosink.h>

#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>

#include <string.h>

GST_DEBUG_CATEGORY_STATIC(ohaudiosink_debug);
#define GST_CAT_DEFAULT ohaudiosink_debug

#define GST_TYPE_OHAUDIO_SINK (gst_ohaudio_sink_get_type())
G_DECLARE_FINAL_TYPE(GstOhAudioSink, gst_ohaudio_sink, GST, OHAUDIO_SINK, GstAudioSink)

struct _GstOhAudioSink {
    GstAudioSink parent;

    OH_AudioStreamBuilder* builder;
    OH_AudioRenderer* renderer;

    /* Кольцо между нашей записью и их запросами. */
    GMutex lock;
    GCond cond;
    guint8* ring;
    gsize ringSize;
    gsize filled;      /* сколько в кольце непрочитанного */
    gsize readPos;     /* откуда читать */
    gsize writePos;    /* куда писать */

    gboolean flushing; /* сбрасываем: write не ждёт, отклик отдаёт тишину */
    gboolean running;

    gint bytesPerFrame;
    gint rate;
};

G_DEFINE_TYPE(GstOhAudioSink, gst_ohaudio_sink, GST_TYPE_AUDIO_SINK)

/* Виды звука, которые умеем отдавать как есть, без перекладывания. */
static GstStaticPadTemplate sinkTemplate = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
        "format = (string) { S16LE, F32LE }, "
        "rate = (int) [ 8000, 192000 ], "
        "channels = (int) [ 1, 8 ], "
        "layout = (string) interleaved"));

// ============================================================================
//  Кольцо
// ============================================================================

static void ringReset(GstOhAudioSink* self)
{
    self->filled = 0;
    self->readPos = 0;
    self->writePos = 0;
}

/* Забрать из кольца сколько получится. Зовётся из отклика звуковой
   подсистемы, поэтому ждать здесь нельзя ни при каких обстоятельствах. */
static gsize ringTake(GstOhAudioSink* self, guint8* out, gsize want)
{
    gsize taken = 0;

    g_mutex_lock(&self->lock);
    while (taken < want && self->filled > 0) {
        gsize chunk = MIN(want - taken, self->ringSize - self->readPos);
        chunk = MIN(chunk, self->filled);
        memcpy(out + taken, self->ring + self->readPos, chunk);
        self->readPos = (self->readPos + chunk) % self->ringSize;
        self->filled -= chunk;
        taken += chunk;
    }
    /* Место освободилось — тот, кто ждал в write, может продолжить. */
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);

    return taken;
}

// ============================================================================
//  Отклик звуковой подсистемы
// ============================================================================

static int32_t onWriteData(OH_AudioRenderer* renderer, void* userData,
                           void* buffer, int32_t length)
{
    (void)renderer;
    GstOhAudioSink* self = GST_OHAUDIO_SINK(userData);

    gsize taken = ringTake(self, (guint8*)buffer, (gsize)length);

    /* Недостачу добиваем тишиной. Отдать меньше просьбы нельзя: подсистема
       проиграет остаток буфера как есть, и выйдет щелчок из мусора. */
    if (taken < (gsize)length) {
        memset((guint8*)buffer + taken, 0, (gsize)length - taken);
        GST_LOG_OBJECT(self, "недобор: просили %d, отдали %" G_GSIZE_FORMAT,
            length, taken);
    }
    return 0;
}

// ============================================================================
//  Обязанности приёмника
// ============================================================================

static gboolean gstOhAudioSinkOpen(GstAudioSink* sink)
{
    /* Открывать нечего: подсистема заводится вместе с потоком, а поток
       нельзя завести, не зная вида звука. Всё происходит в prepare. */
    (void)sink;
    return TRUE;
}

static gboolean gstOhAudioSinkClose(GstAudioSink* sink)
{
    (void)sink;
    return TRUE;
}

static gboolean gstOhAudioSinkPrepare(GstAudioSink* sink, GstAudioRingBufferSpec* spec)
{
    GstOhAudioSink* self = GST_OHAUDIO_SINK(sink);
    const GstAudioInfo* info = &spec->info;

    OH_AudioStream_SampleFormat format;
    switch (GST_AUDIO_INFO_FORMAT(info)) {
    case GST_AUDIO_FORMAT_S16LE: format = AUDIOSTREAM_SAMPLE_S16LE; break;
    case GST_AUDIO_FORMAT_F32LE: format = AUDIOSTREAM_SAMPLE_F32LE; break;
    default:
        GST_ERROR_OBJECT(self, "вид звука не поддержан: %s",
            GST_AUDIO_INFO_NAME(info));
        return FALSE;
    }

    self->bytesPerFrame = GST_AUDIO_INFO_BPF(info);
    self->rate = GST_AUDIO_INFO_RATE(info);

    /* Кольцо: не меньше полусекунды, но не меньше того, что просит
       GStreamer. Тесное даёт треск на заминках, широкое — задержку. */
    gsize wanted = (gsize)spec->segsize * spec->segtotal;
    gsize halfSecond = (gsize)self->rate * self->bytesPerFrame / 2;
    self->ringSize = MAX(wanted, halfSecond);
    self->ring = g_malloc(self->ringSize);
    ringReset(self);
    self->flushing = FALSE;

    OH_AudioStream_Result result =
        OH_AudioStreamBuilder_Create(&self->builder, AUDIOSTREAM_TYPE_RENDERER);
    if (result != AUDIOSTREAM_SUCCESS) {
        GST_ERROR_OBJECT(self, "не завёлся сборщик потока: %d", result);
        return FALSE;
    }

    OH_AudioStreamBuilder_SetSamplingRate(self->builder, self->rate);
    OH_AudioStreamBuilder_SetChannelCount(self->builder, GST_AUDIO_INFO_CHANNELS(info));
    OH_AudioStreamBuilder_SetSampleFormat(self->builder, format);
    OH_AudioStreamBuilder_SetEncodingType(self->builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetRendererInfo(self->builder, AUDIOSTREAM_USAGE_MUSIC);

    OH_AudioRenderer_Callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.OH_AudioRenderer_OnWriteData = onWriteData;
    OH_AudioStreamBuilder_SetRendererCallback(self->builder, callbacks, self);

    result = OH_AudioStreamBuilder_GenerateRenderer(self->builder, &self->renderer);
    if (result != AUDIOSTREAM_SUCCESS) {
        GST_ERROR_OBJECT(self, "не создался проигрыватель: %d", result);
        return FALSE;
    }

    result = OH_AudioRenderer_Start(self->renderer);
    if (result != AUDIOSTREAM_SUCCESS) {
        GST_ERROR_OBJECT(self, "проигрыватель не пошёл: %d", result);
        return FALSE;
    }

    self->running = TRUE;
    GST_INFO_OBJECT(self, "готов: %d Гц, %d каналов, кольцо %" G_GSIZE_FORMAT " байт",
        self->rate, GST_AUDIO_INFO_CHANNELS(info), self->ringSize);
    return TRUE;
}

static gboolean gstOhAudioSinkUnprepare(GstAudioSink* sink)
{
    GstOhAudioSink* self = GST_OHAUDIO_SINK(sink);

    /* Сперва распускаем ждущих, иначе write останется висеть. */
    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);

    if (self->renderer) {
        OH_AudioRenderer_Stop(self->renderer);
        OH_AudioRenderer_Release(self->renderer);
        self->renderer = NULL;
    }
    if (self->builder) {
        OH_AudioStreamBuilder_Destroy(self->builder);
        self->builder = NULL;
    }

    g_clear_pointer(&self->ring, g_free);
    self->ringSize = 0;
    self->running = FALSE;
    return TRUE;
}

/* Положить в кольцо. Ждём, пока освободится место: это и есть та мера,
   которой GStreamer сдерживает подачу звука. */
static gint gstOhAudioSinkWrite(GstAudioSink* sink, gpointer data, guint length)
{
    GstOhAudioSink* self = GST_OHAUDIO_SINK(sink);
    const guint8* in = (const guint8*)data;
    gsize written = 0;

    g_mutex_lock(&self->lock);
    while (written < length) {
        while (self->filled == self->ringSize && !self->flushing)
            g_cond_wait(&self->cond, &self->lock);

        if (self->flushing) {
            g_mutex_unlock(&self->lock);
            return written > 0 ? (gint)written : (gint)length;
        }

        gsize free = self->ringSize - self->filled;
        gsize chunk = MIN((gsize)length - written, free);
        chunk = MIN(chunk, self->ringSize - self->writePos);

        memcpy(self->ring + self->writePos, in + written, chunk);
        self->writePos = (self->writePos + chunk) % self->ringSize;
        self->filled += chunk;
        written += chunk;
    }
    g_mutex_unlock(&self->lock);

    return (gint)written;
}

/* Сколько звука ещё не прозвучало — по этому GStreamer согласует время. */
static guint gstOhAudioSinkDelay(GstAudioSink* sink)
{
    GstOhAudioSink* self = GST_OHAUDIO_SINK(sink);
    guint frames;

    g_mutex_lock(&self->lock);
    frames = self->bytesPerFrame ? (guint)(self->filled / self->bytesPerFrame) : 0;
    g_mutex_unlock(&self->lock);

    return frames;
}

/* Сброс: выбросить накопленное и распустить ждущих. */
static void gstOhAudioSinkReset(GstAudioSink* sink)
{
    GstOhAudioSink* self = GST_OHAUDIO_SINK(sink);

    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    ringReset(self);
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);

    if (self->renderer)
        OH_AudioRenderer_Flush(self->renderer);

    /* Сброс кончился — дальше можно писать снова. */
    g_mutex_lock(&self->lock);
    self->flushing = FALSE;
    g_mutex_unlock(&self->lock);
}

// ============================================================================
//  Заведение вида
// ============================================================================

static void gstOhAudioSinkFinalize(GObject* object)
{
    GstOhAudioSink* self = GST_OHAUDIO_SINK(object);
    g_mutex_clear(&self->lock);
    g_cond_clear(&self->cond);
    g_free(self->ring);
    G_OBJECT_CLASS(gst_ohaudio_sink_parent_class)->finalize(object);
}

static void gst_ohaudio_sink_init(GstOhAudioSink* self)
{
    g_mutex_init(&self->lock);
    g_cond_init(&self->cond);
}

static void gst_ohaudio_sink_class_init(GstOhAudioSinkClass* klass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(klass);
    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass* sinkClass = GST_AUDIO_SINK_CLASS(klass);

    objectClass->finalize = gstOhAudioSinkFinalize;

    gst_element_class_add_static_pad_template(elementClass, &sinkTemplate);
    gst_element_class_set_static_metadata(elementClass,
        "OpenHarmony audio sink", "Sink/Audio",
        "Вывод звука через звуковую подсистему OpenHarmony",
        "WPE на OpenHarmony");

    sinkClass->open = gstOhAudioSinkOpen;
    sinkClass->close = gstOhAudioSinkClose;
    sinkClass->prepare = gstOhAudioSinkPrepare;
    sinkClass->unprepare = gstOhAudioSinkUnprepare;
    sinkClass->write = gstOhAudioSinkWrite;
    sinkClass->delay = gstOhAudioSinkDelay;
    sinkClass->reset = gstOhAudioSinkReset;
}

// ============================================================================
//  Объявление себя
// ============================================================================

static gboolean pluginInit(GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT(ohaudiosink_debug, "ohaudiosink", 0,
        "вывод звука через OpenHarmony");

    /* Вес выше обычного: autoaudiosink перебирает приёмники по весу,
       и наш должен оказаться прежде alsasink. */
    return gst_element_register(plugin, "ohaudiosink", GST_RANK_PRIMARY + 10,
        GST_TYPE_OHAUDIO_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ohaudio,
    "Вывод звука через звуковую подсистему OpenHarmony",
    pluginInit, "1.0", "LGPL", "wpe-ohos", "https://gitee.com/openharmony")
