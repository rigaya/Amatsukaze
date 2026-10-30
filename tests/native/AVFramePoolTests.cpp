#include "../../Amatsukaze/RGYPoolAV.h"
#include "rgy_util.h"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>
RGY_DISABLE_WARNING_PUSH
RGY_DISABLE_WARNING_STR("-Wattributes")
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
RGY_DISABLE_WARNING_POP

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct FakeFrame { int payload = 0; };
struct Counters {
    int allocated = 0, unreferenced = 0, freed = 0;
    bool failAllocation = false, freedWithPayload = false;
} counters;
FakeFrame* AllocateFake() {
    if (counters.failAllocation) return nullptr;
    counters.allocated++;
    return new FakeFrame();
}
void UnrefFake(FakeFrame* frame) {
    counters.unreferenced++;
    frame->payload = 0;
}
void FreeFake(FakeFrame** frame) {
    if (!frame || !*frame) return;
    counters.freed++;
    if ((*frame)->payload != 0) counters.freedWithPayload = true;
    delete *frame;
    *frame = nullptr;
}
using FakePool = RGYPoolAV<FakeFrame, AllocateFake, UnrefFake, FreeFake>;
using AVPool = RGYPoolAV<AVFrame, av_frame_alloc, av_frame_unref, av_frame_free>;

void TestFakeReuse() {
    counters = Counters{};
    {
        FakePool pool;
        pool.returnFree(nullptr);
        FakeFrame* empty = nullptr;
        pool.returnFree(&empty);
        Check(counters.unreferenced == 0, "空ポインタを返却して参照解除された");
        auto first = pool.getFree();
        Check(first != nullptr, "初回の貸出に失敗");
        FakeFrame* original = first.get();
        first->payload = 73;
        first.reset();
        Check(counters.allocated == 1 && counters.unreferenced == 1 && counters.freed == 0,
            "返却時に参照解除されないか、オブジェクトが解放された");
        first = pool.getFree();
        Check(first.get() == original && first->payload == 0 && counters.allocated == 1,
            "返却済みオブジェクトが初期状態で再利用されない");
        auto second = pool.getFree();
        Check(second != nullptr && second.get() != first.get() && counters.allocated == 2,
            "同時貸出が同じオブジェクトを共有した");
        auto moved = std::move(second);
        Check(!second && moved != nullptr, "所有権移動に失敗");
        FakeFrame* raw = moved.release();
        raw->payload = 91;
        pool.returnFree(&raw);
        Check(raw == nullptr && counters.unreferenced == 2, "明示返却でポインタまたは参照が残った");
        first->payload = 17;
    }
    Check(counters.allocated == 2 && counters.freed == 2 && counters.unreferenced == 3
        && !counters.freedWithPayload, "プール破棄までの参照解除・解放回数が不正");
}

void TestFakeExceptionAndGrowth() {
    counters = Counters{};
    constexpr int total = 1057;
    {
        FakePool pool;
        bool caught = false;
        try {
            auto frame = pool.getFree();
            frame->payload = 1;
            throw std::runtime_error("巻戻し確認");
        } catch (const std::runtime_error&) {
            caught = true;
        }
        Check(caught && counters.allocated == 1 && counters.unreferenced == 1 && counters.freed == 0,
            "例外巻戻しで貸出オブジェクトが返却されない");
        // キューの初期容量を超えて返却し、拡張と循環後も二重貸出・取り残しがないことを確認する。
        std::vector<FakePool::Unique> borrowed;
        borrowed.reserve(total);
        for (int i = 0; i < total; i++) borrowed.push_back(pool.getFree());
        Check(counters.allocated == total, "同時貸出数と確保数が一致しない");
        borrowed.clear();
        Check(counters.unreferenced == total + 1, "大量返却で参照解除が不足した");
        for (int i = 0; i < total; i++) borrowed.push_back(pool.getFree());
        Check(counters.allocated == total, "返却済み容量を再利用できない");
    }
    Check(counters.allocated == total && counters.freed == total && counters.unreferenced == total * 2 + 1
        && !counters.freedWithPayload,
        "例外後またはキュー拡張後に解放漏れがある");
}

