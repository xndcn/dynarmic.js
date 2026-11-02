// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Generic SpinLock implementation for targets without a dedicated one
// (WASM/Emscripten included). Uses GCC/Clang atomic builtins, which
// Emscripten lowers to single-threaded-friendly operations.

#include "dynarmic/common/spin_lock.h"

namespace Dynarmic {

void SpinLock::Lock() {
    int expected = 0;
    while (!__atomic_compare_exchange_n(&storage, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
    }
}

void SpinLock::Unlock() {
    __atomic_store_n(&storage, 0, __ATOMIC_RELEASE);
}

}  // namespace Dynarmic
