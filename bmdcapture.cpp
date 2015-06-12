/*
 * Blackmagic Devices Decklink capture
 * Copyright (c) 2013 Luca Barbato.
 *
 * This file is part of bmdtools.
 *
 * bmdtools is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * bmdtools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with bmdtools; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

#include "compat.h"
#include "DeckLinkAPI.h"
#include "Capture.h"
#include "modes.h"
extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/time.h"
}

pthread_mutex_t sleepMutex;
pthread_cond_t sleepCond;
int videoOutputFile = -1;
int audioOutputFile = -1;

IDeckLink *deckLink;
IDeckLinkInput *deckLinkInput;
IDeckLinkDisplayModeIterator *displayModeIterator;
IDeckLinkDisplayMode *displayMode;
IDeckLinkConfiguration *deckLinkConfiguration;

static int g_videoModeIndex      = -1;
static int g_audioChannels       = 2;
static int g_audioSampleDepth    = 16;
const char *g_videoOutputFile    = NULL;
const char *g_audioOutputFile    = NULL;
static int g_maxFrames           = -1;
static int serial_fd             = -1;
static int wallclock             = 0;
static int draw_bars             = 1;
bool g_verbose                   = false;
unsigned long long g_memoryLimit = 1024 * 1024 * 1024;            // 1GByte(>50 sec)

static unsigned long frameCount = 0;
static unsigned int dropped     = 0, totaldropped = 0;
static enum PixelFormat pix_fmt = PIX_FMT_UYVY422;
static enum AVSampleFormat sample_fmt = AV_SAMPLE_FMT_S16;
typedef struct AVPacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    unsigned long long size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AVPacketQueue;

static AVPacketQueue queue;

static AVPacket flush_pkt;

static void avpacket_queue_init(AVPacketQueue *q)
{
    memset(q, 0, sizeof(AVPacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void avpacket_queue_flush(AVPacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt   = NULL;
    q->first_pkt  = NULL;
    q->nb_packets = 0;
    q->size       = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void avpacket_queue_end(AVPacketQueue *q)
{
    avpacket_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    /* duplicate the packet */
    if (pkt != &flush_pkt && av_dup_packet(pkt) < 0) {
        return -1;
    }

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1) {
        return -1;
    }
    pkt1->pkt  = *pkt;
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt) {
        q->first_pkt = pkt1;
    } else {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;; ) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            if (pkt1->pkt.data == flush_pkt.data) {
                ret = 0;
                break;
            }
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt     = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static unsigned long long avpacket_queue_size(AVPacketQueue *q)
{
    unsigned long long size;
    pthread_mutex_lock(&q->mutex);
    size = q->size;
    pthread_mutex_unlock(&q->mutex);
    return size;
}

AVOutputFormat *fmt = NULL;
AVFormatContext *oc;
AVStream *audio_st, *video_st, *data_st;
BMDTimeValue frameRateDuration, frameRateScale;

static AVStream *add_audio_stream(AVFormatContext *oc, enum AVCodecID codec_id)
{
    AVCodecContext *c;
    AVCodec *codec;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }

    c             = st->codec;
    c->codec_id   = codec_id;
    c->codec_type = AVMEDIA_TYPE_AUDIO;

    /* put sample parameters */
    c->sample_fmt = sample_fmt;
//    c->bit_rate = 64000;
    c->sample_rate = 48000;
    c->channels    = g_audioChannels;
    // some formats want stream headers to be separate
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    codec = avcodec_find_encoder(c->codec_id);
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        exit(1);
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
        exit(1);
    }

    return st;
}

static AVStream *add_video_stream(AVFormatContext *oc, enum AVCodecID codec_id)
{
    AVCodecContext *c;
    AVCodec *codec;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }

    c             = st->codec;
    c->codec_id   = codec_id;
    c->codec_type = AVMEDIA_TYPE_VIDEO;

    /* put sample parameters */
