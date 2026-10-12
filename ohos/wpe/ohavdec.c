/*
 * Расшифровщик картинки для GStreamer поверх службы расшифровки OpenHarmony.
 *
 * Кладётся в /system/lib64/gstreamer-1.0/libgstohavdec.so и подхватывается
 * сам. Имя части — ohavdec_h264.
 *
 * ЗАЧЕМ. Разбирать запись нам есть чем (qtdemux, h264parse), а расшифровать
 * нечем: цепочка рассыпалась сразу после разборщиков. Брать ffmpeg отдельным
 * набором было бы проще, но неправильно — у системы есть своя служба
 * расшифровки, и обращаться к ней надо через её свод вызовов. Тогда всё, что
 * система умеет (а в будущем и ускоритель, если он появится), достаётся нам
 * даром.
 *
 * УСТРОЙСТВО. Главная тонкость — опять направление, только теперь в обе
 * стороны. GStreamer зовёт handle_frame, когда у него есть кадр, и ждёт
 * ответа. Служба наоборот сама сообщает откликами: «вот пустое место, клади»
 * и «вот расшифрованный кадр, забирай». Отклики приходят в её собственной
 * нити, не в нашей.
 *
 * Поэтому между ними две очереди:
 *   свободные места  — наполняется откликом, опустошается в handle_frame;
 *   готовые кадры    — наполняется откликом, опустошается отдельной нитью.
 *
 * Отдельная нить нужна потому, что отдавать кадр вниз по цепочке из чужого
 * отклика нельзя: там своя блокировка и свои сроки, и задержка в отклике
 * останавливает расшифровку целиком. Нить запускается на исходящем выводе —
 * так же, как это сделано у расшифровщиков, работающих со службами в других
 * сборках GStreamer.
 *
 * ПРО ВИД ВХОДА. Берём только byte-stream с выравниванием по кадру: так
 * заголовки последовательности идут внутри самого потока, и возиться с
 * отдельным codec_data не приходится. decodebin сам поставит перед нами
 * h264parse, который это обеспечит.
 */

#ifndef PACKAGE
#define PACKAGE "ohavdec"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>

#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_averrors.h>

#include <string.h>

GST_DEBUG_CATEGORY_STATIC(ohavdec_debug);
#define GST_CAT_DEFAULT ohavdec_debug

#define GST_TYPE_OHAVDEC (gst_ohavdec_get_type())
G_DECLARE_FINAL_TYPE(GstOhAvDec, gst_ohavdec, GST, OHAVDEC, GstVideoDecoder)

/* Место в очереди: служба говорит о себе числом, а память даёт отдельно. */
typedef struct {
    uint32_t index;
    OH_AVBuffer* buffer;
} Slot;

struct _GstOhAvDec {
    GstVideoDecoder parent;

    OH_AVCodec* codec;
    GstVideoCodecState* inputState;

    GMutex lock;
    GCond cond;
    GQueue* freeIn;    /* Slot*: пустые места под кадры внутрь */
    GQueue* readyOut;  /* Slot*: расшифрованные кадры наружу */

    gboolean flushing;  /* сбрасываем: ждущие расходятся, новое не берём */
    gboolean started;   /* служба запущена */
    gboolean sawEos;    /* пришёл последний кадр */
    gboolean outputSet; /* вид выхода уже согласован с нижними по цепочке */

    gint width;
    gint height;
    gint stride;       /* длина строки в памяти, может быть больше ширины */
    gint sliceHeight;  /* высота яркостной части, может быть больше высоты */

    GstFlowReturn lastFlow; /* чем ответили нижние по цепочке */
};

G_DEFINE_TYPE(GstOhAvDec, gst_ohavdec, GST_TYPE_VIDEO_DECODER)

static GstStaticPadTemplate sinkTemplate = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-h264, "
        "stream-format = (string) byte-stream, "
        "alignment = (string) au"));

static GstStaticPadTemplate srcTemplate = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format = (string) NV12"));

static void outputLoop(GstOhAvDec* self);

// ============================================================================
//  Очереди
// ============================================================================

static void slotFree(gpointer p)
{
    g_free(p);
}

static void queuesClear(GstOhAvDec* self)
{
    Slot* s;
    while ((s = g_queue_pop_head(self->freeIn)))
        slotFree(s);
    while ((s = g_queue_pop_head(self->readyOut)))
        slotFree(s);
}

