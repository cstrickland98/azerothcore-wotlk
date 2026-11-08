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

#include "MapThreading.h"
#include "Log.h"
#include "Map.h"
#include "Object.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 VIOLATIONS_LOGGED_PER_SITE = 20;

    struct ViolationSite
    {
        uint64 logged = 0;
        std::unordered_map<uint32 /*mapId*/, uint64> sinceLastSummary;
    };

    std::mutex ViolationLock;
    std::unordered_map<std::string, ViolationSite> ViolationSites;

    thread_local MapThreading::Context CurrentContext;
}

MapThreading::Context& MapThreading::Current()
{
    return CurrentContext;
}

bool MapThreading::InParallelPhase()
{
    return CurrentContext.map != nullptr;
}

bool MapThreading::IsWritable(WorldObject const* obj)
{
    Context const& context = CurrentContext;
    if (!context.map || !obj)
        return true;

    if (obj->FindMap() != context.map)
        return true;

    int32 gridX = 0;
    int32 gridY = 0;
    if (!Map::GetObjectGridCoord(obj, gridX, gridY))
        return true;

    return std::abs(gridX - context.gridX) <= 1 && std::abs(gridY - context.gridY) <= 1;
}

void MapThreading::ReportViolation(char const* site, WorldObject const* obj)
{
    Context const& context = CurrentContext;
    uint32 const mapId = context.map ? context.map->GetId() : 0;

    int32 gridX = -1;
    int32 gridY = -1;
    if (obj)
        Map::GetObjectGridCoord(obj, gridX, gridY);

    bool log = false;
    {
        std::lock_guard<std::mutex> guard(ViolationLock);
        ViolationSite& entry = ViolationSites[site ? site : "?"];
        ++entry.sinceLastSummary[mapId];
        if (entry.logged < VIOLATIONS_LOGGED_PER_SITE)
        {
            ++entry.logged;
            log = true;
        }
    }

    if (log)
        LOG_WARN("maps.threading", "Ownership violation at {}: object {} in grid [{},{}] written from the task of "
            "grid [{},{}] on map {}", site ? site : "?", obj ? obj->GetGUID().ToString() : "<null>", gridX, gridY,
            context.gridX, context.gridY, mapId);
}

std::string MapThreading::TakeViolationSummary(Map const* map)
{
    uint32 const mapId = map ? map->GetId() : 0;
    std::vector<std::pair<std::string, uint64>> counts;

    {
        std::lock_guard<std::mutex> guard(ViolationLock);
        for (auto& [site, entry] : ViolationSites)
        {
            auto itr = entry.sinceLastSummary.find(mapId);
            if (itr == entry.sinceLastSummary.end() || !itr->second)
                continue;

            counts.emplace_back(site, itr->second);
            itr->second = 0;
        }
    }

    if (counts.empty())
        return "none";

    std::sort(counts.begin(), counts.end(), [](auto const& a, auto const& b) { return a.second > b.second; });

    std::string summary;
    for (auto const& [site, count] : counts)
    {
        if (!summary.empty())
            summary += ", ";

        summary += site + "=" + std::to_string(count);
    }

    return summary;
}