//    c->bit_rate = 400000;
    /* resolution must be a multiple of two */
    c->width  = displayMode->GetWidth();
    c->height = displayMode->GetHeight();
    /* time base: this is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. for fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identically 1.*/
    displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
    st->time_base.den = frameRateScale;
    st->time_base.num = frameRateDuration;
    c->pix_fmt       = pix_fmt;

    if (codec_id == AV_CODEC_ID_V210 || codec_id == AV_CODEC_ID_R210)
        c->bits_per_raw_sample = 10;
    if (codec_id == AV_CODEC_ID_RAWVIDEO)
        c->codec_tag = avcodec_pix_fmt_to_codec_tag(c->pix_fmt);
    // some formats want stream headers to be separate
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    /* find the video encoder */
    //codec = avcodec_find_encoder(c->codec_id);
    //if (!codec) {
    //    fprintf(stderr, "codec not found\n");
    //    exit(1);
    //}

    /* open the codec */
    // if (avcodec_open2(c, codec, NULL) < 0) {
    //    fprintf(stderr, "could not open codec\n");
    //    exit(1);
    //}

    return st;
}

static AVStream *add_data_stream(AVFormatContext *oc, enum AVCodecID codec_id)
{
    AVCodec *codec;
    AVCodecContext *c;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }

    c = st->codec;
    c->codec_id = codec_id;
    c->codec_type = AVMEDIA_TYPE_DATA;

    displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
    st->time_base.den = frameRateScale;
    st->time_base.num = frameRateDuration;

    // some formats want stream headers to be separate
    if(oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    /* find the video encoder */
    codec = (AVCodec*)av_malloc(sizeof(AVCodec));
    memset(codec, 0, sizeof(AVCodec));
    codec->id = c->codec_id;

    /* open the codec */
    c->codec = codec;

    return st;
}

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : m_refCount(0)
{
    pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
    pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
    pthread_mutex_lock(&m_mutex);
    m_refCount++;
    pthread_mutex_unlock(&m_mutex);

    return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
    pthread_mutex_lock(&m_mutex);
    m_refCount--;
    pthread_mutex_unlock(&m_mutex);

    if (m_refCount == 0) {
        delete this;
        return 0;
    }

    return (ULONG)m_refCount;
}

int64_t initial_video_pts = AV_NOPTS_VALUE;
int64_t initial_audio_pts = AV_NOPTS_VALUE;

static int no_video = 0;

#define CC_LINE 9


// FIXME fail properly.
void vanc_as_side_data(AVPacket *pkt,
                       IDeckLinkVideoInputFrame *frame)
{
    IDeckLinkVideoFrameAncillary *ancillary;
    void *buf;
    uint8_t *vanc;
    int len = frame->GetRowBytes();
    int ret = 0;

    ret = frame->GetAncillaryData(&ancillary);
    if (ret < 0)
        return;

    ret = ancillary->GetBufferForVerticalBlankingLine(CC_LINE, &buf);
    if (ret < 0)
        return;

    vanc = av_packet_new_side_data(pkt, AV_PKT_DATA_VANC, len);

    memcpy(vanc, buf, len);
}

void write_data_packet(char *data, int size, int64_t pts)
{
    AVPacket pkt;
    av_init_packet(&pkt);

    pkt.flags        |= AV_PKT_FLAG_KEY;
    pkt.stream_index  = data_st->index;
    pkt.data          = (uint8_t*)data;
    pkt.size          = size;
    pkt.dts = pkt.pts = pts;

    avpacket_queue_put(&queue, &pkt);
}

void write_audio_packet(IDeckLinkAudioInputPacket *audioFrame)
{
    AVCodecContext *c;
    AVPacket pkt;
    BMDTimeValue audio_pts;
    void *audioFrameBytes;

    av_init_packet(&pkt);

    c = audio_st->codec;
    //hack among hacks
    pkt.size = audioFrame->GetSampleFrameCount() *
               g_audioChannels * (g_audioSampleDepth / 8);
    audioFrame->GetBytes(&audioFrameBytes);
    audioFrame->GetPacketTime(&audio_pts, audio_st->time_base.den);
    pkt.pts = audio_pts / audio_st->time_base.num;

    if (initial_audio_pts == AV_NOPTS_VALUE) {
        initial_audio_pts = pkt.pts;
    }

    pkt.pts -= initial_audio_pts;
    pkt.dts = pkt.pts;

    pkt.flags       |= AV_PKT_FLAG_KEY;
    pkt.stream_index = audio_st->index;
    pkt.data         = (uint8_t *)audioFrameBytes;
    c->frame_number++;

    avpacket_queue_put(&queue, &pkt);
}

