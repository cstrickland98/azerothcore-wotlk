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

#ifndef ACORE_MAP_THREADING_H
#define ACORE_MAP_THREADING_H

#include "Define.h"
#include <string>

class Map;
class WorldObject;

// Thread ownership for maps updated by several threads (MapUpdate.ThreadsPerMap > 1).
// A continent tick runs four parallel phases; in each, every task owns one grid of one
// colour and may write objects in that grid and its eight neighbours (always a different
// colour, hence idle). See modules/mod-playerbots/.agents/plans/bot-perf/bot-perf.INTRAMAP.DESIGN.md
namespace MapThreading
{
    struct Context
    {
        Map* map = nullptr;         // map whose parallel phase this thread is running; nullptr outside a phase
        uint32 workerIndex = 0;     // slot into the map's per-worker scratch; 0 is the map's own update thread
        int32 gridX = -1;           // grid of the task being run; -1 outside a task
        int32 gridY = -1;
    };

    Context& Current();             // thread_local
    bool InParallelPhase();         // Current().map != nullptr
    bool IsWritable(WorldObject const* obj);
    // Whether the running task owns the position: inside its grid, or within half a grid of it. True outside a phase.
    bool IsPositionWritable(float x, float y);
    void ReportViolation(char const* site, WorldObject const* obj);   // logs the first few per site, counts the rest; never aborts
    std::string TakeViolationSummary(Map const* map);                 // "site=count, ..." for this map since the last call, busiest first
}

#ifdef ACORE_MAP_THREAD_CHECKS
#define MT_ASSERT_WRITABLE(obj) \
    do { if (MapThreading::InParallelPhase() && !MapThreading::IsWritable(obj)) MapThreading::ReportViolation(__FUNCTION__, obj); } while (0)
#else
#define MT_ASSERT_WRITABLE(obj) ((void)0)
#endif

#endif // ACORE_MAP_THREADING_H