/* Дождаться свободного места под кадр. Возвращает NULL при сбросе. */
static Slot* takeFreeInput(GstOhAvDec* self)
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
    GstOhAvDec* self = GST_OHAVDEC(userData);
    GST_ERROR_OBJECT(self, "служба расшифровки сообщила об отказе: %d", errorCode);

    /* Распускаем всех ждущих: иначе handle_frame будет ждать места,
       которого уже никто не даст. */
    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);
}

static void onStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData)
{
    (void)codec;
    GstOhAvDec* self = GST_OHAVDEC(userData);

    int32_t w = 0, h = 0;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h);
    GST_INFO_OBJECT(self, "вид потока изменился: %dx%d", w, h);

    /* Пересогласуем при следующем кадре. */
    g_mutex_lock(&self->lock);
    self->outputSet = FALSE;
    g_mutex_unlock(&self->lock);
}

static void onNeedInputBuffer(OH_AVCodec* codec, uint32_t index,
                              OH_AVBuffer* buffer, void* userData)
{
    (void)codec;
    GstOhAvDec* self = GST_OHAVDEC(userData);

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
    GstOhAvDec* self = GST_OHAVDEC(userData);

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

static void stopCodec(GstOhAvDec* self)
{
    if (!self->codec)
        return;

    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);

    gst_pad_stop_task(GST_VIDEO_DECODER_SRC_PAD(self));

    if (self->started) {
        OH_VideoDecoder_Stop(self->codec);
        self->started = FALSE;
    }
    OH_VideoDecoder_Destroy(self->codec);
    self->codec = NULL;

    queuesClear(self);
}

static gboolean startCodec(GstOhAvDec* self, gint width, gint height,
                           gint fpsNum, gint fpsDen)
{
    self->codec = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (!self->codec) {
        GST_ERROR_OBJECT(self, "расшифровщик H.264 не создался");
        return FALSE;
    }

    OH_AVCodecCallback callback;
    memset(&callback, 0, sizeof(callback));
    callback.onError = onError;
    callback.onStreamChanged = onStreamChanged;
    callback.onNeedInputBuffer = onNeedInputBuffer;
    callback.onNewOutputBuffer = onNewOutputBuffer;

    OH_AVErrCode ret = OH_VideoDecoder_RegisterCallback(self->codec, callback, self);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "отклики не приняты: %d", ret);
        return FALSE;
    }

    OH_AVFormat* format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    if (fpsDen > 0)
        OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE,
            (double)fpsNum / (double)fpsDen);

    ret = OH_VideoDecoder_Configure(self->codec, format);
    OH_AVFormat_Destroy(format);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "настройка не принята: %d", ret);
        return FALSE;
    }

    ret = OH_VideoDecoder_Prepare(self->codec);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "подготовка не прошла: %d", ret);
        return FALSE;
    }

    g_mutex_lock(&self->lock);
    self->flushing = FALSE;
    self->sawEos = FALSE;
    self->outputSet = FALSE;
    self->lastFlow = GST_FLOW_OK;
    self->width = width;
    self->height = height;
    self->stride = width;
    self->sliceHeight = height;
    g_mutex_unlock(&self->lock);

    ret = OH_VideoDecoder_Start(self->codec);
    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "расшифровщик не пошёл: %d", ret);
        return FALSE;
    }
    self->started = TRUE;

    gst_pad_start_task(GST_VIDEO_DECODER_SRC_PAD(self),
        (GstTaskFunction)outputLoop, self, NULL);

    GST_INFO_OBJECT(self, "расшифровщик пошёл: %dx%d", width, height);
    return TRUE;
}

// ============================================================================
//  Отдача кадров вниз по цепочке
// ============================================================================

/* Размеры в памяти могут отличаться от размеров картинки: служба выравнивает
   строки и высоту под свои нужды. Спрашиваем её об этом, иначе кадр
   развалится на косые полосы. */
static void readOutputGeometry(GstOhAvDec* self)
{
    OH_AVFormat* format = OH_VideoDecoder_GetOutputDescription(self->codec);
    if (!format)
        return;

    int32_t w = 0, h = 0, stride = 0, slice = 0;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_STRIDE, &stride);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &slice);

    if (w > 0) self->width = w;
    if (h > 0) self->height = h;
    self->stride = stride > 0 ? stride : self->width;
    self->sliceHeight = slice > 0 ? slice : self->height;

    GST_INFO_OBJECT(self, "вид выхода: %dx%d, строка %d, высота яркости %d",
        self->width, self->height, self->stride, self->sliceHeight);

    OH_AVFormat_Destroy(format);
}

