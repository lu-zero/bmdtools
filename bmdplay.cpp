/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
** Copyright (c) 2011 Luca Barbato
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
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
}
#include "compat.h"
#include "Play.h"

pthread_mutex_t          sleepMutex;
pthread_cond_t           sleepCond;
IDeckLinkConfiguration  *deckLinkConfiguration;

AVFormatContext *ic;
AVFrame *avframe;
AVStream *audio_st = NULL;
AVStream *video_st = NULL;

static enum PixelFormat           pix_fmt = PIX_FMT_UYVY422;
static BMDPixelFormat                 pix = bmdFormat8BitYUV;

DECLARE_ALIGNED(16,uint8_t,audio_buffer)[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
int data_size = sizeof(audio_buffer);
int offset = 0;
int buffer = 2;

const unsigned long        kAudioWaterlevel = 48000/4; /* small */

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

PacketQueue audioqueue;
PacketQueue videoqueue;
struct SwsContext *sws;

static int packet_queue_put(PacketQueue *q, AVPacket *pkt);

static void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
//    packet_queue_put(q, &flush_pkt);
}

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for(pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    /* duplicate the packet */
    if (av_dup_packet(pkt) < 0)
        return -1;

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt)

        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for(;;) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
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
int64_t first_audio_pts = AV_NOPTS_VALUE;
int64_t first_video_pts = AV_NOPTS_VALUE;
int64_t first_pts = AV_NOPTS_VALUE;

void *fill_queues(void *unused) {
    AVPacket pkt;
    AVStream *st;

    while (1) {
	    int err = av_read_frame(ic, &pkt);
	    if (err) {
                if (videoqueue.nb_packets >= 0) {
                    fprintf(stderr, "Cannot get new frames, flushing\n");
                    continue;
                } else {
                    fprintf(stderr, "End of stream\n");
                }
            }
            if (videoqueue.nb_packets > 1000) {
                fprintf(stderr, "Queue size %d problems ahead\n", videoqueue.size);
            }
	    st = ic->streams[pkt.stream_index];
	    switch (st->codec->codec_type) {
	    case AVMEDIA_TYPE_VIDEO:
            if (pkt.pts != AV_NOPTS_VALUE) {
                if (first_pts == AV_NOPTS_VALUE) {
	            first_pts = first_video_pts = pkt.pts;
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
                    first_pts = first_audio_pts = pkt.pts;
                    first_video_pts =
                         av_rescale_q(pkt.pts, audio_st->time_base,                                                            video_st->time_base);
                }
	        pkt.pts -= first_audio_pts;
            }
		packet_queue_put(&audioqueue, &pkt);
	    break;
	    }
/*	    while (videoqueue.nb_packets>10)
		usleep(30); */
	    //fprintf(stderr, "V %d A %d\n", videoqueue.nb_packets, audioqueue.nb_packets); //----->
            av_free_packet(&pkt);
    }
    return NULL;
}

void    sigfunc (int signum)
{
    pthread_cond_signal(&sleepCond);
}

