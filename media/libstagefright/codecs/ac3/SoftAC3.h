/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SOFT_RAW_H_

#define SOFT_RAW_H_

#include <media/stagefright/omx/SimpleSoftOMXComponent.h>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavresample/avresample.h>
    #include <libswresample/swresample.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/error.h>
    #include <libavutil/opt.h>
}

const int AVCODEC_MAX_AUDIO_FRAME_SIZE = 192000; // Deprecated in ffmpeg

namespace android {

struct SoftAC3 : public SimpleSoftOMXComponent {
    SoftAC3(const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftAC3();

    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortEnableCompleted(OMX_U32 portIndex, bool enabled);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);

private:
    enum {
        kInputPortIndex   = 0,
        kOutputPortIndex  = 1,
        kNumInputBuffers  = 4,
        kNumOutputBuffers = 4,
        kOutputBufferSize = 4608 * 2
    };

    enum StreamStatus {
        INPUT_DATA_AVAILABLE,
        INPUT_EOS_SEEN,
        OUTPUT_FRAMES_FLUSHED,
    };

    enum {
        ERR_NO_FRM              = 2,
        ERR_FLUSHED             = 1,
        ERR_OK                  = 0,
        ERR_OOM                 = -1,
        ERR_INVALID_PARAM       = -2,
        ERR_CODEC_NOT_FOUND     = -3,
        ERR_DECODER_OPEN_FAILED = -4,
        ERR_SWR_INIT_FAILED     = -5,
        ERR_RESAMPLE_FAILED     = -6
    };

    enum {
        NONE,
        AWAITING_DISABLED,
        AWAITING_ENABLED
    } mOutputPortSettingsChange;

    bool mSignalledError;
    int32_t mSrcChannelCount;
    int32_t mSrcFrequency;
    int32_t mDestChannelCount;
    int32_t mDestFrequency;
    int64_t mSrcChannelLayout;
    int64_t mDestChannelLayout;
    enum AVSampleFormat mSrcFormat;
    enum AVSampleFormat mDestFormat;
    DECLARE_ALIGNED(16, uint8_t, mAudioBuffer)[AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];
    uint8_t mSilenceBuffer[kOutputBufferSize];
    uint8_t *mResampledData;
    int32_t mResampledDataSize;
    int64_t mAudioClock;
    int32_t mInputBufferSize;
    StreamStatus mStreamStatus;
    AVCodec *mCodec;
    AVCodecContext *mCodecContext;
    struct SwrContext *mResampleContext;
    AVFrame *mFrame;

    void initPorts();
    status_t initDecoder();
    status_t initCodecContext();
    void resetCodecContext();
    void releaseCodecContext();
    bool isConfigured();
    void adjustAudioParams();
    void updateTimeStamp(OMX_BUFFERHEADERTYPE *inHeader);
    int32_t decodeAudio();
    int32_t resampleAudio();
    void drainOneOutputBuffer();
    void drainEOSOutputBuffer();
    void drainAllOutputBuffers();
    void initPacket(AVPacket *pkt, OMX_BUFFERHEADERTYPE *inHeader);
    
    DISALLOW_EVIL_CONSTRUCTORS(SoftAC3);
};

}  // namespace android

#endif  // SOFT_RAW_H_
