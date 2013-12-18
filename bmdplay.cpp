/*
 * Blackmagic Devices Decklink capture
 * Copyright (c) 2013 Luca Barbato.
 *
 * This file is part of bmdtools.
 *
 * libbmd is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libbmd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libbmd; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <libgen.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include "libswscale/swscale.h"
#include "packet.h"
}
#include "compat.h"
#include "Play.h"

pthread_mutex_t sleepMutex;
pthread_cond_t sleepCond;
IDeckLinkConfiguration *deckLinkConfiguration;

AVFormatContext *ic;
AVFrame *avframe;
AVStream *audio_st = NULL;
AVStream *video_st = NULL;

static enum PixelFormat pix_fmt = PIX_FMT_UYVY422;
static BMDPixelFormat pix       = bmdFormat8BitYUV;

static int buffer    = 2000 * 1000;
static int serial_fd = -1;

const unsigned long kAudioWaterlevel = 48000 / 4;      /* small */

PacketQueue audioqueue;
PacketQueue videoqueue;
PacketQueue dataqueue;
struct SwsContext *sws;


int64_t first_audio_pts = AV_NOPTS_VALUE;
int64_t first_video_pts = AV_NOPTS_VALUE;
int64_t first_pts       = AV_NOPTS_VALUE;
int fill_me             = 1;
int first_packet        = 0;

void Player::FillQueues()
{
    AVPacket pkt;
    AVStream *st;
    int once = 0;

    while (fill_me) {
        int err = av_read_frame(ic, &pkt);
        if (err) {
            pthread_cond_signal(&sleepCond);
            return NULL;
        }
        if (videoqueue.nb_packets > 1000) {
            if (!once++)
                fprintf(stderr, "Queue size %d problems ahead\n",
                        videoqueue.size);
        }
        st = ic->streams[pkt.stream_index];
        switch (st->codec->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (pkt.pts != AV_NOPTS_VALUE) {
                if (first_pts == AV_NOPTS_VALUE) {
                    first_pts       = first_video_pts = pkt.pts;
                    first_audio_pts =
                        av_rescale_q(pkt.pts, video_st->time_base,
                                     audio_st->time_base);
                }
                pkt.pts -= first_video_pts;
            }
            packet_queue_put(&videoqueue, &pkt);
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (pkt.pts != AV_NOPTS_VALUE) {
                if (first_pts == AV_NOPTS_VALUE) {
                    first_pts       = first_audio_pts = pkt.pts;
                    first_video_pts =
                        av_rescale_q(pkt.pts, audio_st->time_base,
                                     video_st->time_base);
                }
                pkt.pts -= first_audio_pts;
            }
            packet_queue_put(&audioqueue, &pkt);
            break;
        case AVMEDIA_TYPE_DATA:
	    packet_queue_put(&dataqueue, &pkt);
            break;
        }
    }
    return NULL;
}

void sigfunc(int signum)
{
    pthread_cond_signal(&sleepCond);
}

void print_output_modes(IDeckLink *deckLink)
{
    IDeckLinkOutput *deckLinkOutput                   = NULL;
    IDeckLinkDisplayModeIterator *displayModeIterator = NULL;
    IDeckLinkDisplayMode *displayMode                 = NULL;
    HRESULT result;
    int displayModeCount = 0;

    // Query the DeckLink for its configuration interface
    result = deckLink->QueryInterface(IID_IDeckLinkOutput,
                                      (void **)&deckLinkOutput);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the IDeckLinkOutput interface - result = %08x\n",
            result);
        goto bail;
    }

    // Obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output
    result = deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the video output display mode iterator - result = %08x\n",
            result);
        goto bail;
    }

    // List all supported output display modes
    printf("Supported video output display modes and pixel formats:\n");
    while (displayModeIterator->Next(&displayMode) == S_OK) {
        BMDProbeString str;

        result = displayMode->GetName(&str);
        if (result == S_OK) {
            char modeName[64];
            int modeWidth;
            int modeHeight;
            BMDTimeValue frameRateDuration;
            BMDTimeScale frameRateScale;
            int pixelFormatIndex = 0;                                                         // index into the gKnownPixelFormats / gKnownFormatNames arrays
            BMDDisplayModeSupport displayModeSupport;
            // Obtain the display mode's properties
            modeWidth  = displayMode->GetWidth();
            modeHeight = displayMode->GetHeight();
            displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
            printf("        %2d:   %-20s \t %d x %d \t %7g FPS\n",
                   displayModeCount++, ToStr(str), modeWidth, modeHeight,
                   (double)frameRateScale / (double)frameRateDuration);

            FreeStr(str);
        }
        // Release the IDeckLinkDisplayMode object to prevent a leak
        displayMode->Release();
    }
