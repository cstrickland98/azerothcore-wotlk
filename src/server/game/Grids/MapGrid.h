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

#ifndef MAP_GRID_H
#define MAP_GRID_H

#include "GridCell.h"
#include "GridReference.h"
#include "MapThreading.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

class GridTerrainData;

template
<
    class GRID_OBJECT_TYPES,
    class FAR_VISIBLE_OBJECT_TYPES
>
class MapGrid
{
public:
    typedef GridCell<GRID_OBJECT_TYPES, FAR_VISIBLE_OBJECT_TYPES> GridCellType;

    MapGrid(uint16 const x, uint16 const y)
        : _x(x), _y(y), _objectDataLoaded(false), _terrainData(nullptr) { }

    ~MapGrid()
    {
        for (auto& cellX : _cells)
            for (auto& cellY : cellX)
                delete cellY.load(std::memory_order_relaxed);
    }

    MapGrid(MapGrid const&) = delete;
    MapGrid& operator=(MapGrid const&) = delete;

    // Unique identifier for grid
    uint32 GetId() const { return _y * MAX_NUMBER_OF_GRIDS + _x; }

    uint16 GetX() const { return _x; }
    uint16 GetY() const { return _y; }

    bool IsObjectDataLoaded() const { return _objectDataLoaded; }
    void SetObjectDataLoaded() { _objectDataLoaded = true; }

    template<class SPECIFIC_OBJECT> void AddGridObject(uint16 const x, uint16 const y, SPECIFIC_OBJECT* obj)
    {
        GetOrCreateCell(x, y).AddGridObject(obj);
    }

    template<class SPECIFIC_OBJECT> void RemoveGridObject(uint16 const x, uint16 const y, SPECIFIC_OBJECT* obj)
    {
        GetOrCreateCell(x, y).RemoveGridObject(obj);
    }

    template<class SPECIFIC_OBJECT> void AddFarVisibleObject(uint16 const x, uint16 const y, SPECIFIC_OBJECT* obj)
    {
        ChangeFarVisible([this, x, y, obj]() { GetOrCreateCell(x, y).AddFarVisibleObject(obj); });
    }

    template<class SPECIFIC_OBJECT> void RemoveFarVisibleObject(uint16 const x, uint16 const y, SPECIFIC_OBJECT* obj)
    {
        ChangeFarVisible([this, x, y, obj]() { GetOrCreateCell(x, y).RemoveFarVisibleObject(obj); });
    }

    // Visit all cells
    template<class T, class TT>
    void VisitAllCells(TypeContainerVisitor<T, TT>& visitor)
    {
        for (auto& cellX : _cells)
        {
            for (auto& cellY : cellX)
            {
                GridCellType* gridCell = cellY.load(std::memory_order_acquire);
                if (!gridCell)
                    continue;

                gridCell->Visit(visitor);
            }
        }
    }

    // Visit single cell
    template<class T, class TT>
    void VisitCell(uint16 const x, uint16 const y, TypeContainerVisitor<T, TT>& visitor)
    {
        GridCellType* gridCell = GetCell(x, y);
        if (!gridCell)
            return;

        // In a parallel phase the far visible lists of this grid can be changed by the task of another grid: they
        // are read under the lock. The other lists are owned by this thread.
        if constexpr (std::is_same_v<TT, TypeVectorContainer<FAR_VISIBLE_OBJECT_TYPES>>)
        {
            if (MapThreading::InParallelPhase())
            {
                FarVisibleReadState& state = GetFarVisibleReadState();
                {
                    std::shared_lock<std::shared_mutex> guard(_cellLock);
                    FarVisibleReadScope const readScope(state);
                    gridCell->Visit(visitor);
                }

                if (!state.depth)
                    ApplyPendingFarVisibleChanges(state);

                return;
            }
        }

        gridCell->Visit(visitor);
    }

    void link(GridRefMgr<MapGrid<GRID_OBJECT_TYPES, FAR_VISIBLE_OBJECT_TYPES>>* pTo)
    {
        _gridReference.link(pTo, this);
    }