void	print_output_modes (IDeckLink* deckLink)
{
	IDeckLinkOutput*					deckLinkOutput = NULL;
	IDeckLinkDisplayModeIterator*		displayModeIterator = NULL;
	IDeckLinkDisplayMode*				displayMode = NULL;
	HRESULT								result;
        int displayModeCount = 0;

	// Query the DeckLink for its configuration interface
	result = deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&deckLinkOutput);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not obtain the IDeckLinkOutput interface - result = %08x\n", result);
		goto bail;
	}

	// Obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output
	result = deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not obtain the video output display mode iterator - result = %08x\n", result);
		goto bail;
	}

	// List all supported output display modes
	printf("Supported video output display modes and pixel formats:\n");
	while (displayModeIterator->Next(&displayMode) == S_OK)
	{
		char *			displayModeString = NULL;

		result = displayMode->GetName((const char **) &displayModeString);
		if (result == S_OK)
		{
			char					modeName[64];
			int						modeWidth;
			int						modeHeight;
			BMDTimeValue			frameRateDuration;
			BMDTimeScale			frameRateScale;
			int						pixelFormatIndex = 0; // index into the gKnownPixelFormats / gKnownFormatNames arrays
			BMDDisplayModeSupport	displayModeSupport;
			// Obtain the display mode's properties
			modeWidth = displayMode->GetWidth();
			modeHeight = displayMode->GetHeight();
			displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
			printf("        %2d:   %-20s \t %d x %d \t %7g FPS\n", displayModeCount++, displayModeString, modeWidth, modeHeight, (double)frameRateScale / (double)frameRateDuration);

			free(displayModeString);
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
    IDeckLinkIterator* deckLinkIterator;
    IDeckLink*      deckLink;
    int       numDevices = 0;

    fprintf(stderr,
        "Usage: bmdplay -m <mode id> [OPTIONS]\n"
        "\n"
        "    -m <mode id>:\n"
    );

	// Create an IDeckLinkIterator object to enumerate all DeckLink cards in the system
	deckLinkIterator = CreateDeckLinkIteratorInstance();
	if (deckLinkIterator == NULL)
	{
		fprintf(stderr, "A DeckLink iterator could not be created.  The DeckLink drivers may not be installed.\n");
		return 1;
	}

	// Enumerate all cards in this system
	while (deckLinkIterator->Next(&deckLink) == S_OK)
	{
		char *		deviceNameString = NULL;

		// Increment the total number of DeckLink cards found
		numDevices++;
		if (numDevices > 1)
			printf("\n\n");

		// *** Print the model name of the DeckLink card
		result = deckLink->GetModelName((const char **) &deviceNameString);
		if (result == S_OK)
		{
			printf("=============== %s (-C %d )===============\n\n", deviceNameString, numDevices-1);
			free(deviceNameString);
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

    fprintf(stderr,
        "    -f <filename>        Filename raw video will be written to\n"
        "    -C <num>             Card number to be used\n"
        "    -b <num>             Seconds of pre-buffering before playback (default = 2 sec)\n"
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
    int         ch;
    int         videomode = 2;
    int         connection = 0;
    int         camera = 0;
    char *filename=NULL;

    while ((ch = getopt(argc, argv, "?hs:f:a:m:n:F:C:O:b:p:")) != -1)
    {
        switch (ch)
        {
            case 'p':
                switch (atoi(optarg)) {
                case  8:
                    pix = bmdFormat8BitYUV;
                    pix_fmt = PIX_FMT_UYVY422;
                    break;
                case 10:
                    pix = bmdFormat10BitYUV;
                    pix_fmt = PIX_FMT_YUV422P10;
                    break;
                default:
                    fprintf(stderr, "Invalid argument: Pixel Format Depth must be either 8 bits or 10 bits\n");
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
                buffer = atoi(optarg);
                break;
            case '?':
            case 'h':
               return usage(0);
        }
    }

    if (!filename) return usage(1);

    av_register_all();
    ic = avformat_alloc_context();

    avformat_open_input(&ic, filename, NULL, NULL);
    avformat_find_stream_info(ic, NULL);

    for (int i =0; i<ic->nb_streams; i++) {
        AVStream *st= ic->streams[i];
        AVCodecContext *avctx = st->codec;
        AVCodec *codec = avcodec_find_decoder(avctx->codec_id);
        if (!codec || avcodec_open2(avctx, codec, NULL) < 0)
            fprintf(stderr, "cannot find codecs for %s\n",
                (avctx->codec_type == AVMEDIA_TYPE_AUDIO)? "Audio" : "Video");
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

    if (!generator.Init(videomode, connection, camera))
        return 1;

    return 0;
}

Player::Player()
{
    m_audioChannelCount = 2;
    m_audioSampleRate = bmdAudioSampleRate48kHz;
    m_audioSampleDepth = 16;
    m_running = false;
    m_outputSignal = kOutputSignalDrop;
}

bool    Player::Init(int videomode, int connection, int camera)
{
    // Initialize the DeckLink API
    IDeckLinkIterator*            deckLinkIterator = CreateDeckLinkIteratorInstance();
    HRESULT                        result;
    int i=0;

    if (!deckLinkIterator)
    {
        fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }

    do {
        result = deckLinkIterator->Next(&m_deckLink);
    } while(i++<camera);

    if (result != S_OK)
    {
        fprintf(stderr, "No DeckLink PCI cards found\n");
        goto bail;
    }

    // Obtain the audio/video output interface (IDeckLinkOutput)
    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deckLinkOutput) != S_OK)
        goto bail;

    result = m_deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&deckLinkConfiguration);
    if (result != S_OK)
    {
        fprintf(stderr, "Could not obtain the IDeckLinkConfiguration interface - result = %08x\n", result);
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
    pthread_t th;
    pthread_create(&th, NULL, fill_queues, NULL);

    sleep(buffer);// You can add the seconds you need for pre-buffering before start playing
    // Start playing
    StartRunning(videomode);

    pthread_mutex_lock(&sleepMutex);
    pthread_cond_wait(&sleepCond, &sleepMutex);
    pthread_mutex_unlock(&sleepMutex);
    pthread_kill(th, 9);
    fprintf(stderr, "Bailling out\n");

bail:
    if (m_running == true)
    {
        StopRunning();
    }
    else
    {
        // Release any resources that were partially allocated
        if (m_deckLinkOutput != NULL)
        {
            m_deckLinkOutput->Release();
            m_deckLinkOutput = NULL;
        }
        //
        if (m_deckLink != NULL)
        {
            m_deckLink->Release();
            m_deckLink = NULL;
        }
    }

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    return true;
}

IDeckLinkDisplayMode*    Player::GetDisplayModeByIndex(int selectedIndex)
{
    // Populate the display mode combo with a list of display modes supported by the installed DeckLink card
    IDeckLinkDisplayModeIterator*        displayModeIterator;
    IDeckLinkDisplayMode*                deckLinkDisplayMode;
    IDeckLinkDisplayMode*                selectedMode = NULL;
    int index = 0;

    if (m_deckLinkOutput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
        goto bail;
    while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK)
    {
        const char       *modeName;

        if (deckLinkDisplayMode->GetName(&modeName) == S_OK)
        {
            if (index == selectedIndex)
            {
                printf("Selected mode: %s\n\n\n", modeName);
                selectedMode = deckLinkDisplayMode;
                goto bail;
            }
        }
        index++;
    }
bail:
    displayModeIterator->Release();
    return selectedMode;
}

void    Player::StartRunning (int videomode)
{
    IDeckLinkDisplayMode*    videoDisplayMode = NULL;
    unsigned long            audioSamplesPerFrame;

    // Get the display mode for 1080i 59.95
    videoDisplayMode = GetDisplayModeByIndex(videomode);

    if (!videoDisplayMode)
        return;

    m_frameWidth = videoDisplayMode->GetWidth();
    m_frameHeight = videoDisplayMode->GetHeight();
    videoDisplayMode->GetFrameRate(&m_frameDuration, &m_frameTimescale);

    // Set the video output mode
    if (m_deckLinkOutput->EnableVideoOutput(videoDisplayMode->GetDisplayMode(), bmdVideoOutputFlagDefault) != S_OK)
    {
        fprintf(stderr, "Failed to enable video output\n");
        goto bail;
    }

    // Set the audio output mode
    if (m_deckLinkOutput->EnableAudioOutput(bmdAudioSampleRate48kHz, m_audioSampleDepth, audio_st->codec->channels, bmdAudioOutputStreamTimestamped) != S_OK)
/*    bmdAudioOutputStreamTimestamped
    bmdAudioOutputStreamContinuous */
    {
        fprintf(stderr, "Failed to enable audio output\n");
        goto bail;
    }

    for (unsigned i = 0; i < 10; i++)
    	ScheduleNextFrame(true);

    // Begin audio preroll.  This will begin calling our audio callback, which will start the DeckLink output stream.
//    m_audioBufferOffset = 0;
    if (m_deckLinkOutput->BeginAudioPreroll() != S_OK)
    {
        fprintf(stderr, "Failed to begin audio preroll\n");
        goto bail;
    }

    m_running = true;

    return;

bail:
    // *** Error-handling code.  Cleanup any resources that were allocated. *** //
    StopRunning();
}


void    Player::StopRunning ()
{
    // Stop the audio and video output streams immediately
    m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
    //
    m_deckLinkOutput->DisableAudioOutput();
    m_deckLinkOutput->DisableVideoOutput();

    if (m_videoFrameBlack != NULL)
        m_videoFrameBlack->Release();
    m_videoFrameBlack = NULL;

    if (m_videoFrameBars != NULL)
        m_videoFrameBars->Release();
    m_videoFrameBars = NULL;

    if (m_audioBuffer != NULL)
        free(m_audioBuffer);
    m_audioBuffer = NULL;

    // Success; update the UI
    m_running = false;
}

void    Player::ScheduleNextFrame (bool prerolling)
{
	AVPacket pkt;
        AVPicture picture;

	packet_queue_get(&videoqueue, &pkt, 1);

        IDeckLinkMutableVideoFrame *videoFrame;
        m_deckLinkOutput->CreateVideoFrame(m_frameWidth,
                                           m_frameHeight,
                                           m_frameWidth*2,
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
                                                pkt.pts * video_st->time_base.num,
                                                pkt.duration * video_st->time_base.num,
                                                video_st->time_base.den) != S_OK)
	fprintf(stderr, "Error scheduling frame\n");
        }
	av_free_packet(&pkt);
}

void    Player::WriteNextAudioSamples ()
{
    uint32_t samplesWritten = 0;
    AVPacket pkt = {0};
    unsigned int bufferedSamples;
    m_deckLinkOutput->GetBufferedAudioSampleFrameCount(&bufferedSamples);

    if (bufferedSamples>kAudioWaterlevel) return;

    if(!packet_queue_get(&audioqueue, &pkt, 0)) {
       fprintf(stderr, "I'd quit now \n");
       exit(0);
       return;
    }

    data_size = sizeof(audio_buffer);
    avcodec_decode_audio3(audio_st->codec,
                          (int16_t*)audio_buffer, &data_size, &pkt);
    av_free_packet(&pkt);

    if (m_deckLinkOutput->ScheduleAudioSamples(audio_buffer+offset,
                                               data_size/4,
                                               pkt.pts, 48000,
                                               &samplesWritten) != S_OK)
        fprintf(stderr, "error writing audio sample\n");
//    offset = (samplesWritten + offset) % data_size;

    //fprintf(stderr, "Buffer %d, written %d, available %d offset %d\n", bufferedSamples, samplesWritten, data_size-offset, offset);//--------->
}

/************************* DeckLink API Delegate Methods *****************************/

HRESULT        Player::ScheduledFrameCompleted (IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result)
{
    completedFrame->Release(); // We could recycle them probably
    ScheduleNextFrame(false);
    return S_OK;
}

HRESULT        Player::ScheduledPlaybackHasStopped ()
{
    return S_OK;
}

HRESULT        Player::RenderAudioSamples (bool preroll)
{
    // Provide further audio samples to the DeckLink API until our preferred buffer waterlevel is reached
    WriteNextAudioSamples();

    if (preroll)
    {
        // Start audio and video output
        m_deckLinkOutput->StartScheduledPlayback(0, 100, 1.0);
    }

    return S_OK;
}
