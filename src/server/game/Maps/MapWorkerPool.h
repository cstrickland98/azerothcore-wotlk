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

#ifndef ACORE_MAP_WORKER_POOL_H
#define ACORE_MAP_WORKER_POOL_H

#include "Define.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Helper threads of one map updated by several threads (MapUpdate.ThreadsPerMap > 1).
// The map's own update thread is worker 0; the pool owns workers 1..K-1, parked between runs.
class MapWorkerPool
{
public:
    explicit MapWorkerPool(uint32 helperThreads);
    ~MapWorkerPool();

    MapWorkerPool(MapWorkerPool const&) = delete;
    MapWorkerPool& operator=(MapWorkerPool const&) = delete;

    // Runs job(workerIndex) once on every worker, the calling thread being worker 0, and returns when all are done
    void Run(std::function<void(uint32)> const& job);

    [[nodiscard]] uint32 GetWorkerCount() const { return uint32(_threads.size()) + 1; }

private:
    void WorkerMain(uint32 workerIndex);

    std::vector<std::thread> _threads;
    std::mutex _lock;
    std::condition_variable _wake;
    std::condition_variable _done;
    std::function<void(uint32)> const* _job;
    uint64 _generation;
    uint32 _running;
    bool _stop;
};

#endif // ACORE_MAP_WORKER_POOL_H