    GridTerrainData* GetTerrainData() const { return _terrainData.get(); }
    std::shared_ptr<GridTerrainData> GetTerrainDataSharedPtr() { return _terrainData; }
    void SetTerrainData(std::shared_ptr<GridTerrainData> terrainData) { _terrainData = terrainData; }

    uint32 GetCreatedCellsCount()
    {
        uint32 count = 0;
        for (auto& cellX : _cells)
        {
            for (auto& cellY : cellX)
            {
                if (!cellY.load(std::memory_order_acquire))
                    continue;

                ++count;
            }
        }
        return count;
    }

private:
    // Far visible list changes made while this thread reads far visible lists (of any grid) under a shared lock.
    // Writing under the shared lock would race the other readers, and waiting for a unique lock while holding a
    // shared one can deadlock, so they are applied once the outermost read ends.
    struct FarVisibleReadState
    {
        uint32 depth = 0;
        std::vector<std::function<void()>> pending;
    };

    struct FarVisibleReadScope
    {
        explicit FarVisibleReadScope(FarVisibleReadState& state) : _state(state) { ++_state.depth; }
        ~FarVisibleReadScope() { --_state.depth; }

        FarVisibleReadScope(FarVisibleReadScope const&) = delete;
        FarVisibleReadScope& operator=(FarVisibleReadScope const&) = delete;

        FarVisibleReadState& _state;
    };

    static FarVisibleReadState& GetFarVisibleReadState()
    {
        thread_local FarVisibleReadState state;
        return state;
    }

    static void ApplyPendingFarVisibleChanges(FarVisibleReadState& state)
    {
        if (state.pending.empty())
            return;

        std::vector<std::function<void()>> pending;
        pending.swap(state.pending);
        for (std::function<void()>& change : pending)
            change();

        // Keep the capacity for the next read
        pending.clear();
        if (state.pending.empty())
            state.pending.swap(pending);
    }

    template<class CHANGE>
    void ChangeFarVisible(CHANGE&& change)
    {
        if (!MapThreading::InParallelPhase())
        {
            change();
            return;
        }

        FarVisibleReadState& state = GetFarVisibleReadState();
        if (state.depth)
        {
            state.pending.emplace_back([this, change = std::forward<CHANGE>(change)]() mutable
            {
                std::unique_lock<std::shared_mutex> guard(_cellLock);
                change();
            });

            return;
        }

        std::unique_lock<std::shared_mutex> guard(_cellLock);
        change();
    }

    // Creates and returns the cell if not already created. In a parallel phase the tasks of two grids can create
    // cells of a third one at once: the first cell published wins, without a lock.
    GridCellType& GetOrCreateCell(uint16 const x, uint16 const y)
    {
        if (GridCellType* cell = GetCell(x, y))
            return *cell;

        GridCellType* created = new GridCellType();
        GridCellType* expected = nullptr;
        if (_cells[x][y].compare_exchange_strong(expected, created, std::memory_order_acq_rel, std::memory_order_acquire))
            return *created;

        delete created;
        return *expected;
    }

    GridCellType* GetCell(uint16 const x, uint16 const y)
    {
        ASSERT(x < MAX_NUMBER_OF_CELLS && y < MAX_NUMBER_OF_CELLS);
        return _cells[x][y].load(std::memory_order_acquire);
    }

    GridCellType const* GetCell(uint16 const x, uint16 const y) const
    {
        ASSERT(x < MAX_NUMBER_OF_CELLS && y < MAX_NUMBER_OF_CELLS);
        return _cells[x][y].load(std::memory_order_acquire);
    }

    uint16 _x;
    uint16 _y;

    bool _objectDataLoaded;
    mutable std::shared_mutex _cellLock;    // far visible lists, in a parallel phase
    std::array<std::array<std::atomic<GridCellType*>, MAX_NUMBER_OF_CELLS>, MAX_NUMBER_OF_CELLS> _cells{}; // N * N array, owned
    GridReference<MapGrid<GRID_OBJECT_TYPES, FAR_VISIBLE_OBJECT_TYPES>> _gridReference;

    // Instances will share a copy of the parent maps terrainData
    std::shared_ptr<GridTerrainData> _terrainData;
};

#endif