/* Найти кадр, которому принадлежит расшифрованное. Служба отдаёт кадры в
   порядке показа, а не прихода, поэтому сверяем по времени; если не нашли —
   берём самый старый из ждущих. */
static GstVideoCodecFrame* findFrame(GstOhAvDec* self, int64_t ptsUs)
{
    GList* frames = gst_video_decoder_get_frames(GST_VIDEO_DECODER(self));
    GstVideoCodecFrame* found = NULL;

    for (GList* l = frames; l; l = l->next) {
        GstVideoCodecFrame* f = l->data;
        if (GST_CLOCK_TIME_IS_VALID(f->pts)
            && (int64_t)(f->pts / GST_USECOND) == ptsUs) {
            found = gst_video_codec_frame_ref(f);
            break;
        }
    }
    g_list_free_full(frames, (GDestroyNotify)gst_video_codec_frame_unref);

    if (!found)
        found = gst_video_decoder_get_oldest_frame(GST_VIDEO_DECODER(self));

    return found;
}

/* Переложить NV12 из памяти службы в кадр GStreamer. Копируем построчно:
   длина строки у службы своя, и разом переложить нельзя. */
static GstBuffer* copyFrameData(GstOhAvDec* self, OH_AVBuffer* buffer)
{
    uint8_t* src = OH_AVBuffer_GetAddr(buffer);
    if (!src) {
        GST_ERROR_OBJECT(self, "у расшифрованного кадра нет памяти");
        return NULL;
    }

    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, self->width, self->height);

    GstBuffer* out = gst_buffer_new_allocate(NULL, GST_VIDEO_INFO_SIZE(&info), NULL);
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, out, GST_MAP_WRITE)) {
        gst_buffer_unref(out);
        return NULL;
    }

    /* Яркостная часть. */
    guint8* dst = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
    gint dstStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    for (gint y = 0; y < self->height; y++)
        memcpy(dst + y * dstStride, src + y * self->stride, self->width);

    /* Цветовая часть: вдвое ниже, строка той же длины, лежит следом за
       яркостной — но отступ считается от выровненной высоты, а не от
       настоящей. Это и есть то место, где ошибаются чаще всего. */
    uint8_t* srcChroma = src + (size_t)self->stride * self->sliceHeight;
    dst = GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);
    dstStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
    for (gint y = 0; y < self->height / 2; y++)
        memcpy(dst + y * dstStride, srcChroma + y * self->stride, self->width);

    gst_video_frame_unmap(&frame);
    return out;
}

static void outputLoop(GstOhAvDec* self)
{
    GstVideoDecoder* decoder = GST_VIDEO_DECODER(self);
    Slot* slot = NULL;

    g_mutex_lock(&self->lock);
    while (!self->flushing && g_queue_is_empty(self->readyOut))
        g_cond_wait(&self->cond, &self->lock);
    if (!self->flushing)
        slot = g_queue_pop_head(self->readyOut);
    g_mutex_unlock(&self->lock);

    if (!slot) {
        gst_pad_pause_task(GST_VIDEO_DECODER_SRC_PAD(self));
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
        OH_VideoDecoder_FreeOutputBuffer(self->codec, slot->index);
        slotFree(slot);
        gst_pad_pause_task(GST_VIDEO_DECODER_SRC_PAD(self));
        return;
    }

    GST_VIDEO_DECODER_STREAM_LOCK(decoder);

    if (!self->outputSet) {
        readOutputGeometry(self);

        GstVideoCodecState* state = gst_video_decoder_set_output_state(decoder,
            GST_VIDEO_FORMAT_NV12, self->width, self->height, self->inputState);
        gst_video_codec_state_unref(state);

        if (!gst_video_decoder_negotiate(decoder)) {
            GST_ERROR_OBJECT(self, "вид выхода не согласован");
            GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
            OH_VideoDecoder_FreeOutputBuffer(self->codec, slot->index);
            slotFree(slot);
            gst_pad_pause_task(GST_VIDEO_DECODER_SRC_PAD(self));
            return;
        }
        self->outputSet = TRUE;
    }

    GstBuffer* out = (attr.size > 0) ? copyFrameData(self, slot->buffer) : NULL;

    /* Память службы освобождаем сразу после перекладывания: её немного, и
       держать её, пока кадр идёт вниз по цепочке, значит остановить
       расшифровку. */
    OH_VideoDecoder_FreeOutputBuffer(self->codec, slot->index);

    GstVideoCodecFrame* frame = findFrame(self, attr.pts);
    GstFlowReturn flow = GST_FLOW_OK;

    if (frame) {
        if (out) {
            frame->output_buffer = out;
            flow = gst_video_decoder_finish_frame(decoder, frame);
        } else {
            flow = gst_video_decoder_drop_frame(decoder, frame);
        }
    } else {
        if (out)
            gst_buffer_unref(out);
        GST_WARNING_OBJECT(self, "кадру неоткуда взяться: время %" G_GINT64_FORMAT, (gint64)attr.pts);
    }

    self->lastFlow = flow;
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);

    slotFree(slot);

    if (flow != GST_FLOW_OK) {
        GST_INFO_OBJECT(self, "нижние по цепочке ответили: %s", gst_flow_get_name(flow));
        gst_pad_pause_task(GST_VIDEO_DECODER_SRC_PAD(self));
    }
}