//	printf("\n");
bail:
    // Ensure that the interfaces we obtained are released to prevent a memory leak
    if (displayModeIterator != NULL)
        displayModeIterator->Release();
    if (deckLinkOutput != NULL)
        deckLinkOutput->Release();
}

int usage(int status)
{
    HRESULT result;
    IDeckLinkIterator *deckLinkIterator;
    IDeckLink *deckLink;
    int numDevices = 0;

    fprintf(stderr,
            "Usage: bmdplay -m <mode id> [OPTIONS]\n"
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
        if (numDevices > 1)
            printf("\n\n");

        // *** Print the model name of the DeckLink card
        result = deckLink->GetModelName(&str);
        if (result == S_OK) {
            printf("-> %s (-C %d )\n\n",
                   ToStr(str),
                   numDevices - 1);
            FreeStr(str);
        }

        print_output_modes(deckLink);
        // Release the IDeckLink instance when we've finished with it to prevent leaks
        deckLink->Release();
    }
    deckLinkIterator->Release();

    // If no DeckLink cards were found in the system, inform the user
    if (numDevices == 0)
        printf("No Blackmagic Design devices were found.\n");
    printf("\n");

    fprintf(
        stderr,
        "    -f <filename>        Filename raw video will be written to\n"
        "    -C <num>             Card number to be used\n"
        "    -b <num>             Milliseconds of pre-buffering before playback (default = 2000 ms)\n"
        "    -p <pixel>           PixelFormat Depth (8 or 10 - default is 8)\n"
        "    -O <output>          Output connection:\n"
        "                         1: Composite video + analog audio\n"
        "                         2: Components video + analog audio\n"
        "                         3: HDMI video + audio\n"
        "                         4: SDI video + audio\n\n");

    return status;
}

int main(int argc, char *argv[])
{
    Player generator;
    int ch, ret;
    int videomode  = 2;
    int connection = 0;
    int camera     = 0;
    char *filename = NULL;

    av_register_all();
    ic = avformat_alloc_context();

    while ((ch = getopt(argc, argv, "?hs:f:a:m:n:F:C:O:b:p:")) != -1) {
        switch (ch) {
        case 'p':
            switch (atoi(optarg)) {
            case  8:
                pix     = bmdFormat8BitYUV;
                pix_fmt = PIX_FMT_UYVY422;
                break;
            case 10:
                pix     = bmdFormat10BitYUV;
                pix_fmt = PIX_FMT_YUV422P10;
                break;
            default:
                fprintf(
                    stderr,
                    "Invalid argument: Pixel Format Depth must be either 8 bits or 10 bits\n");
                return usage(1);
            }
            break;
        case 'f':
            filename = strdup(optarg);
            break;
        case 'm':
            videomode = atoi(optarg);
            break;
        case 'O':
            connection = atoi(optarg);
            break;
        case 'C':
            camera = atoi(optarg);
            break;
        case 'b':
            buffer = atoi(optarg) * 1000;
            break;
        case 'S':
            serial_fd = open(optarg, O_RDWR | O_NONBLOCK);
            break;
        case '?':
        case 'h':
            return usage(0);
        }
    }

    if (!filename)
        return usage(1);

    avformat_open_input(&ic, filename, NULL, NULL);
    avformat_find_stream_info(ic, NULL);

    for (int i = 0; i < ic->nb_streams; i++) {
        AVStream *st          = ic->streams[i];
        AVCodecContext *avctx = st->codec;
        AVCodec *codec        = avcodec_find_decoder(avctx->codec_id);
        if (!codec || avcodec_open2(avctx, codec, NULL) < 0)
            fprintf(
                stderr, "cannot find codecs for %s\n",
                (avctx->codec_type ==
                 AVMEDIA_TYPE_AUDIO) ? "Audio" : "Video");
        if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_st = st;
        }
        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_st = st;
        }
    }

    av_dump_format(ic, 0, filename, 0);

    sws = sws_getContext(video_st->codec->width,
                         video_st->codec->height,
                         video_st->codec->pix_fmt,
                         video_st->codec->width,
                         video_st->codec->height,
                         pix_fmt,
                         SWS_BILINEAR, NULL, NULL, NULL);

    signal(SIGINT, sigfunc);
    pthread_mutex_init(&sleepMutex, NULL);
    pthread_cond_init(&sleepCond, NULL);

    free(filename);

    ret = generator.Init(videomode, connection, camera);

    avformat_close_input(&ic);

    fprintf(stderr, "video %ld audio %ld", videoqueue.nb_packets,
            audioqueue.nb_packets);

    return ret;
}

