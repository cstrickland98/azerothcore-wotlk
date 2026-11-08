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
    _selfObject(selfObject)
{
}

ObjectVisibilityContainer::~ObjectVisibilityContainer()
{
    ASSERT(_visiblePlayersMap.empty());
    if (_visibleWorldObjectsMap)
        ASSERT((*_visibleWorldObjectsMap).empty());
}

void ObjectVisibilityContainer::InitForPlayer()
{
    _visibleWorldObjectsMap = std::make_unique<VisibleWorldObjectsMap>();
}

void ObjectVisibilityContainer::CleanVisibilityReferences()
{
    // Take our references out under the lock, then clean the other containers without holding it
    VisiblePlayersMap visiblePlayers;
    VisibleWorldObjectsMap visibleWorldObjects;
    {
        std::lock_guard<Acore::SpinLock> guard(_lock);
        visiblePlayers.swap(_visiblePlayersMap);
        if (_visibleWorldObjectsMap)
            visibleWorldObjects.swap(*_visibleWorldObjectsMap);
    }

    ObjectGuid const selfGuid = _selfObject->GetGUID();

    for (auto const& kvPair : visiblePlayers)
        kvPair.second->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(selfGuid);

    for (auto const& kvPair : visibleWorldObjects)
        kvPair.second->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(selfGuid);
}

void ObjectVisibilityContainer::LinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Do not link self
    if (worldObject == _selfObject)
        return;

    // Transports are special and should not be added to our visibility map
    if (worldObject->IsGameObject() && worldObject->ToGameObject()->IsTransport())
        return;

    {
        std::lock_guard<Acore::SpinLock> guard(_lock);

        // Only players can link visibility
        if (!_visibleWorldObjectsMap)
            return;

        _visibleWorldObjectsMap->insert(std::make_pair(worldObject->GetGUID(), worldObject));
    }

    worldObject->GetObjectVisibilityContainer().DirectInsertVisiblePlayerReference(_selfObject->ToPlayer());
}

void ObjectVisibilityContainer::UnlinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Only players can unlink visibility (set once at construction time, no lock needed for the check)
    if (!_visibleWorldObjectsMap)
        return;

    worldObject->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());

    std::lock_guard<Acore::SpinLock> guard(_lock);
    _visibleWorldObjectsMap->erase(worldObject->GetGUID());
}

void ObjectVisibilityContainer::UnlinkVisiblePlayer(Player* player)
{
    {
        std::lock_guard<Acore::SpinLock> guard(_lock);
        _visiblePlayersMap.erase(player->GetGUID());
    }

    player->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(_selfObject->GetGUID());
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
    ASSERT(_visibleWorldObjectsMap);
    std::lock_guard<Acore::SpinLock> guard(_lock);
    _visibleWorldObjectsMap->erase(guid);
}

void ObjectVisibilityContainer::DirectInsertVisiblePlayerReference(Player* player)
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    _visiblePlayersMap.insert(std::make_pair(player->GetGUID(), player));
}

void ObjectVisibilityContainer::DirectRemoveVisiblePlayerReference(ObjectGuid guid)
{
    std::lock_guard<Acore::SpinLock> guard(_lock);
    _visiblePlayersMap.erase(guid);
}