void write_video_packet(IDeckLinkVideoInputFrame *videoFrame,
                        int64_t pts, int64_t duration)
{
    AVPacket pkt;
    AVCodecContext *c;
    void *frameBytes;
    time_t cur_time;

    av_init_packet(&pkt);
    c = video_st->codec;
    if (g_verbose && frameCount % 25 == 0) {
        unsigned long long qsize = avpacket_queue_size(&queue);
        fprintf(stderr,
                "Frame received (#%lu) - Valid (%liB) - QSize %f\n",
                frameCount,
                videoFrame->GetRowBytes() * videoFrame->GetHeight(),
                (double)qsize / 1024 / 1024);
    }

    videoFrame->GetBytes(&frameBytes);

    if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
        if (pix_fmt == AV_PIX_FMT_UYVY422 && draw_bars) {
            unsigned bars[8] = {
                0xEA80EA80, 0xD292D210, 0xA910A9A5, 0x90229035,
                0x6ADD6ACA, 0x51EF515A, 0x286D28EF, 0x10801080 };
            int width  = videoFrame->GetWidth();
            int height = videoFrame->GetHeight();
            unsigned *p = (unsigned *)frameBytes;

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x += 2)
                    *p++ = bars[(x * 8) / width];
            }
        }
        if (!no_video) {
            time(&cur_time);
            fprintf(stderr,"%s "
                    "Frame received (#%lu) - No input signal detected "
                    "- Frames dropped %u - Total dropped %u\n",
                    ctime(&cur_time),
                    frameCount, ++dropped, ++totaldropped);
        }
        no_video = 1;
    } else {
        if (no_video) {
            time(&cur_time);
            fprintf(stderr, "%s "
                    "Frame received (#%lu) - Input returned "
                    "- Frames dropped %u - Total dropped %u\n",
                    ctime(&cur_time),
                    frameCount, ++dropped, ++totaldropped);
        }
        no_video = 0;
    }

    pkt.dts = pkt.pts = pts;

    pkt.duration = duration;
    //To be made sure it still applies
    pkt.flags       |= AV_PKT_FLAG_KEY;
    pkt.stream_index = video_st->index;
    pkt.data         = (uint8_t *)frameBytes;
    pkt.size         = videoFrame->GetRowBytes() *
                       videoFrame->GetHeight();
    //fprintf(stderr,"Video Frame size %d ts %d\n", pkt.size, pkt.pts);
    c->frame_number++;

    if (pix_fmt == AV_PIX_FMT_YUV422P10)
        vanc_as_side_data(&pkt, videoFrame);

    avpacket_queue_put(&queue, &pkt);
}


HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame *videoFrame, IDeckLinkAudioInputPacket *audioFrame)
{

    frameCount++;

    // Handle Video Frame
    if (videoFrame) {
        BMDTimeValue frameTime;
        BMDTimeValue frameDuration;
        int64_t pts;
        videoFrame->GetStreamTime(&frameTime, &frameDuration,
                                  video_st->time_base.den);

        pts = frameTime / video_st->time_base.num;

        if (initial_video_pts == AV_NOPTS_VALUE) {
            initial_video_pts = pts;
        }

        pts -= initial_video_pts;

        write_video_packet(videoFrame, pts, frameDuration);

        if (serial_fd > 0) {
            char line[8] = {0};
            int count = read(serial_fd, line, 7);
            if (count > 0)
                fprintf(stderr, "read %d bytes: %s  \n", count, line);
            else line[0] = ' ';
            write_data_packet(line, 7, pts);
        }

        if (wallclock) {
            int64_t t = av_gettime();
            char line[20];
            snprintf(line, sizeof(line), "%" PRId64, t);
            write_data_packet(line, strlen(line), pts);
        }
    }

    // Handle Audio Frame
    if (audioFrame)
        write_audio_packet(audioFrame);


    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode,
    BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

int usage(int status)
{
    HRESULT result;
    IDeckLinkIterator *deckLinkIterator;
    IDeckLink *deckLink;
    int numDevices = 0;

    fprintf(stderr,
            "Usage: bmdcapture -m <mode id> [OPTIONS]\n"
            "\n"
            "    -m <mode id>:\n"
            );

    // Create an IDeckLinkIterator object to enumerate all DeckLink cards in the system
    deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (deckLinkIterator == NULL) {
        fprintf(
            stderr,
            "A DeckLink iterator could not be created.  The DeckLink drivers may not be installed.\n");
        return 1;
    }

    // Enumerate all cards in this system
    while (deckLinkIterator->Next(&deckLink) == S_OK) {
        BMDProbeString str;
        // Increment the total number of DeckLink cards found
        numDevices++;
        if (numDevices > 1) {
            printf("\n\n");
        }

        // Print the model name of the DeckLink card
        result = deckLink->GetModelName(&str);
        if (result == S_OK) {
            printf("-> %s (-C %d )\n\n",
                   ToStr(str),
                   numDevices - 1);
            FreeStr(str);
        }

        print_input_modes(deckLink);
        // Release the IDeckLink instance when we've finished with it to prevent leaks
        deckLink->Release();
    }
    deckLinkIterator->Release();

    // If no DeckLink cards were found in the system, inform the user
    if (numDevices == 0) {
        printf("No Blackmagic Design devices were found.\n");
    }
    printf("\n");

    fprintf(
        stderr,
        "    -v                   Be verbose (report each 25 frames)\n"
        "    -f <filename>        Filename raw video will be written to\n"
        "    -F <format>          Define the file format to be used\n"
        "    -c <channels>        Audio Channels (2, 8 or 16 - default is 2)\n"
        "    -s <depth>           Audio Sample Depth (16 or 32 - default is 16)\n"
        "    -p <pixel>           PixelFormat (yuv8, yuv10, rgb10)\n"
        "    -n <frames>          Number of frames to capture (default is unlimited)\n"
        "    -M <memlimit>        Maximum queue size in GB (default is 1 GB)\n"
        "    -C <num>             number of card to be used\n"
        "    -S <serial_device>   data input serial\n"
        "    -A <audio-in>        Audio input:\n"
        "                         1: Analog (RCA or XLR)\n"
        "                         2: Embedded Audio (HDMI/SDI)\n"
        "                         3: Digital Audio (AES/EBU)\n"
        "    -V <video-in>        Video input:\n"
        "                         1: Composite\n"
        "                         2: Component\n"
        "                         3: HDMI\n"
        "                         4: SDI\n"
        "                         5: Optical SDI\n"
        "                         6: S-Video\n"
        "    -o <optionstring>    AVFormat options\n"
        "    -w                   Embed a wallclock stream\n"
        "    -d <filler>          When the source is offline draw a black frame or color bars\n"
        "                         0: black frame\n"
        "                         1: color bars\n"
        "Capture video and audio to a file.\n"
        "Raw video and audio can be sent to a pipe to avconv or vlc e.g.:\n"
        "\n"
        "    bmdcapture -m 2 -A 1 -V 1 -F nut -f pipe:1\n\n\n"
        );

    exit(status);
}

static void *push_packet(void *ctx)
{
    AVFormatContext *s = (AVFormatContext *)ctx;
    AVPacket pkt;
    int ret;

    while (avpacket_queue_get(&queue, &pkt, 1)) {
        av_interleaved_write_frame(s, &pkt);
        if ((g_maxFrames > 0 && frameCount >= g_maxFrames) ||
            avpacket_queue_size(&queue) > g_memoryLimit) {
            pthread_cond_signal(&sleepCond);
        }
    }

    return NULL;
}

static void exit_handler(int sig)
{
   pthread_cond_signal(&sleepCond);
}

static void set_signal()
{
    signal(SIGINT , exit_handler);
    signal(SIGTERM, exit_handler);
}

int main(int argc, char *argv[])
{
    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    DeckLinkCaptureDelegate *delegate;
    BMDDisplayMode selectedDisplayMode = bmdModeNTSC;
    int displayModeCount               = 0;
    int exitStatus                     = 1;
    int aconnection                    = 0, vconnection = 0, camera = 0, i = 0;
    int ch;
    AVDictionary *opts = NULL;
    BMDPixelFormat pix = bmdFormat8BitYUV;
    HRESULT result;
    pthread_t th;

    pthread_mutex_init(&sleepMutex, NULL);
    pthread_cond_init(&sleepCond, NULL);
    av_register_all();

    if (!deckLinkIterator) {
        fprintf(stderr,
                "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }

    // Parse command line options
    while ((ch = getopt(argc, argv, "?hvc:s:f:a:m:n:p:M:F:C:A:V:o:w")) != -1) {
        switch (ch) {
        case 'v':
            g_verbose = true;
            break;
        case 'm':
            g_videoModeIndex = atoi(optarg);
            break;
        case 'c':
            g_audioChannels = atoi(optarg);
            if (g_audioChannels != 2 &&
                g_audioChannels != 8 &&
                g_audioChannels != 16) {
                fprintf(
                    stderr,
                    "Invalid argument: Audio Channels must be either 2, 8 or 16\n");
                goto bail;
            }
            break;
        case 's':
            g_audioSampleDepth = atoi(optarg);
            switch (g_audioSampleDepth) {
            case 16:
                sample_fmt = AV_SAMPLE_FMT_S16;
                break;
            case 32:
                sample_fmt = AV_SAMPLE_FMT_S32;
                break;
            default:
                fprintf(stderr,
                        "Invalid argument:"
                        " Audio Sample Depth must be either 16 bits"
                        " or 32 bits\n");
                goto bail;
            }
            break;
        case 'p':
            switch (atoi(optarg)) {
            case  8:
                pix     = bmdFormat8BitYUV;
                pix_fmt = AV_PIX_FMT_UYVY422;
                break;
            case 10:
                pix     = bmdFormat10BitYUV;
                pix_fmt = AV_PIX_FMT_YUV422P10;
                break;
            default:
                if (!strcmp("rgb10", optarg)) {
                    pix     = bmdFormat10BitRGB;
                    pix_fmt = AV_PIX_FMT_RGB48;
                    break;
                }
                if (!strcmp("yuv10", optarg)) {
                    pix     = bmdFormat10BitYUV;
                    pix_fmt = AV_PIX_FMT_YUV422P10;
                    break;
                }
                if (!strcmp("yuv8", optarg)) {
                    pix     = bmdFormat8BitYUV;
                    pix_fmt = AV_PIX_FMT_UYVY422;
                    break;
                }

                fprintf(
                    stderr,
                    "Invalid argument: Pixel Format Depth must be either 8 bits or 10 bits\n");
                goto bail;
            }
            break;
        case 'f':
            g_videoOutputFile = optarg;
            break;
        case 'n':
            g_maxFrames = atoi(optarg);
            break;
        case 'M':
            g_memoryLimit = atoi(optarg) * 1024 * 1024 * 1024L;
            break;
        case 'F':
            fmt = av_guess_format(optarg, NULL, NULL);
            break;
        case 'A':
            aconnection = atoi(optarg);
            break;
        case 'V':
            vconnection = atoi(optarg);
            break;
        case 'C':
            camera = atoi(optarg);
            break;
        case 'S':
            serial_fd = open(optarg, O_RDWR | O_NONBLOCK);
            break;
        case 'o':
            if (av_dict_parse_string(&opts, optarg, "=", ":", 0) < 0) {
                fprintf(stderr, "Cannot parse option string %s\n",
                        optarg);
                goto bail;
            }
            break;
        case 'w':
            wallclock = true;
            break;
        case 'd':
            draw_bars = atoi(optarg);
            break;
        case '?':
        case 'h':
            usage(0);
        }
    }

    if (serial_fd > 0 && wallclock) {
        fprintf(stderr, "%s",
                "Wallclock and serial are not supported together\n"
                "Please disable either.\n");
        exit(1);
    }

    /* Connect to the first DeckLink instance */
    do
        result = deckLinkIterator->Next(&deckLink);
    while (i++ < camera);

    if (result != S_OK) {
        fprintf(stderr, "No DeckLink PCI cards found.\n");
        goto bail;
    }

    if (deckLink->QueryInterface(IID_IDeckLinkInput,
                                 (void **)&deckLinkInput) != S_OK) {
        goto bail;
    }

    result = deckLink->QueryInterface(IID_IDeckLinkConfiguration,
                                      (void **)&deckLinkConfiguration);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the IDeckLinkConfiguration interface - result = %08x\n",
            result);
        goto bail;
    }

    result = S_OK;
    switch (aconnection) {
    case 1:
        result = DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionAnalog);
        break;
    case 2:
        result = DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionEmbedded);
        break;
    case 3:
        result = DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionAESEBU);
        break;
    default:
        // do not change it
        break;
    }
    if (result != S_OK) {
        fprintf(stderr, "Failed to set audio input - result = %08x\n", result);
        goto bail;
    }

    result = S_OK;
    switch (vconnection) {
    case 1:
        result = DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionComposite);
        break;
    case 2:
        result = DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionComponent);
        break;
    case 3:
        result = DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionHDMI);
        break;
    case 4:
        result = DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionSDI);
        break;
    case 5:
        result = DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionOpticalSDI);
        break;
    case 6:
        result = DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionSVideo);
        break;
    default:
        // do not change it
        break;
    }
    if (result != S_OK) {
        fprintf(stderr, "Failed to set video input - result %08x\n", result);
        goto bail;
    }

    delegate = new DeckLinkCaptureDelegate();
    deckLinkInput->SetCallback(delegate);

    // Obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output
    result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the video output display mode iterator - result = %08x\n",
            result);
        goto bail;
    }

    if (!g_videoOutputFile) {
        fprintf(stderr,
                "Missing argument: Please specify output path using -f\n");
        goto bail;
    }

    if (!fmt) {
        fmt = av_guess_format(NULL, g_videoOutputFile, NULL);
        if (!fmt) {
            fprintf(
                stderr,
                "Unable to guess output format, please specify explicitly using -F\n");
            goto bail;
        }
    }

    if (g_videoModeIndex < 0) {
        fprintf(stderr, "No video mode specified\n");
        usage(0);
    }

    while (displayModeIterator->Next(&displayMode) == S_OK) {
        if (g_videoModeIndex == displayModeCount) {
            selectedDisplayMode = displayMode->GetDisplayMode();
            break;
        }
        displayModeCount++;
        displayMode->Release();
    }

    result = deckLinkInput->EnableVideoInput(selectedDisplayMode, pix, 0);
    if (result != S_OK) {
        fprintf(stderr,
                "Failed to enable video input. Is another application using "
                "the card?\n");
        goto bail;
    }

    result = deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz,
                                             g_audioSampleDepth,
                                             g_audioChannels);
    if (result != S_OK) {
        fprintf(stderr,
                "Failed to enable audio input. Is another application using "
                "the card?\n");
        goto bail;
    }

    oc          = avformat_alloc_context();
    oc->oformat = fmt;

    snprintf(oc->filename, sizeof(oc->filename), "%s", g_videoOutputFile);


    switch (pix) {
    case bmdFormat8BitYUV:
        fmt->video_codec = AV_CODEC_ID_RAWVIDEO;
        break;
    case bmdFormat10BitYUV:
        fmt->video_codec = AV_CODEC_ID_V210;
        break;
    case bmdFormat10BitRGB:
        fmt->video_codec = AV_CODEC_ID_R210;
        break;
    }

    fmt->audio_codec = (sample_fmt == AV_SAMPLE_FMT_S16 ? AV_CODEC_ID_PCM_S16LE : AV_CODEC_ID_PCM_S32LE);

    video_st = add_video_stream(oc, fmt->video_codec);
    audio_st = add_audio_stream(oc, fmt->audio_codec);

    if (serial_fd > 0 || wallclock)
        data_st = add_data_stream(oc, AV_CODEC_ID_TEXT);

    if (!(fmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc->pb, oc->filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open '%s'\n", oc->filename);
            exit(1);
        }
    }

    avformat_write_header(oc, &opts);
    avpacket_queue_init(&queue);

    result = deckLinkInput->StartStreams();
    if (result != S_OK) {
        goto bail;
    }
    // All Okay.
    exitStatus = 0;

    if (pthread_create(&th, NULL, push_packet, oc))
        goto bail;

    // Block main thread until signal occurs
    pthread_mutex_lock(&sleepMutex);
    set_signal();
    pthread_cond_wait(&sleepCond, &sleepMutex);
    pthread_mutex_unlock(&sleepMutex);
    deckLinkInput->StopStreams();
    fprintf(stderr, "Stopping Capture\n");
    avpacket_queue_end(&queue);

bail:
    if (displayModeIterator != NULL) {
        displayModeIterator->Release();
        displayModeIterator = NULL;
    }

    if (deckLinkInput != NULL) {
        deckLinkInput->Release();
        deckLinkInput = NULL;
    }

    if (deckLink != NULL) {
        deckLink->Release();
        deckLink = NULL;
    }

    if (deckLinkIterator != NULL) {
        deckLinkIterator->Release();
    }

    if (oc != NULL) {
        av_write_trailer(oc);
        if (!(fmt->flags & AVFMT_NOFILE)) {
            /* close the output file */
            avio_close(oc->pb);
        }
    }

    return exitStatus;
}