// ============================================================================
//  Обязанности расшифровщика
// ============================================================================

static gboolean gstOhAvDecStart(GstVideoDecoder* decoder)
{
    GstOhAvDec* self = GST_OHAVDEC(decoder);
    self->flushing = FALSE;
    self->started = FALSE;
    self->lastFlow = GST_FLOW_OK;
    return TRUE;
}

static gboolean gstOhAvDecStop(GstVideoDecoder* decoder)
{
    GstOhAvDec* self = GST_OHAVDEC(decoder);

    stopCodec(self);
    g_clear_pointer(&self->inputState, gst_video_codec_state_unref);
    return TRUE;
}

static gboolean gstOhAvDecSetFormat(GstVideoDecoder* decoder, GstVideoCodecState* state)
{
    GstOhAvDec* self = GST_OHAVDEC(decoder);
    GstVideoInfo* info = &state->info;

    gint width = GST_VIDEO_INFO_WIDTH(info);
    gint height = GST_VIDEO_INFO_HEIGHT(info);
    if (width <= 0 || height <= 0) {
        GST_ERROR_OBJECT(self, "размер картинки неизвестен — нечего настраивать");
        return FALSE;
    }

    /* Если уже работаем, начинаем заново: менять настройку на ходу служба
       не обещает, а пересоздание стоит недорого и случается редко. */
    stopCodec(self);

    g_clear_pointer(&self->inputState, gst_video_codec_state_unref);
    self->inputState = gst_video_codec_state_ref(state);

    return startCodec(self, width, height,
        GST_VIDEO_INFO_FPS_N(info), GST_VIDEO_INFO_FPS_D(info));
}

static GstFlowReturn gstOhAvDecHandleFrame(GstVideoDecoder* decoder,
                                           GstVideoCodecFrame* frame)
{
    GstOhAvDec* self = GST_OHAVDEC(decoder);

    if (!self->codec || !self->started) {
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_NOT_NEGOTIATED;
    }

    if (self->lastFlow != GST_FLOW_OK) {
        GstFlowReturn flow = self->lastFlow;
        gst_video_codec_frame_unref(frame);
        return flow;
    }

    GstMapInfo map;
    if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ)) {
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_ERROR;
    }

    /* Пока ждём свободного места, держать общую блокировку нельзя: её же
       берёт нить отдачи, а без её работы места и не освободятся. */
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    Slot* slot = takeFreeInput(self);
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);

    if (!slot) {
        gst_buffer_unmap(frame->input_buffer, &map);
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_FLUSHING;
    }

    gint32 capacity = OH_AVBuffer_GetCapacity(slot->buffer);
    if ((gsize)capacity < map.size) {
        GST_ERROR_OBJECT(self, "кадр не влезает: %" G_GSIZE_FORMAT " при месте %d",
            map.size, capacity);
        gst_buffer_unmap(frame->input_buffer, &map);
        slotFree(slot);
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_ERROR;
    }

    memcpy(OH_AVBuffer_GetAddr(slot->buffer), map.data, map.size);

    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.offset = 0;
    attr.size = (int32_t)map.size;
    attr.pts = GST_CLOCK_TIME_IS_VALID(frame->pts)
        ? (int64_t)(frame->pts / GST_USECOND) : 0;
    attr.flags = GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame)
        ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(slot->buffer, &attr);

    gst_buffer_unmap(frame->input_buffer, &map);

    OH_AVErrCode ret = OH_VideoDecoder_PushInputBuffer(self->codec, slot->index);
    slotFree(slot);
    gst_video_codec_frame_unref(frame);

    if (ret != AV_ERR_OK) {
        GST_ERROR_OBJECT(self, "кадр не принят: %d", ret);
        return GST_FLOW_ERROR;
    }

    return self->lastFlow;
}

