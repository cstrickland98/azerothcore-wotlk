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

#ifndef _OBJECTVISIBILITYCONTAINER_H
#define _OBJECTVISIBILITYCONTAINER_H

#include "Common.h"
#include "Errors.h"
#include "ObjectGuid.h"
#include "SpinLock.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

class Player;
class WorldObject;

typedef std::unordered_map<ObjectGuid, WorldObject*> VisibleWorldObjectsMap;
typedef std::unordered_map<ObjectGuid, Player*> VisiblePlayersMap;

// Class that manages the visibility containers of a worldobject
// Thread safety: with intra-map threading the maps are written from several map threads
// (a link/unlink touches the player's map and the target's map). Every access goes through
// _lock, and a container never holds its lock while calling into another object, so two
// container locks are never nested. Callbacks of ForEach* run on a copy, outside the lock.
class ObjectVisibilityContainer
{
public:
    ObjectVisibilityContainer(WorldObject* selfObject);
    ~ObjectVisibilityContainer();

    // Creates the _visibleWorldObjectsMap map if we are a player
    void InitForPlayer();

    // Cleans up all visibility references from other worldobjects,
    // this is used before a worldobject is deleted to prevent any dangling references
    void CleanVisibilityReferences();

    void LinkWorldObjectVisibility(WorldObject* worldObject);
    void UnlinkWorldObjectVisibility(WorldObject* worldObject);

    // Removes player from the players who can see us, and us from the player's visible objects
    void UnlinkVisiblePlayer(Player* player);

    // True if at least one player can see us
    [[nodiscard]] bool HasVisiblePlayers() const;

    // True if we (a player) can see at least one worldobject. Always false for non-players.
    [[nodiscard]] bool HasVisibleWorldObjects() const;

    // True if we (a player) can see the worldobject with this guid. Always false for non-players.
    [[nodiscard]] bool IsWorldObjectVisible(ObjectGuid guid) const;

    // Calls worker(Player*) for all players who can see us.
    // Works on a snapshot taken under the lock, the worker runs without the lock held.
    template<typename Worker>
    void ForEachVisiblePlayer(Worker&& worker) const
    {
        ScratchBuffer<Player> scratch;
        std::vector<Player*>& players = scratch.Get();
        SnapshotVisiblePlayers(players);

        for (Player* player : players)
            worker(player);
    }

    // Calls worker(WorldObject*) for all worldobjects we (a player) can see. Does nothing for non-players.
    // Works on a snapshot taken under the lock, the worker runs without the lock held.
    template<typename Worker>
    void ForEachVisibleWorldObject(Worker&& worker) const
    {
        ScratchBuffer<WorldObject> scratch;
        std::vector<WorldObject*>& worldObjects = scratch.Get();
        SnapshotVisibleWorldObjects(worldObjects);

        for (WorldObject* worldObject : worldObjects)
            worker(worldObject);
    }

private:
    // Aborts when `object`, reached through one of our maps under `key`, was destroyed (or its memory reused)
    // while we still listed it: the dangling entry would make us write into freed memory.
    void AssertListedObjectAlive(WorldObject const* object, ObjectGuid key) const;

    // Aborts when this container was destroyed: a write reached it through a stale pointer.
    void AssertAlive(ObjectGuid key) const;

    // Copy the map's values under the lock. A map whose node chain was damaged by a stray write would be
    // iterated forever here, allocating until memory runs out: both abort as soon as the copy outgrows size().
    void SnapshotVisiblePlayers(std::vector<Player*>& out) const;
    void SnapshotVisibleWorldObjects(std::vector<WorldObject*>& out) const;

    // Per-thread pool of snapshot vectors, so ForEach* does not allocate in steady state
    // and stays safe when a worker re-enters ForEach* (each nesting level takes its own vector).
    template<typename T>
    class ScratchBuffer
    {
    public:
        ScratchBuffer()
        {
            std::vector<std::vector<T*>>& pool = Pool();
            if (!pool.empty())
            {
                _buffer = std::move(pool.back());
                pool.pop_back();
            }
        }

        ~ScratchBuffer()
        {
            _buffer.clear();
            Pool().push_back(std::move(_buffer));
        }

        ScratchBuffer(ScratchBuffer const&) = delete;
        ScratchBuffer& operator=(ScratchBuffer const&) = delete;

        std::vector<T*>& Get() { return _buffer; }

    private:
        static std::vector<std::vector<T*>>& Pool()
        {
            thread_local std::vector<std::vector<T*>> pool;
            return pool;
        }

        std::vector<T*> _buffer;
    };

    // Directly removes visibility reference. This is to be ONLY used as
    // a more efficient method for cleaning up visibility references.
    // Warning: Improper use will leave dangling references and result in crashes.
    void DirectRemoveVisibilityReference(ObjectGuid guid);

    // Directly inserts player visibility reference.
    // Warning: Improper use will leave dangling references and result in crashes.
    void DirectInsertVisiblePlayerReference(Player* player);

    // Directly removes player visibility reference.
    // Warning: Improper use will leave dangling references and result in crashes.
    void DirectRemoveVisiblePlayerReference(ObjectGuid guid);

    WorldObject* _selfObject;

    // ALIVE while the owner exists, DEAD once destroyed: freed memory keeps DEAD until it is reused,
    // so a stale pointer that reaches this container is caught by the writer, not by a later reader.
    static constexpr uint32 ALIVE = 0x4F564341;
    static constexpr uint32 DEAD = 0xDEADDEAD;
    std::atomic<uint32> _aliveMagic;

    // Protects _visibleWorldObjectsMap and _visiblePlayersMap
    mutable Acore::SpinLock _lock;

    // List of all worldobjects that are visible to us (including other players)
    // Only players contain this map, thus we will only allocate it as needed.
    std::unique_ptr<VisibleWorldObjectsMap> _visibleWorldObjectsMap;

    // List of players who are currently able to see this worldobject.
    // All worldobjects will contain this map
    VisiblePlayersMap _visiblePlayersMap;
};

#endif