void TestFakeAllocationFailure() {
    counters = Counters{};
    {
        FakePool pool;
        counters.failAllocation = true;
        auto failed = pool.getFree();
        Check(!failed, "確保失敗で空以外のポインタを返した");
        counters.failAllocation = false;
        auto recovered = pool.getFree();
        Check(recovered != nullptr && counters.allocated == 1, "確保失敗後に再利用できない");
    }
    Check(counters.freed == 1 && counters.unreferenced == 1, "空の貸出を誤って解放した");
}

struct BufferDeleter {
    void operator()(AVBufferRef* buffer) const { av_buffer_unref(&buffer); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
std::unique_ptr<AVFrame, FrameDeleter> MakeImage() {
    std::unique_ptr<AVFrame, FrameDeleter> source(av_frame_alloc());
    Check(source != nullptr, "実画像のフレーム確保に失敗");
    source->format = AV_PIX_FMT_GRAY8;
    source->width = 32;
    source->height = 16;
    Check(av_frame_get_buffer(source.get(), 32) >= 0, "実画像の画素確保に失敗");
    source->data[0][0] = 77;
    source->pts = 12345;
    return source;
}
void CheckEmpty(const AVFrame* frame) {
    Check(frame->buf[0] == nullptr && frame->data[0] == nullptr && frame->width == 0
        && frame->height == 0 && frame->format == -1 && frame->pts == AV_NOPTS_VALUE,
        "再貸出フレームに画像参照またはプロパティが残った");
}

void TestRealFrameReferences() {
    AVPool pool;
    auto source = MakeImage();
    std::unique_ptr<AVBufferRef, BufferDeleter> witness(av_buffer_ref(source->buf[0]));
    Check(witness != nullptr, "画素バッファの観測用参照確保に失敗");
    auto first = pool.getFree(), second = pool.getFree();
    Check(first && second && first.get() != second.get(), "実フレームの同時貸出に失敗");
    Check(av_frame_ref(first.get(), source.get()) >= 0 && av_frame_ref(second.get(), first.get()) >= 0,
        "実画像の参照複製に失敗");
    Check(av_buffer_get_ref_count(witness.get()) == 4, "参照複製で画素バッファが共有されない");
    source.reset();
    Check(first->data[0][0] == 77 && second->pts == 12345
        && av_buffer_get_ref_count(witness.get()) == 3, "元フレーム解放後に画像参照が失われた");
    AVFrame* original = first.get();
    first.reset();
    Check(second->data[0][0] == 77 && av_buffer_get_ref_count(witness.get()) == 2,
        "一方の返却がもう一方の画素参照を壊した");
    auto reused = pool.getFree();
    Check(reused.get() == original, "実フレームの入れ物が再利用されない");
    CheckEmpty(reused.get());
    second.reset();
    Check(av_buffer_get_ref_count(witness.get()) == 1, "返却済みフレームに画素参照が残った");
}

void TestRealFrameException() {
    AVPool pool;
    auto source = MakeImage();
    std::unique_ptr<AVBufferRef, BufferDeleter> witness(av_buffer_ref(source->buf[0]));
    Check(witness != nullptr, "巻戻し確認用の参照確保に失敗");
    bool caught = false;
    try {
        auto frame = pool.getFree();
        Check(frame != nullptr && av_frame_ref(frame.get(), source.get()) >= 0, "巻戻し用フレームの準備に失敗");
        source.reset();
        throw std::runtime_error("巻戻し確認");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    Check(caught && av_buffer_get_ref_count(witness.get()) == 1, "例外巻戻しで画像参照が残った");
    auto frame = pool.getFree();
    CheckEmpty(frame.get());
}
} // 名前空間

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        { "再利用と同時貸出", TestFakeReuse },
        { "例外巻戻しとキュー拡張", TestFakeExceptionAndGrowth },
        { "確保失敗と復帰", TestFakeAllocationFailure },
        { "実画像の参照と返却", TestRealFrameReferences },
        { "実画像の例外巻戻し", TestRealFrameException },
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            fprintf(stdout, "成功: %s\n", test.name);
        } catch (const std::exception& exception) {
            fprintf(stderr, "失敗: %s: %s\n", test.name, exception.what());
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}
