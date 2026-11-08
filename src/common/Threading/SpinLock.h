/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACORE_SPINLOCK_H
#define ACORE_SPINLOCK_H

#include <atomic>
#include <thread>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define ACORE_SPINLOCK_PAUSE() _mm_pause()
#else
#define ACORE_SPINLOCK_PAUSE() std::this_thread::yield()
#endif

namespace Acore
{
    // Minimal test-and-test-and-set spinlock (one byte), meant for very short
    // critical sections on objects that exist in large numbers, where a
    // std::mutex would be too heavy. Satisfies Lockable (std::lock_guard etc.).
    // Not recursive.
    class SpinLock
    {
    public:
        SpinLock() = default;
        SpinLock(SpinLock const&) = delete;
        SpinLock& operator=(SpinLock const&) = delete;

        void lock() noexcept
        {
            for (;;)
            {
                if (!_locked.exchange(true, std::memory_order_acquire))
                    return;

                while (_locked.load(std::memory_order_relaxed))
                    ACORE_SPINLOCK_PAUSE();
            }
        }

        bool try_lock() noexcept
        {
            return !_locked.load(std::memory_order_relaxed) && !_locked.exchange(true, std::memory_order_acquire);
        }

        void unlock() noexcept
        {
            _locked.store(false, std::memory_order_release);
        }

    private:
        std::atomic<bool> _locked{ false };
    };
}

#endif