Player::Player()
{
    m_audioSampleRate = bmdAudioSampleRate48kHz;
    m_running         = false;
    m_outputSignal    = kOutputSignalDrop;
}

bool Player::Init(int videomode, int connection, int camera)
{
    // Initialize the DeckLink API
    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    HRESULT result;
    int i = 0;

    if (!deckLinkIterator) {
        fprintf(stderr,
                "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }

    m_audioSampleDepth =
        av_get_exact_bits_per_sample(audio_st->codec->codec_id);

    switch (audio_st->codec->channels) {
        case  2:
        case  8:
        case 16:
            break;
        default:
            fprintf(stderr,
                    "%d channels not supported, please use 2, 8 or 16\n",
                    audio_st->codec->channels);
            goto bail;
    }

    switch (m_audioSampleDepth) {
        case 16:
        case 32:
            break;
        default:
            fprintf(stderr, "%dbit audio not supported use 16bit or 32bit\n",
                    m_audioSampleDepth);
    }

    do
        result = deckLinkIterator->Next(&m_deckLink);
    while (i++ < camera);

    if (result != S_OK) {
        fprintf(stderr, "No DeckLink PCI cards found\n");
        goto bail;
    }

    // Obtain the audio/video output interface (IDeckLinkOutput)
    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput,
                                   (void **)&m_deckLinkOutput) != S_OK)
        goto bail;

    result = m_deckLink->QueryInterface(IID_IDeckLinkConfiguration,
                                        (void **)&deckLinkConfiguration);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the IDeckLinkConfiguration interface - result = %08x\n",
            result);
        goto bail;
    }
    //XXX make it generic
    switch (connection) {
    case 1:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionComposite);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionAnalog);
        break;
    case 2:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionComponent);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionAnalog);
        break;
    case 3:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionHDMI);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionEmbedded);
        break;
    case 4:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionSDI);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionEmbedded);
        break;
    default:
        // do not change it
        break;
    }

    // Provide this class as a delegate to the audio and video output interfaces
    m_deckLinkOutput->SetScheduledFrameCompletionCallback(this);
    m_deckLinkOutput->SetAudioCallback(this);

    avframe = avcodec_alloc_frame();

    packet_queue_init(&audioqueue);
    packet_queue_init(&videoqueue);
    packet_queue_init(&dataqueue);
    pthread_t th;
    pthread_create(&th, NULL, fill_queues, NULL);

    usleep(buffer);

    StartRunning(videomode);

    pthread_mutex_lock(&sleepMutex);
    pthread_cond_wait(&sleepCond, &sleepMutex);
    pthread_mutex_unlock(&sleepMutex);
    fill_me = 0;
    fprintf(stderr, "Exiting, cleaning up\n");
    packet_queue_end(&audioqueue);
    packet_queue_end(&videoqueue);

bail:
    if (m_running == true) {
        StopRunning();
    } else {
        // Release any resources that were partially allocated
        if (m_deckLinkOutput != NULL) {
            m_deckLinkOutput->Release();
            m_deckLinkOutput = NULL;
        }
        //
        if (m_deckLink != NULL) {
            m_deckLink->Release();
            m_deckLink = NULL;
        }
    }

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    return true;
}

IDeckLinkDisplayMode *Player::GetDisplayModeByIndex(int selectedIndex)
{
    // Populate the display mode combo with a list of display modes supported by the installed DeckLink card
    IDeckLinkDisplayModeIterator *displayModeIterator;
    IDeckLinkDisplayMode *deckLinkDisplayMode;
    IDeckLinkDisplayMode *selectedMode = NULL;
    int index                          = 0;

    if (m_deckLinkOutput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
        goto bail;
    while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK) {
        BMDProbeString str;

        if (deckLinkDisplayMode->GetName(&str) == S_OK) {
            if (index == selectedIndex) {
                printf("Selected mode: %s\n\n\n", ToStr(str));
                selectedMode = deckLinkDisplayMode;
                FreeStr(str);
                goto bail;
            }
        }
        index++;
    }
bail:
    displayModeIterator->Release();
    return selectedMode;
}

