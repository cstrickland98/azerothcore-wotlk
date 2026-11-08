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

#include "MapWorkerPool.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MapThreading.h"
#include <exception>

MapWorkerPool::MapWorkerPool(uint32 helperThreads) : _job(nullptr), _generation(0), _running(0), _stop(false)
{
    _threads.reserve(helperThreads);
    for (uint32 i = 0; i < helperThreads; ++i)
        _threads.emplace_back(&MapWorkerPool::WorkerMain, this, i + 1);
}

MapWorkerPool::~MapWorkerPool()
{
    {
        std::lock_guard<std::mutex> guard(_lock);
        _stop = true;
    }

    _wake.notify_all();

    for (std::thread& thread : _threads)
        if (thread.joinable())
            thread.join();
}

void MapWorkerPool::Run(std::function<void(uint32)> const& job)
{
    if (_threads.empty())
    {
        job(0);
        return;
    }

    {
        std::lock_guard<std::mutex> guard(_lock);
        _job = &job;
        _running = uint32(_threads.size());
        ++_generation;
    }

    _wake.notify_all();

    // The helpers reference job until they are done: wait for them even when worker 0 throws
    std::exception_ptr error;
    try
    {
        job(0);
    }
    catch (...)
    {
        error = std::current_exception();
    }

    {
        std::unique_lock<std::mutex> guard(_lock);
        _done.wait(guard, [this] { return _running == 0; });
        _job = nullptr;
    }

    if (error)
        std::rethrow_exception(error);
}

void MapWorkerPool::WorkerMain(uint32 workerIndex)
{
    LoginDatabase.WarnAboutSyncQueries(true);
    CharacterDatabase.WarnAboutSyncQueries(true);
    WorldDatabase.WarnAboutSyncQueries(true);

    uint64 seenGeneration = 0;

    for (;;)
    {
        std::function<void(uint32)> const* job = nullptr;

        {
            std::unique_lock<std::mutex> guard(_lock);
            _wake.wait(guard, [this, seenGeneration] { return _stop || _generation != seenGeneration; });

            if (_stop)
                return;

            seenGeneration = _generation;
            job = _job;
        }

        try
        {
            (*job)(workerIndex);
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("maps.threading", "Map worker {}: exception in a parallel phase: {}", workerIndex, e.what());
            MapThreading::Current() = MapThreading::Context();
        }
        catch (...)
        {
            LOG_ERROR("maps.threading", "Map worker {}: unknown exception in a parallel phase", workerIndex);
            MapThreading::Current() = MapThreading::Context();
        }

        bool last = false;
        {
            std::lock_guard<std::mutex> guard(_lock);
            last = --_running == 0;
        }

        if (last)
            _done.notify_one();
    }
}
