#pragma once

#include "LogoScan.h"
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace logo {

// 検出の先頭画素列と停止中のデコーダを、同じ入力の生成に引き継ぐ。
class LogoDecodeSession {
public:
    using GenerationCallback = std::function<bool(AVFrame*, int, int, int64_t)>;

    LogoDecodeSession(AMTContext& context, const tstring& source, int service)
        : reader(context), source(source), service(service) {}
    ~LogoDecodeSession() { stop(); }

    bool matches(const tstring& path, int id) const { return source == path && service == id; }
    bool hasStarted() const { return started; }
    size_t cachedFrames() const { return positions.size(); }

    void configure(AVFrame* frame, int x, int y, int width, int height,
        int scaleNum, int scaleDen, int frameCount, uint64_t availableBytes) {
        if (scaleNum != scaleDen || frameCount <= 0 || width <= 0 || height <= 0
            || ((x | y | width | height) & 1) != 0 || x < 0 || y < 0
            || x + width > frame->width || y + height > frame->height
            || (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)) return;
        const uint64_t pixels = uint64_t(width) * height;
        const uint64_t bytes = pixels * 3 / 2 * frameCount;
        // 検出の追加フィルタキャッシュと2GiBの余裕も留保する。
        if (bytes > (2ULL << 30) || availableBytes < bytes + pixels * frameCount + (2ULL << 30)) return;
        try {
            prefix.reserve(static_cast<size_t>(bytes));
            positions.reserve(frameCount);
            if (av_frame_ref(firstFrame(), frame) != 0) { disableCache(); return; }
            cropX = x;
            cropY = y;
            cropW = width;
            cropH = height;
            frameBytes = static_cast<size_t>(pixels * 3 / 2);
            frameLimit = frameCount;
            cacheEnabled = true;
        } catch (const std::bad_alloc&) {
            disableCache();
        }
    }

    void detect(const SimpleVideoReader::FirstFrameCallback& first,
        const SimpleVideoReader::FrameCallback& frame,
        const std::function<void(AVFrame*)>& configureCache) {
        started = true;
        decodeThread = std::thread([this, first, frame, configureCache]() {
            try {
                reader.readAll(source, service,
                    [&](AVStream* stream, AVFrame* picture) {
                        first(stream, picture);
                        configureCache(picture);
                    },
                    [&](AVFrame* picture) {
                        GenerationCallback continuation;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (cancelled) return false;
                            if (resume) continuation = generation;
                        }
                        if (continuation) return continuation(picture, 0, 0, reader.currentPos);
                        if (frame(picture)) {
                            capture(picture);
                            return true;
                        }
                        if (!cacheEnabled) return false;
                        GenerationCallback generate;
                        {
                            std::unique_lock<std::mutex> lock(mutex);
                            paused = true;
                            changed.notify_all();
                            changed.wait(lock, [&]() { return resume || cancelled; });
                            if (cancelled) return false;
                            generate = generation;
                        }
                        return generate(picture, 0, 0, reader.currentPos);
                    });
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                done = true;
            }
            changed.notify_all();
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&]() { return paused || done; });
        }
        if (!paused) finish();
    }

    bool generate(const tstring& path, int id, int x, int y, int width, int height,
        const SimpleVideoReader::FirstFrameCallback& first, const GenerationCallback& frame) {
        if (!started || !matches(path, id) || !cacheEnabled || positions.empty()
            || x < cropX || y < cropY || x + width > cropX + cropW || y + height > cropY + cropH) {
            stop();
            return false;
        }
        try {
            first(nullptr, firstFrame());
            AVFrame picture{};
            picture.width = cropW;
            picture.height = cropH;
            picture.format = firstFrame()->format;
            picture.linesize[0] = cropW;
            picture.linesize[1] = picture.linesize[2] = cropW / 2;
            const size_t yBytes = size_t(cropW) * cropH;
            for (size_t i = 0; i < positions.size(); i++) {
                picture.data[0] = prefix.data() + i * frameBytes;
                picture.data[1] = picture.data[0] + yBytes;
                picture.data[2] = picture.data[1] + yBytes / 4;
                if (!frame(&picture, cropX, cropY, positions[i])) {
                    stop();
                    return true;
                }
            }
            std::vector<uint8_t>().swap(prefix);
            std::vector<int64_t>().swap(positions);
            av_frame_unref(firstFrame());
            {
                std::lock_guard<std::mutex> lock(mutex);
                generation = frame;
                resume = true;
            }
            changed.notify_all();
            finish();
            return true;
        } catch (...) {
            stop();
            throw;
        }
    }

private:
    void disableCache() {
        cacheEnabled = false;
        std::vector<uint8_t>().swap(prefix);
        std::vector<int64_t>().swap(positions);
        av_frame_unref(firstFrame());
    }

    void capture(AVFrame* picture) {
        if (!cacheEnabled) return;
        if (picture->format != firstFrame()->format || picture->width != firstFrame()->width
            || picture->height != firstFrame()->height || positions.size() >= size_t(frameLimit)) {
            disableCache();
            return;
        }
        try {
            const size_t offset = prefix.size();
            prefix.resize(offset + frameBytes);
            uint8_t* dst = prefix.data() + offset;
            for (int plane = 0; plane < 3; plane++) {
                const int shift = plane == 0 ? 0 : 1;
                const int width = cropW >> shift;
                const int height = cropH >> shift;
                const uint8_t* src = picture->data[plane] + (cropY >> shift) * picture->linesize[plane] + (cropX >> shift);
                for (int row = 0; row < height; row++) {
                    memcpy(dst, src + row * picture->linesize[plane], width);
                    dst += width;
                }
            }
            positions.push_back(reader.currentPos);
        } catch (const std::bad_alloc&) {
            disableCache();
        }
    }

    void finish() {
        if (decodeThread.joinable()) decodeThread.join();
        if (error) std::rethrow_exception(error);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancelled = true;
        }
        changed.notify_all();
        if (decodeThread.joinable()) decodeThread.join();
        disableCache();
    }

    SimpleVideoReader reader;
    tstring source;
    int service;
    av::Frame firstFrame;
    std::thread decodeThread;
    std::mutex mutex;
    std::condition_variable changed;
    GenerationCallback generation;
    std::exception_ptr error;
    std::vector<uint8_t> prefix;
    std::vector<int64_t> positions;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0, frameLimit = 0;
    size_t frameBytes = 0;
    bool started = false, cacheEnabled = false, paused = false, done = false;
    bool resume = false, cancelled = false;
};

inline thread_local LogoDecodeSession* activeLogoDecodeSession = nullptr;

}
