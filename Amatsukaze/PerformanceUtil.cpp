/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "PerformanceUtil.h"


Stopwatch::Stopwatch()
    : sum() {
}

void Stopwatch::reset() {
    sum = std::chrono::microseconds(0);
}

void Stopwatch::start() {
    prev = std::chrono::high_resolution_clock::now();
}

double Stopwatch::current() {
    const auto cur = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(cur - prev).count() * 1e-6;
}

void Stopwatch::stop() {
    const auto cur = std::chrono::high_resolution_clock::now();
    sum += std::chrono::duration_cast<std::chrono::microseconds>(cur - prev);
    prev = cur;
}

double Stopwatch::getTotal() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(sum).count() * 1e-6;
}

double Stopwatch::getAndReset() {
    stop();
    double ret = getTotal();
    sum = std::chrono::microseconds(0);
    return ret;
}

void FpsPrinter::updateProgress(bool last) {
    sum.span += current.span;
    sum.count += current.count;
    times.push_back(current);
    current = TimeCount();

    if (last) {
        ctx.infoF("complete. %.2ffps (%dフレーム)", sum.count / sum.span, total);
    } else {
        float sumtime = 0;
        int sumcount = 0;
        for (int i = 0; i < (int)times.size(); i++) {
            sumtime += times[i].span;
            sumcount += times[i].count;
        }
        ctx.progressF("%d/%d %.2ffps", sum.count, total, sumcount / sumtime);

        if ((int)times.size() > navg) {
            times.pop_front();
        }
    }
}
FpsPrinter::FpsPrinter(AMTContext& ctx, int navg)
    : AMTObject(ctx)
    , navg(navg) {}

void FpsPrinter::start(int total_) {
    total = total_;
    sum = TimeCount();
    current = TimeCount();
    times.clear();

    sw.start();
}

void FpsPrinter::update(int count) {
    current.count += count;
    current.span += (float)sw.getAndReset();
    if (current.span >= 0.5f) {
        updateProgress(false);
    }
}

void FpsPrinter::stop() {
    current.span += (float)sw.getAndReset();
    updateProgress(true);
    sw.stop();
}