void Player::StartRunning(int videomode)
{
    IDeckLinkDisplayMode *videoDisplayMode = NULL;
    unsigned long audioSamplesPerFrame;

    // Get the display mode for 1080i 59.95
    videoDisplayMode = GetDisplayModeByIndex(videomode);

    if (!videoDisplayMode)
        return;

    m_frameWidth  = videoDisplayMode->GetWidth();
    m_frameHeight = videoDisplayMode->GetHeight();
    videoDisplayMode->GetFrameRate(&m_frameDuration, &m_frameTimescale);

    // Set the video output mode
    if (m_deckLinkOutput->EnableVideoOutput(videoDisplayMode->GetDisplayMode(),
                                            bmdVideoOutputFlagDefault) !=
        S_OK) {
        fprintf(stderr, "Failed to enable video output\n");
        return;
    }

    // Set the audio output mode
    if (m_deckLinkOutput->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                            m_audioSampleDepth,
                                            audio_st->codec->channels,
                                            bmdAudioOutputStreamTimestamped) !=
        S_OK) {
        fprintf(stderr, "Failed to enable audio output\n");
        return;
    }

    for (unsigned i = 0; i < 10; i++)
        ScheduleNextFrame(true);

    // Begin audio preroll.  This will begin calling our audio callback,
    // which will start the DeckLink output stream
    if (m_deckLinkOutput->BeginAudioPreroll() != S_OK) {
        fprintf(stderr, "Failed to begin audio preroll\n");
        return;
    }

    m_running = true;

    return;
}

void Player::StopRunning()
{
    // Stop the audio and video output streams immediately
    m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
    //
    m_deckLinkOutput->DisableAudioOutput();
    m_deckLinkOutput->DisableVideoOutput();
}

void Player::ScheduleNextFrame(bool prerolling)
{
    AVPacket pkt;
    AVPicture picture;

    if (serial_fd > 0 && packet_queue_get(&dataqueue, &pkt, 0)) {
        if (pkt.data[0] != ' '){
            fprintf(stderr,"written %.*s  \n", pkt.size, pkt.data);
            write(serial_fd, pkt.data, pkt.size);
        }
        av_free_packet(&pkt);
    }

    if (packet_queue_get(&videoqueue, &pkt, 0) < 1)
        return;

    IDeckLinkMutableVideoFrame *videoFrame;
    m_deckLinkOutput->CreateVideoFrame(m_frameWidth,
                                       m_frameHeight,
                                       m_frameWidth * 2,
                                       pix,
                                       bmdFrameFlagDefault,
                                       &videoFrame);
    void *frame;
    int got_picture;
    videoFrame->GetBytes(&frame);

    avcodec_decode_video2(video_st->codec, avframe, &got_picture, &pkt);
    if (got_picture) {
        avpicture_fill(&picture, (uint8_t *)frame, pix_fmt,
                       m_frameWidth, m_frameHeight);

        sws_scale(sws, avframe->data, avframe->linesize, 0, avframe->height,
                  picture.data, picture.linesize);

        if (m_deckLinkOutput->ScheduleVideoFrame(videoFrame,
                                                 pkt.pts *
                                                 video_st->time_base.num,
                                                 pkt.duration *
                                                 video_st->time_base.num,
                                                 video_st->time_base.den) !=
            S_OK)
            fprintf(stderr, "Error scheduling frame\n");
    }
    videoFrame->Release();
    av_free_packet(&pkt);
}

void Player::WriteNextAudioSamples()
{
    uint32_t samplesWritten = 0;
    AVPacket pkt            = { 0 };
    unsigned int bufferedSamples;
    int got_frame = 0;
    int i;
    int bytes_per_sample =
        av_get_bytes_per_sample(audio_st->codec->sample_fmt) *
        audio_st->codec->channels;
    int samples, off = 0;

    m_deckLinkOutput->GetBufferedAudioSampleFrameCount(&bufferedSamples);

    if (bufferedSamples > kAudioWaterlevel)
        return;

    if (!packet_queue_get(&audioqueue, &pkt, 0))
        return;

    samples = pkt.size / bytes_per_sample;

    do {
        if (m_deckLinkOutput->ScheduleAudioSamples(pkt.data +
                                                   off * bytes_per_sample,
                                                   samples,
                                                   pkt.pts + off, 48000,
                                                   &samplesWritten) != S_OK)
            fprintf(stderr, "error writing audio sample\n");
        samples -= samplesWritten;
        off     += samplesWritten;
    } while (samples > 0);

    av_free_packet(&pkt);
}

/************************* DeckLink API Delegate Methods *****************************/

HRESULT Player::ScheduledFrameCompleted(IDeckLinkVideoFrame *completedFrame,
                                        BMDOutputFrameCompletionResult result)
{
    if (fill_me)
        ScheduleNextFrame(false);
    return S_OK;
}

HRESULT Player::ScheduledPlaybackHasStopped()
{
    return S_OK;
}

HRESULT Player::RenderAudioSamples(bool preroll)
{
    // Provide further audio samples to the DeckLink API until our preferred buffer waterlevel is reached
    WriteNextAudioSamples();

    if (preroll) {
        // Start audio and video output
        m_deckLinkOutput->StartScheduledPlayback(0, 100, 1.0);
    }

    return S_OK;
}
