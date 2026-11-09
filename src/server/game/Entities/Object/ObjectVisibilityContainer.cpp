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

#include "ObjectVisibilityContainer.h"
#include "MapThreading.h"
#include "Object.h"
#include "Player.h"

/*
* Some implementation notes:
* Non-player worldobjects do not have any concept of 'visibility', thus,
* the most important and mainly used map is 'VisibleWorldObjectsMap'
* which is only accessible for player objects. The 'VisiblePlayersMap'
* map is simply for managing the references so we can use direct pointers.
*/

ObjectVisibilityContainer::ObjectVisibilityContainer(WorldObject* selfObject) :
    _selfObject(selfObject), _aliveMagic(ALIVE), _closed(false)
{
}

ObjectVisibilityContainer::~ObjectVisibilityContainer()
{
    ASSERT(_visiblePlayersMap.empty());
    if (_visibleWorldObjectsMap)
        ASSERT((*_visibleWorldObjectsMap).empty());
    _aliveMagic.store(DEAD, std::memory_order_relaxed);
}

void ObjectVisibilityContainer::AssertListedObjectAlive(WorldObject const* object, ObjectGuid key) const
{
    if (object->GetObjectVisibilityContainer()._aliveMagic.load(std::memory_order_relaxed) != ALIVE)
        ABORT("ObjectVisibilityContainer: {} lists {} at {}, which was destroyed", _selfObject->GetGUID().ToString(), key.ToString(), static_cast<void const*>(object));
    if (object->GetGUID() != key)
        ABORT("ObjectVisibilityContainer: {} lists {} at {}, whose memory now holds {}", _selfObject->GetGUID().ToString(), key.ToString(), static_cast<void const*>(object), object->GetGUID().ToString());
}

void ObjectVisibilityContainer::AssertAlive(ObjectGuid key) const
{
    if (_aliveMagic.load(std::memory_order_relaxed) != ALIVE)
        ABORT("ObjectVisibilityContainer: write for {} into the destroyed container at {}", key.ToString(), static_cast<void const*>(this));
}

void ObjectVisibilityContainer::InitForPlayer()
{
    _visibleWorldObjectsMap = std::make_unique<VisibleWorldObjectsMap>();
}

void ObjectVisibilityContainer::CleanVisibilityReferences()
{
    CleanReferences(false);
}

void ObjectVisibilityContainer::CloseForRemoval()
{
    CleanReferences(true);
}

void ObjectVisibilityContainer::Reopen()
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    _closed = false;
}

void ObjectVisibilityContainer::CleanReferences(bool close)
{
    // Take our references out under the lock, then clean the other containers without holding it
    VisiblePlayersMap visiblePlayers;
    VisibleWorldObjectsMap visibleWorldObjects;
    {
        std::lock_guard<Acore::SpinLock> guard(_lock);
        if (close)
            _closed = true;
        visiblePlayers.swap(_visiblePlayersMap);
        if (_visibleWorldObjectsMap)
            visibleWorldObjects.swap(*_visibleWorldObjectsMap);
    }

    ObjectGuid const selfGuid = _selfObject->GetGUID();

    for (auto const& kvPair : visiblePlayers)
    {
        AssertListedObjectAlive(kvPair.second, kvPair.first);
        kvPair.second->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(selfGuid);
    }

    for (auto const& kvPair : visibleWorldObjects)
    {
        AssertListedObjectAlive(kvPair.second, kvPair.first);
        kvPair.second->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(selfGuid);
    }
}

void ObjectVisibilityContainer::LinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Do not link self
    if (worldObject == _selfObject)
        return;

    // Transports are special and should not be added to our visibility map
    if (worldObject->IsGameObject() && worldObject->ToGameObject()->IsTransport())
        return;

    // Only players can link visibility (set once at construction time, no lock needed for the check)
    if (!_visibleWorldObjectsMap)
        return;

    ObjectVisibilityContainer& other = worldObject->GetObjectVisibilityContainer();
    AssertAlive(worldObject->GetGUID());
    other.AssertAlive(_selfObject->GetGUID());
    MT_ASSERT_WRITABLE(_selfObject);

    std::scoped_lock guard(_lock, other._lock);
    if (_closed || other._closed)
        return;

    _visibleWorldObjectsMap->insert(std::make_pair(worldObject->GetGUID(), worldObject));
    other._visiblePlayersMap.insert(std::make_pair(_selfObject->GetGUID(), _selfObject->ToPlayer()));
}

