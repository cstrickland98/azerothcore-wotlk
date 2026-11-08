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
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <type_traits>

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
        GridCellType& cell = GetOrCreateCell(x, y);
        std::unique_lock<std::shared_mutex> guard = LockFarVisibleForWrite();
        cell.AddFarVisibleObject(obj);
    }

    template<class SPECIFIC_OBJECT> void RemoveFarVisibleObject(uint16 const x, uint16 const y, SPECIFIC_OBJECT* obj)
    {
        GridCellType& cell = GetOrCreateCell(x, y);
        std::unique_lock<std::shared_mutex> guard = LockFarVisibleForWrite();
        cell.RemoveFarVisibleObject(obj);
    }

    // Visit all cells
    template<class T, class TT>
    void VisitAllCells(TypeContainerVisitor<T, TT>& visitor)
    {
        for (auto& cellX : _cells)
        {
            for (auto& cellY : cellX)
            {
                if (!cellY)
                    continue;

                cellY->Visit(visitor);
            }
        }
    }

    // Visit single cell
    template<class T, class TT>
    void VisitCell(uint16 const x, uint16 const y, TypeContainerVisitor<T, TT>& visitor)
    {
        if (!MapThreading::InParallelPhase())
        {
            if (GridCellType* gridCell = GetCell(x, y))
                gridCell->Visit(visitor);

            return;
        }

        // In a parallel phase cells of this grid can be created, and its far visible lists changed, by the task of
        // another grid: the far visible lists are read under the lock, the other lists are owned by this thread
        std::shared_lock<std::shared_mutex> guard(_cellLock);
        GridCellType* gridCell = GetCell(x, y);
        if (!gridCell)
            return;

        if constexpr (std::is_same_v<TT, TypeVectorContainer<FAR_VISIBLE_OBJECT_TYPES>>)
        {
            FarVisibleReadScope const readScope(&_cellLock);
            gridCell->Visit(visitor);
        }
        else
        {
            guard.unlock();
            gridCell->Visit(visitor);
        }
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
                if (!cellY)
                    continue;

                ++count;
            }
        }
        return count;
    }

private:
    // Marks the far visible lock this thread reads under, so that a visitor changing a far visible list of the
    // same grid does not wait for itself
    struct FarVisibleReadScope
    {
        explicit FarVisibleReadScope(std::shared_mutex const* lock) : _previous(HeldFarVisibleLock())
        {
            HeldFarVisibleLock() = lock;
        }

        ~FarVisibleReadScope() { HeldFarVisibleLock() = _previous; }

        std::shared_mutex const* _previous;
    };

    static std::shared_mutex const*& HeldFarVisibleLock()
    {
        thread_local std::shared_mutex const* held = nullptr;
        return held;
    }

    std::unique_lock<std::shared_mutex> LockFarVisibleForWrite()
    {
        std::unique_lock<std::shared_mutex> guard(_cellLock, std::defer_lock);
        if (MapThreading::InParallelPhase() && HeldFarVisibleLock() != &_cellLock)
            guard.lock();

        return guard;
    }

    // Creates and returns the cell if not already created
    GridCellType& GetOrCreateCell(uint16 const x, uint16 const y)
    {
        if (!MapThreading::InParallelPhase() || HeldFarVisibleLock() == &_cellLock)
        {
            if (!_cells[x][y])
                _cells[x][y] = std::make_unique<GridCellType>();

            return *_cells[x][y];
        }

        {
            std::shared_lock<std::shared_mutex> guard(_cellLock);
            if (GridCellType* cell = GetCell(x, y))
                return *cell;
        }

        std::unique_lock<std::shared_mutex> guard(_cellLock);
        if (!_cells[x][y])
            _cells[x][y] = std::make_unique<GridCellType>();

        return *_cells[x][y];
    }

    GridCellType* GetCell(uint16 const x, uint16 const y)
    {
        ASSERT(x < MAX_NUMBER_OF_CELLS && y < MAX_NUMBER_OF_CELLS);
        return _cells[x][y].get();
    }

    GridCellType const* GetCell(uint16 const x, uint16 const y) const
    {
        ASSERT(x < MAX_NUMBER_OF_CELLS && y < MAX_NUMBER_OF_CELLS);
        return _cells[x][y].get();
    }

    uint16 _x;
    uint16 _y;

    bool _objectDataLoaded;
    mutable std::shared_mutex _cellLock;
    std::array<std::array<std::unique_ptr<GridCellType>, MAX_NUMBER_OF_CELLS>, MAX_NUMBER_OF_CELLS> _cells; // N * N array
    GridReference<MapGrid<GRID_OBJECT_TYPES, FAR_VISIBLE_OBJECT_TYPES>> _gridReference;

    // Instances will share a copy of the parent maps terrainData
    std::shared_ptr<GridTerrainData> _terrainData;
};

#endif
