/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "VideoFilter.h"

VideoFilter::VideoFilter() : nextFilter(NULL) {}

void VideoFilter::sendFrame(std::unique_ptr<av::Frame>&& frame) {
    if (nextFilter != NULL) {
        nextFilter->onFrame(std::move(frame));
    }
}
TemporalNRFilter::TemporalNRFilter() {}

void TemporalNRFilter::init(int temporalDistance, int threshold, bool interlaced) {
    NFRAMES_ = temporalDistance * 2 + 1;
    DIFFMAX_ = threshold;
    interlaced_ = interlaced;

    if (NFRAMES_ > MAX_NFRAMES) {
        THROW(InvalidOperationException, "TemporalNRFilter最大枚数を超えています");
    }
}
/* virtual */ void TemporalNRFilter::start() {
    //
}
/* virtual */ void TemporalNRFilter::onFrame(std::unique_ptr<av::Frame>&& frame) {
    int format = (*frame)()->format;
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUV420P12LE:
    case AV_PIX_FMT_YUV420P14LE:
    case AV_PIX_FMT_YUV420P16LE:
        break;
    default:
        THROW(FormatException, "未対応フォーマットです");
    }

    frames_.emplace_back(std::move(frame));

    int half = (NFRAMES_ + 1) / 2;
    if (frames_.size() < half) {
        return;
    }

    AVFrame* frames[MAX_NFRAMES];
    for (int i = 0, f = (int)frames_.size() - NFRAMES_; i < NFRAMES_; ++i, ++f) {
        frames[i] = (*frames_[std::max(f, 0)])();
    }
    sendFrame(TNRFilter(frames, frames_[frames_.size() - half]->frameIndex_));

    if (frames_.size() >= NFRAMES_) {
        frames_.pop_front();
    }
}
/* virtual */ void TemporalNRFilter::finish() {
    int half = NFRAMES_ / 2;
    AVFrame* frames[MAX_NFRAMES];

    while (frames_.size() > half) {
        for (int i = 0; i < NFRAMES_; ++i) {
            frames[i] = (*frames_[std::min(i, (int)frames_.size() - 1)])();
        }
        sendFrame(TNRFilter(frames, frames_[half]->frameIndex_));

        frames_.pop_front();
    }
}

std::unique_ptr<av::Frame> TemporalNRFilter::TNRFilter(AVFrame** frames, int frameIndex) {
    auto dstframe = std::unique_ptr<av::Frame>(new av::Frame(frameIndex));

    AVFrame* top = frames[0];
    AVFrame* dst = (*dstframe)();

    // フレームのプロパティをコピー
    av_frame_copy_props(dst, top);

    // メモリサイズに関する情報をコピー
    dst->format = top->format;
    dst->width = top->width;
    dst->height = top->height;

    // メモリ確保
    if (av_frame_get_buffer(dst, 64) != 0) {
        THROW(RuntimeException, "failed to allocate frame buffer");
    }

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)(top->format));
    int thresh = DIFFMAX_ << (desc->comp[0].depth - 8);

    float kernel[MAX_NFRAMES];
    for (int i = 0; i < NFRAMES_; ++i) {
        kernel[i] = 1;
    }

    if (desc->comp[0].depth > 8) {
        filterKernel<uint16_t>(frames, dst, interlaced_, thresh, kernel);
    } else {
        filterKernel<uint8_t>(frames, dst, interlaced_, thresh, kernel);
    }

    return std::move(dstframe);
}
CudaTemporalNRFilter::CudaTemporalNRFilter() : filter_(NULL), frame_(-1) {}

void CudaTemporalNRFilter::init(int temporalDistance, int threshold, int batchSize, int interlaced) {
    filter_ = cudaTNRCreate(temporalDistance, threshold, batchSize, interlaced);
    if (filter_ == NULL) {
        THROW(RuntimeException, "cudaTNRCreateに失敗");
    }
}
/* virtual */ void CudaTemporalNRFilter::start() {}
/* virtual */ void CudaTemporalNRFilter::onFrame(std::unique_ptr<av::Frame>&& frame) {
    if (filter_ == NULL) {
        THROW(InvalidOperationException, "initを呼んでください");
    }
    if (cudaTNRSendFrame(filter_, (*frame)()) != 0) {
        THROW(RuntimeException, "cudaTNRSendFrameに失敗");
    }
    frameQueue_.emplace_back(std::move(frame));
    while (cudaTNRRecvFrame(filter_, frame_()) == 0) {
        av_frame_copy_props(frame_(), (*frameQueue_.front())());
        frame_.frameIndex_ = frameQueue_.front()->frameIndex_;
        frameQueue_.pop_front();
        sendFrame(std::unique_ptr<av::Frame>(new av::Frame(frame_)));
    }
}
/* virtual */ void CudaTemporalNRFilter::finish() {
    if (filter_ == NULL) {
        THROW(InvalidOperationException, "initを呼んでください");
    }
    if (cudaTNRFinish(filter_) != 0) {
        THROW(RuntimeException, "cudaTNRFinishに失敗");
    }
    while (cudaTNRRecvFrame(filter_, frame_()) == 0) {
        if (frameQueue_.size() == 0) {
            THROW(RuntimeException, "フィルタでフレームが増加しました");
        }
        av_frame_copy_props(frame_(), (*frameQueue_.front())());
        frame_.frameIndex_ = frameQueue_.front()->frameIndex_;
        frameQueue_.pop_front();
        sendFrame(std::unique_ptr<av::Frame>(new av::Frame(frame_)));
    }
    if (frameQueue_.size() != 0) {
        THROW(RuntimeException, "フィルタでフレームが減少しました");
    }
}