void ObjectVisibilityContainer::UnlinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Only players can unlink visibility (set once at construction time, no lock needed for the check)
    if (!_visibleWorldObjectsMap || worldObject == _selfObject)
        return;

    ObjectVisibilityContainer& other = worldObject->GetObjectVisibilityContainer();
    AssertAlive(worldObject->GetGUID());
    other.AssertAlive(_selfObject->GetGUID());
    MT_ASSERT_WRITABLE(_selfObject);

    std::scoped_lock guard(_lock, other._lock);
    other._visiblePlayersMap.erase(_selfObject->GetGUID());
    _visibleWorldObjectsMap->erase(worldObject->GetGUID());
}

void ObjectVisibilityContainer::UnlinkVisiblePlayer(Player* player)
{
    if (player == _selfObject)
        return;

    ObjectVisibilityContainer& other = player->GetObjectVisibilityContainer();
    AssertAlive(player->GetGUID());
    other.AssertAlive(_selfObject->GetGUID());
    ASSERT(other._visibleWorldObjectsMap);
    MT_ASSERT_WRITABLE(player);

    std::scoped_lock guard(_lock, other._lock);
    _visiblePlayersMap.erase(player->GetGUID());
    other._visibleWorldObjectsMap->erase(_selfObject->GetGUID());
}

void ObjectVisibilityContainer::SnapshotVisiblePlayers(std::vector<Player*>& out) const
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    std::size_t const count = _visiblePlayersMap.size();
    ASSERT(count < 1000000, "ObjectVisibilityContainer: {} visible players on {}", count, _selfObject->GetGUID().ToString());
    out.reserve(count);
    for (auto const& kvPair : _visiblePlayersMap)
    {
        if (out.size() >= count)
            ABORT("ObjectVisibilityContainer: the visible players of {} (in world: {}) iterate past their size {}", _selfObject->GetGUID().ToString(), _selfObject->IsInWorld(), count);
        AssertListedObjectAlive(kvPair.second, kvPair.first);
        out.push_back(kvPair.second);
    }
}

void ObjectVisibilityContainer::SnapshotVisibleWorldObjects(std::vector<WorldObject*>& out) const
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    if (!_visibleWorldObjectsMap)
        return;

    std::size_t const count = _visibleWorldObjectsMap->size();
    ASSERT(count < 1000000, "ObjectVisibilityContainer: {} visible objects on {}", count, _selfObject->GetGUID().ToString());
    out.reserve(count);
    for (auto const& kvPair : *_visibleWorldObjectsMap)
    {
        if (out.size() >= count)
            ABORT("ObjectVisibilityContainer: the visible objects of {} (in world: {}) iterate past their size {}", _selfObject->GetGUID().ToString(), _selfObject->IsInWorld(), count);
        AssertListedObjectAlive(kvPair.second, kvPair.first);
        out.push_back(kvPair.second);
    }
}

bool ObjectVisibilityContainer::HasVisiblePlayers() const
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    return !_visiblePlayersMap.empty();
}

bool ObjectVisibilityContainer::HasVisibleWorldObjects() const
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    return _visibleWorldObjectsMap && !_visibleWorldObjectsMap->empty();
}

bool ObjectVisibilityContainer::IsWorldObjectVisible(ObjectGuid guid) const
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    return _visibleWorldObjectsMap && _visibleWorldObjectsMap->find(guid) != _visibleWorldObjectsMap->end();
}

void ObjectVisibilityContainer::DirectRemoveVisibilityReference(ObjectGuid guid)
{
    AssertAlive(guid);
    ASSERT(_visibleWorldObjectsMap);
    MT_ASSERT_WRITABLE(_selfObject);
    std::lock_guard<Acore::SpinLock> guard(_lock);
    _visibleWorldObjectsMap->erase(guid);
}

void ObjectVisibilityContainer::DirectRemoveVisiblePlayerReference(ObjectGuid guid)
{
    AssertAlive(guid);
    std::lock_guard<Acore::SpinLock> guard(_lock);
    _visiblePlayersMap.erase(guid);
}