/* Конец потока: отправляем пустой кадр с пометкой и ждём, пока служба
   выдаст всё, что у неё накопилось. Без этого пропадают последние кадры:
   расшифровщик держит их для переупорядочивания. */
static GstFlowReturn gstOhAvDecFinish(GstVideoDecoder* decoder)
{
    GstOhAvDec* self = GST_OHAVDEC(decoder);

    if (!self->codec || !self->started)
        return GST_FLOW_OK;

    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);

    Slot* slot = takeFreeInput(self);
    if (slot) {
        OH_AVCodecBufferAttr attr;
        memset(&attr, 0, sizeof(attr));
        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
        OH_AVBuffer_SetBufferAttr(slot->buffer, &attr);
        OH_VideoDecoder_PushInputBuffer(self->codec, slot->index);
        slotFree(slot);

        gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
        g_mutex_lock(&self->lock);
        while (!self->sawEos && !self->flushing) {
            if (!g_cond_wait_until(&self->cond, &self->lock, deadline)) {
                GST_WARNING_OBJECT(self, "конца потока не дождались");
                break;
            }
        }
        g_mutex_unlock(&self->lock);
    }

    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
    return GST_FLOW_OK;
}

static gboolean gstOhAvDecFlush(GstVideoDecoder* decoder)
{
    GstOhAvDec* self = GST_OHAVDEC(decoder);

    if (!self->codec || !self->started)
        return TRUE;

    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->lock);

    gst_pad_stop_task(GST_VIDEO_DECODER_SRC_PAD(self));

    OH_VideoDecoder_Flush(self->codec);
    queuesClear(self);

    g_mutex_lock(&self->lock);
    self->flushing = FALSE;
    self->sawEos = FALSE;
    self->lastFlow = GST_FLOW_OK;
    g_mutex_unlock(&self->lock);

    OH_VideoDecoder_Start(self->codec);
    gst_pad_start_task(GST_VIDEO_DECODER_SRC_PAD(self),
        (GstTaskFunction)outputLoop, self, NULL);

    return TRUE;
}

// ============================================================================
//  Заведение вида
// ============================================================================

static void gstOhAvDecFinalize(GObject* object)
{
    GstOhAvDec* self = GST_OHAVDEC(object);

    queuesClear(self);
    g_queue_free(self->freeIn);
    g_queue_free(self->readyOut);
    g_mutex_clear(&self->lock);
    g_cond_clear(&self->cond);

    G_OBJECT_CLASS(gst_ohavdec_parent_class)->finalize(object);
}

static void gst_ohavdec_init(GstOhAvDec* self)
{
    g_mutex_init(&self->lock);
    g_cond_init(&self->cond);
    self->freeIn = g_queue_new();
    self->readyOut = g_queue_new();

    gst_video_decoder_set_packetized(GST_VIDEO_DECODER(self), TRUE);
    gst_video_decoder_set_needs_format(GST_VIDEO_DECODER(self), TRUE);
}

static void gst_ohavdec_class_init(GstOhAvDecClass* klass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(klass);
    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);
    GstVideoDecoderClass* decoderClass = GST_VIDEO_DECODER_CLASS(klass);

    objectClass->finalize = gstOhAvDecFinalize;

    gst_element_class_add_static_pad_template(elementClass, &sinkTemplate);
    gst_element_class_add_static_pad_template(elementClass, &srcTemplate);
    gst_element_class_set_static_metadata(elementClass,
        "OpenHarmony H.264 decoder", "Codec/Decoder/Video",
        "Расшифровка картинки через службу расшифровки OpenHarmony",
        "WPE на OpenHarmony");

    decoderClass->start = gstOhAvDecStart;
    decoderClass->stop = gstOhAvDecStop;
    decoderClass->set_format = gstOhAvDecSetFormat;
    decoderClass->handle_frame = gstOhAvDecHandleFrame;
    decoderClass->finish = gstOhAvDecFinish;
    decoderClass->flush = gstOhAvDecFlush;
}

// ============================================================================
//  Объявление себя
// ============================================================================

static gboolean pluginInit(GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT(ohavdec_debug, "ohavdec", 0,
        "расшифровка картинки через OpenHarmony");

    return gst_element_register(plugin, "ohavdec_h264", GST_RANK_PRIMARY + 10,
        GST_TYPE_OHAVDEC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ohavdec,
    "Расшифровка картинки через службу расшифровки OpenHarmony",
    pluginInit, "1.0", "LGPL", "wpe-ohos", "https://gitee.com/openharmony")
