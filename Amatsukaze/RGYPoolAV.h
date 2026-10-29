#pragma once

#include <memory>
#include "rgy_queue.h"

// 借りたオブジェクトは、必ずプールより先に返却する。
template<typename T, T* Talloc(), void Tunref(T*), void Tfree(T**)>
class RGYPoolAV {
    RGYQueueMPMP<T*> queue;

public:
    struct Deleter {
        RGYPoolAV* pool;
        void operator()(T* ptr) const { pool->returnFree(&ptr); }
    };
    using Unique = std::unique_ptr<T, Deleter>;

    RGYPoolAV() { queue.init(); }
    RGYPoolAV(const RGYPoolAV&) = delete;
    RGYPoolAV& operator=(const RGYPoolAV&) = delete;
    ~RGYPoolAV() {
        queue.close([](T** ptr) { Tfree(ptr); });
    }

    Unique getUnique(T* ptr) { return Unique(ptr, Deleter{this}); }
    Unique getFree() {
        T* ptr = nullptr;
        if (!queue.front_copy_and_pop_no_lock(&ptr) || ptr == nullptr) {
            ptr = Talloc();
        }
        return getUnique(ptr);
    }

    void returnFree(T** ptr) {
        if (ptr == nullptr || *ptr == nullptr) return;
        Tunref(*ptr);
        // キュー拡張に失敗した場合も、返却するオブジェクトを取り残さない。
        if (!queue.push(*ptr)) {
            Tfree(ptr);
        } else {
            *ptr = nullptr;
        }
    }
};
