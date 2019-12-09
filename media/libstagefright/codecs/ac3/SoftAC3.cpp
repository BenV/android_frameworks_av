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

//#define LOG_NDEBUG 0
#define LOG_TAG "SoftAC3"
#include <utils/Log.h>

#include "SoftAC3.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/hexdump.h>

#include <OMX_AudioExt.h>
#include <OMX_IndexExt.h>

namespace android {

static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
static const uint64_t SF_NOPTS_VALUE = ((uint64_t)AV_NOPTS_VALUE-1);

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SoftAC3::SoftAC3(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mOutputPortSettingsChange(NONE),
      mSignalledError(false),
      mResampledData(NULL),
      mResampledDataSize(0),
      mAudioClock(0),
      mInputBufferSize(0),
      mStreamStatus(INPUT_DATA_AVAILABLE),
      mCodec(NULL),
      mCodecContext(NULL),
      mResampleContext(NULL),
      mFrame(NULL) {
    initPorts();

    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftAC3::~SoftAC3() {
    releaseCodecContext();
}

void SoftAC3::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 64 * 1024;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

     def.format.audio.cMIMEType =
        const_cast<char *>(MEDIA_MIMETYPE_AUDIO_AC3);

    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = 
        (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingAndroidAC3;

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumOutputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 64 * 1024;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.audio.cMIMEType = const_cast<char *>("audio/raw");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    addPort(def);
}

bool SoftAC3::isConfigured() {
	return mSrcChannelCount != 0;
}

status_t SoftAC3::initDecoder() {
    mCodecContext = avcodec_alloc_context3(mCodec);
    if (!mCodecContext) {
        ALOGE("Failed to allocate context.");
        return NO_MEMORY;
    }

    resetCodecContext();
    memset(mSilenceBuffer, 0, kOutputBufferSize);
    return OK;
}

status_t SoftAC3::initCodecContext() {
    if (mCodecContext->codec) {
        return OK;
    }

    mCodecContext->codec = avcodec_find_decoder(AV_CODEC_ID_AC3);
    if (!mCodecContext->codec) {
        return NAME_NOT_FOUND;
    }   

    mCodecContext->request_sample_fmt = OUTPUT_FORMAT_PCM_16BIT;
    int result = avcodec_open2(mCodecContext, mCodec, NULL);

    if (result < 0) {
        ALOGE("avcodec_open2 error: %d", result);
        mCodecContext = NULL; // TODO: release context
        return UNKNOWN_ERROR;
    }

    mCodecContext->workaround_bugs = 1;
    mCodecContext->idct_algo = 0;
    mCodecContext->skip_frame = AVDISCARD_DEFAULT;
    mCodecContext->skip_idct = AVDISCARD_DEFAULT;
    mCodecContext->skip_loop_filter = AVDISCARD_DEFAULT;
    mCodecContext->error_concealment = 3;

    // if (mCodecContext->lowres) mCodecContext->flags |= CODEC_FLAG_EMU_EDGE;
    // if (mCodecContext->capabilities & AV_CODEC_CAP_DR1) mCodecContext->flags |= AV_CODEC_FLAG_EMU_EDGE;

    mFrame = av_frame_alloc();
    if (!mFrame) {
        ALOGE("OOM for frame");
        return NO_MEMORY;
    }

    ALOGD("initCodecContext successful");
    return OK;
}

void SoftAC3::resetCodecContext() {
    mCodecContext->channels = 0;
    mCodecContext->sample_rate = 0;
    mCodecContext->sample_fmt = AV_SAMPLE_FMT_NONE;
    mCodecContext->bit_rate = 0;
    mCodecContext->extradata = NULL;
    mCodecContext->extradata_size = 0;

    mSrcChannelCount = mDestChannelCount = 0;
    mSrcFrequency = mDestFrequency = 0;
    mSrcFormat = mDestFormat = AV_SAMPLE_FMT_NONE;
    mSrcChannelLayout = mDestChannelLayout = 0;
}

void SoftAC3::releaseCodecContext() {
    if (mCodecContext) {
        avcodec_close(mCodecContext);
        av_free(mCodecContext);
        mCodecContext = NULL;
    }

    if (mFrame) {
        av_freep(&mFrame);
        mFrame = NULL;
    }

    if (mResampleContext) {
        swr_free(&mResampleContext);
        mResampleContext = NULL;
    }
}

void SoftAC3::adjustAudioParams() {
    int32_t channels = mCodecContext->channels;
    int32_t samplingRate = mCodecContext->sample_rate;

    CHECK(!isConfigured());

    mSrcChannelCount = channels;
    mSrcFrequency = samplingRate;
    mSrcFormat = AV_SAMPLE_FMT_S16;
    mSrcChannelLayout = av_get_default_channel_layout(mSrcChannelCount);

    channels = channels >= 2 ? 2 : 1;
    samplingRate = samplingRate < 4000 ? 4000 : samplingRate > 48000 ? 48000 : samplingRate;
    
    if (mDestChannelCount == 0) {
        mDestChannelCount = channels;
    }
    
    if (mDestFrequency == 0) {
        mDestFrequency = samplingRate;
    }

    mDestFormat = AV_SAMPLE_FMT_S16;
    mDestChannelLayout = av_get_default_channel_layout(mDestChannelCount);
    ALOGD("adjustAudioParams: srcChannels=%d, srcFrequency=%d, channels=%d, frequency=%d", mSrcChannelCount, mSrcFrequency, channels, samplingRate);
}

void SoftAC3::updateTimeStamp(OMX_BUFFERHEADERTYPE *inHeader) {
    //XXX reset to AV_NOPTS_VALUE if the pts is invalid
    if (inHeader->nTimeStamp == SF_NOPTS_VALUE) {
        inHeader->nTimeStamp = AV_NOPTS_VALUE;
    }

    //update the audio clock if the pts is valid
    if (inHeader->nTimeStamp != AV_NOPTS_VALUE) {
        mAudioClock = inHeader->nTimeStamp;
    }
}

OMX_ERRORTYPE SoftAC3::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    
    ALOGD("internalGetParameter index:0x%x", index);            
    switch ((int)index) {
        case OMX_IndexParamAudioPortFormat:
        {
            OMX_AUDIO_PARAM_PORTFORMATTYPE *formatParams = (OMX_AUDIO_PARAM_PORTFORMATTYPE *)params;

            if (!isValidOMXParam(formatParams)) {
                return OMX_ErrorBadParameter;
            }

            if (formatParams->nPortIndex > 1) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex > 0) {
                return OMX_ErrorNoMore;
            }

            formatParams->eEncoding = OMX_AUDIO_CodingPCM;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm:
        {
            OMX_AUDIO_PARAM_PCMMODETYPE *pcmParams = (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (!isValidOMXParam(pcmParams)) {
                return OMX_ErrorBadParameter;
            }

            if (pcmParams->nPortIndex > kOutputPortIndex) {
                ALOGE("pcmParams->nPortindex=%d >= %d, returning error", pcmParams->nPortIndex, kOutputPortIndex);
                return OMX_ErrorUndefined;
            }

            pcmParams->eNumData = OMX_NumericalDataSigned;
            pcmParams->eEndian = OMX_EndianBig;
            pcmParams->bInterleaved = OMX_TRUE;
            pcmParams->nBitPerSample = 16;
            pcmParams->ePCMMode = OMX_AUDIO_PCMModeLinear;
            pcmParams->eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcmParams->eChannelMapping[1] = OMX_AUDIO_ChannelRF;

            pcmParams->nChannels = mDestChannelCount;
            pcmParams->nSamplingRate = mDestFrequency;

            ALOGD("get pcm params, nChannels:%d, nSamplingRate:%d", pcmParams->nChannels, pcmParams->nSamplingRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAndroidAc3:
        {
            OMX_AUDIO_PARAM_ANDROID_AC3TYPE *profile =
                (OMX_AUDIO_PARAM_ANDROID_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nChannels = 0;
            profile->nSampleRate = 0;

            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftAC3::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    ALOGD("internalSetParameter index:0x%x, AC3 is: 0x%x, EAC3 is: 0x%x", index, OMX_IndexParamAudioAndroidAc3, OMX_IndexParamAudioAndroidEac3);

    switch ((int)index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams = (const OMX_PARAM_COMPONENTROLETYPE *)params;

            if (!isValidOMXParam(roleParams)) {
                return OMX_ErrorBadParameter;
            }

            if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.ac3",
                        OMX_MAX_STRINGNAME_SIZE - 1)) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPortFormat:
        {
            const OMX_AUDIO_PARAM_PORTFORMATTYPE *formatParams = (const OMX_AUDIO_PARAM_PORTFORMATTYPE *)params;

            if (!isValidOMXParam(formatParams)) {
                return OMX_ErrorBadParameter;
            }

            if (formatParams->nPortIndex > 1) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->eEncoding != OMX_AUDIO_CodingPCM) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm:
        {
            const OMX_AUDIO_PARAM_PCMMODETYPE *pcmParams = (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (!isValidOMXParam(pcmParams)) {
                ALOGE("isValidOMXParam returned false, bad parameters");
                return OMX_ErrorBadParameter;
            }

            if (pcmParams->nPortIndex != kOutputPortIndex) {
                ALOGE("pcmParams->nPortIndex=%d, expecting %d", pcmParams->nPortIndex, kOutputPortIndex);
                return OMX_ErrorUndefined;
            }

            mDestChannelCount = pcmParams->nChannels;
            mDestFrequency = pcmParams->nSamplingRate;

            ALOGD(
                "set OMX_IndexParamAudioPcm, nChannels:%d, "
                "nSampleRate:%d, nBitsPerSample:%d",
                pcmParams->nChannels, pcmParams->nSamplingRate,
                pcmParams->nBitPerSample
            );

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAndroidAc3:
        {
            OMX_AUDIO_PARAM_ANDROID_AC3TYPE *profile = (OMX_AUDIO_PARAM_ANDROID_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCodecContext->channels = profile->nChannels;
            mCodecContext->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGD("set OMX_IndexParamAudioAc3, nChannels:%d, nSampleRate:%d", profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        default:
        {
            if (index >= OMX_IndexVendorStartUnused) {
                ALOGW("Ignoring unsupported vendor-specific parameter: 0x%x", index);
                return OMX_ErrorNone;
            }

            return SimpleSoftOMXComponent::internalSetParameter(index, params);
        }
    }
}

void SoftAC3::onQueueFilled(OMX_U32 /* portIndex */) {
    if (mSignalledError || mOutputPortSettingsChange != NONE || mStreamStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    while (((mStreamStatus != INPUT_DATA_AVAILABLE) || !inQueue.empty()) && !outQueue.empty()) {
        if (mStreamStatus == INPUT_EOS_SEEN) {
            drainAllOutputBuffers();
            return;
        }

        inInfo = *inQueue.begin();
        inHeader = inInfo->mHeader;

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            ALOGD("audio decoder empty eos inbuf");
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
            mStreamStatus = INPUT_EOS_SEEN;
            continue;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            continue;
        }

        if (!mCodecContext || !mCodecContext->codec) {
            if (initCodecContext() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
	            return;
            }
        }

        while (!outQueue.empty()) {
            if (mResampledDataSize == 0) {
                int32_t err = decodeAudio();
                if (err < ERR_OK) {
                    notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                    mSignalledError = true;
                    return;
                } else if (err == ERR_FLUSHED) {
                    drainEOSOutputBuffer();
                    return;
                } else {
                    CHECK_EQ(err, ERR_OK);
                }
            } else {
                drainOneOutputBuffer();
            }
        }
    }
}

int32_t SoftAC3::resampleAudio() {
	int channels = 0;
    int64_t channelLayout = 0;
    size_t dataSize = 0;

    dataSize = av_samples_get_buffer_size(NULL, mFrame->channels,
            mFrame->nb_samples, (enum AVSampleFormat)mFrame->format, 1);

#if DEBUG_FRM
    ALOGV("ffmpeg audio decoder, nb_samples:%d, get buffer size:%d",
            mFrame->nb_samples, dataSize);
#endif

	channels = av_get_channel_layout_nb_channels(mFrame->channel_layout);
    channelLayout =
        (mFrame->channel_layout && mFrame->channels == channels) ?
        mFrame->channel_layout : av_get_default_channel_layout(mFrame->channels);

    if (mFrame->format != mSrcFormat
            || channelLayout != mSrcChannelLayout
            || mFrame->sample_rate != mSrcFrequency) {
        if (mResampleContext) {
            swr_free(&mResampleContext);
        }
        mResampleContext = swr_alloc_set_opts(NULL,
                mDestChannelLayout, mDestFormat, mDestFrequency,
                channelLayout,       (enum AVSampleFormat)mFrame->format, mFrame->sample_rate,
                0, NULL);
        if (!mResampleContext || swr_init(mResampleContext) < 0) {
            ALOGE("Cannot create sample rate converter for conversion "
                    "of %d Hz %s %d channels to %d Hz %s %d channels!",
                    mFrame->sample_rate,
                    av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                    mFrame->channels,
                    mDestFrequency,
                    av_get_sample_fmt_name(mDestFormat),
                    mDestChannelCount);
            return ERR_SWR_INIT_FAILED;
        }

        char src_layout_name[1024] = {0};
        char tgt_layout_name[1024] = {0};
        av_get_channel_layout_string(src_layout_name, sizeof(src_layout_name),
                mCodecContext->channels, channelLayout);
        av_get_channel_layout_string(tgt_layout_name, sizeof(tgt_layout_name),
                mDestChannelCount, mDestChannelLayout);
        ALOGI("Create sample rate converter for conversion "
                "of %d Hz %s %d channels(%s) "
                "to %d Hz %s %d channels(%s)!",
                mFrame->sample_rate,
                av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                mFrame->channels,
                src_layout_name,
                mDestFrequency,
                av_get_sample_fmt_name(mDestFormat),
                mDestChannelCount,
                tgt_layout_name);

        mSrcChannelLayout = channelLayout;
        mSrcChannelCount = mFrame->channels;
        mSrcFrequency = mFrame->sample_rate;
        mSrcFormat = (enum AVSampleFormat)mFrame->format;
    }

    if (mResampleContext) {
        const uint8_t **in = (const uint8_t **)mFrame->extended_data;
        uint8_t *out[] = {mAudioBuffer};
        int out_count = sizeof(mAudioBuffer) / mDestChannelCount / av_get_bytes_per_sample(mDestFormat);
        int out_size  = av_samples_get_buffer_size(NULL, mDestChannelCount, out_count, mDestFormat, 0);
        int len2 = 0;
        if (out_size < 0) {
            ALOGE("av_samples_get_buffer_size() failed");
            return ERR_INVALID_PARAM;
        }

        len2 = swr_convert(mResampleContext, out, out_count, in, mFrame->nb_samples);
        if (len2 < 0) {
            ALOGE("audio_resample() failed");
            return ERR_RESAMPLE_FAILED;
        }
        if (len2 == out_count) {
            ALOGE("warning: audio buffer is probably too small");
            swr_init(mResampleContext);
        }
        mResampledData = mAudioBuffer;
        mResampledDataSize = len2 * mDestChannelCount * av_get_bytes_per_sample(mDestFormat);

#if DEBUG_FRM
        ALOGD("ffmpeg audio decoder(resample), mFrame->nb_samples:%d, len2:%d, mResampledDataSize:%d, "
                "src channel:%u, src fmt:%s, tgt channel:%u, tgt fmt:%s",
                mFrame->nb_samples, len2, mResampledDataSize,
                mFrame->channels,
                av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                mDestChannelCount,
                av_get_sample_fmt_name(mDestFormat));
#endif
    } else {
        mResampledData = mFrame->data[0];
        mResampledDataSize = dataSize;
#if DEBUG_FRM
    ALOGD("ffmpeg audio decoder(no resample),"
            "nb_samples(before resample):%d, mResampledDataSize:%d",
            mFrame->nb_samples, mResampledDataSize);
#endif
    }

	return ERR_OK;
}

int32_t SoftAC3::decodeAudio() {
    int ret = ERR_OK;
    int32_t inputBufferUsedLength = 0;
	bool isFlush = (mStreamStatus != INPUT_DATA_AVAILABLE);
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    CHECK_EQ(mResampledDataSize, 0);

    if (isFlush) {
        ALOGI("decodeAudio early exit due to flush");
        return ERR_FLUSHED;
    } else {
        inInfo = *inQueue.begin();
        if (!inInfo) {
            ALOGW("inInfo = NULL, skipping");
            mResampledData = mSilenceBuffer;
            mResampledDataSize = kOutputBufferSize;
            return ERR_OK;
        }

        inHeader = inInfo->mHeader;
        if (mInputBufferSize == 0) {
		    updateTimeStamp(inHeader);
            mInputBufferSize = inHeader->nFilledLen;
        }
    }

    AVPacket pkt;
    initPacket(&pkt, inHeader);
    
    int result = avcodec_send_packet(mCodecContext, &pkt);
    if (result == AVERROR_EOF) {
        ALOGW("EOF error in avcodec_send_packet: %d", result);
        return ERR_FLUSHED;
    } else if (result < 0) {
         ALOGE("Error in avcodec_send_packet: %d", result);
         mResampledData = mSilenceBuffer;
         mResampledDataSize = kOutputBufferSize;
    }
   
    while (result >= 0) {
        av_frame_unref(mFrame);
        result = avcodec_receive_frame(mCodecContext, mFrame);

        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            ALOGW("Error in avcodec_receive_frame: %d", ret);
            mResampledData = mSilenceBuffer;
            mResampledDataSize = kOutputBufferSize;
            break;
        }

        ret = resampleAudio();
        if (mResampledData == mSilenceBuffer) {
            inputBufferUsedLength = mInputBufferSize;
        } else {
            inputBufferUsedLength = mFrame->pkt_size;
        }

        CHECK_GE(inHeader->nFilledLen, (uint32_t)inputBufferUsedLength);
        inHeader->nOffset += inputBufferUsedLength;
        inHeader->nFilledLen -= inputBufferUsedLength;
        mInputBufferSize -= inputBufferUsedLength;

        ALOGV("decodeAudio mInputBufferSize=%d, size=%d, filledLen=%d", mInputBufferSize, inputBufferUsedLength, inHeader->nFilledLen);
        if (inHeader->nFilledLen == 0) {
            CHECK_EQ(mInputBufferSize, 0);
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
        }
    }

    return ret;
}

void SoftAC3::drainOneOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
	BufferInfo *outInfo = *outQueue.begin();
	CHECK(outInfo != NULL);
	OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

	CHECK_GT(mResampledDataSize, 0);

    size_t copy = mResampledDataSize;
    if (mResampledDataSize > kOutputBufferSize) {
        copy = kOutputBufferSize;
	}

    outHeader->nOffset = 0;
    outHeader->nFilledLen = copy;
    outHeader->nTimeStamp = mAudioClock; 
    memcpy(outHeader->pBuffer, mResampledData, copy);
    outHeader->nFlags = 0;

    mResampledData += copy;
    mResampledDataSize -= copy;

    size_t frames = copy / (av_get_bytes_per_sample(mDestFormat) * mDestChannelCount);
    mAudioClock += (frames * 1000000ll) / mDestFrequency;

    ALOGV("drainOneOutputBuffer success, size=%d", copy);
    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);
}

void SoftAC3::drainEOSOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
	BufferInfo *outInfo = *outQueue.begin();
	CHECK(outInfo != NULL);
	OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    ALOGD("fill eos outbuf, resampledDataSize: %d", mResampledDataSize);

    outHeader->nTimeStamp = 0;
    outHeader->nFilledLen = 0;
    outHeader->nFlags = OMX_BUFFERFLAG_EOS;

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    mStreamStatus = OUTPUT_FRAMES_FLUSHED;
}

void SoftAC3::drainAllOutputBuffers() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    if (!mCodecContext || !mCodecContext->codec) {
        drainEOSOutputBuffer();
        return;
    }

    if(!(mCodecContext->codec->capabilities & AV_CODEC_CAP_DELAY)) {
        drainEOSOutputBuffer();
        return;
    }

    while (!outQueue.empty()) {
        if (mResampledDataSize == 0) {
            int32_t err = decodeAudio();
            if (err < ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
			    return;
            } else if (err == ERR_FLUSHED) {
                drainEOSOutputBuffer();
                return;
			} else {
                CHECK_EQ(err, ERR_OK);
			}
        } else {
            drainOneOutputBuffer();
        }
    }
}

void SoftAC3::onPortFlushCompleted(OMX_U32 portIndex) {
    ALOGV("decoder flush port(%d)", portIndex);

    if (portIndex == kInputPortIndex) {
        if (avcodec_is_open(mCodecContext)) {
            avcodec_flush_buffers(mCodecContext);
        }

	    mAudioClock = 0;
	    mInputBufferSize = 0;
	    mResampledDataSize = 0;
	    mResampledData = NULL;
        mStreamStatus = INPUT_DATA_AVAILABLE;
    }
}

void SoftAC3::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    if (portIndex != kOutputPortIndex) {
        return;
    }

    switch (mOutputPortSettingsChange) {
        case NONE:
            break;

        case AWAITING_DISABLED:
        {
            CHECK(!enabled);
            mOutputPortSettingsChange = AWAITING_ENABLED;
            break;
        }

        default:
        {
            CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
            CHECK(enabled);
            mOutputPortSettingsChange = NONE;
            break;
        }
    }
}

void SoftAC3::initPacket(AVPacket *pkt, OMX_BUFFERHEADERTYPE *inHeader) {
    memset(pkt, 0, sizeof(AVPacket));
    av_init_packet(pkt);

    if (inHeader) {
        pkt->data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
        pkt->size = inHeader->nFilledLen;
        pkt->pts = inHeader->nTimeStamp; //ingore it, we will compute it
    } else {
        pkt->data = NULL;
        pkt->size = 0;
        pkt->pts = AV_NOPTS_VALUE;
    }

#if DEBUG_PKT
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        ALOGV("pkt size:%d, pts:%lld", pkt->size, pkt->pts);
    } else {
        ALOGV("pkt size:%d, pts:N/A", pkt->size);
    }
#endif
}

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    ALOGD("Creating SoftAC3 component: %s", name);
    return new android::SoftAC3(name, callbacks, appData, component);
}
