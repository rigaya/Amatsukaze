#pragma once

/**
* Amtasukaze Performance Utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <deque>
#include <chrono>
#include "StreamUtils.h"

class Stopwatch {
    std::chrono::microseconds sum;
    std::chrono::high_resolution_clock::time_point prev;
public:
    Stopwatch();

    void reset();

    void start();

    double current();

    void stop();

    double getTotal() const;

    double getAndReset();
};

class FpsPrinter : AMTObject {
    struct TimeCount {
        float span;
        int count;
    };

    Stopwatch sw;
    std::deque<TimeCount> times;
    int navg;
    int total;

    TimeCount sum;
    TimeCount current;

    void updateProgress(bool last);
public:
    FpsPrinter(AMTContext& ctx, int navg);

    void start(int total_);

    void update(int count);

    void stop();
};


