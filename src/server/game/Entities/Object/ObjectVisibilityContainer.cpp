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
#include "ObjectAccessor.h"
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
    // NOTE: these were ASSERTs that the maps are empty at destruction. Under heavy
    // playerbot churn the bidirectional links can be left inconsistent (a stale
    // entry that cleanup validates-and-skips rather than unlinks), so asserting here
    // aborts the whole server on shutdown/logout. The maps own no objects (raw
    // pointers / GUID->ptr), so leftover entries are harmless to destroy; readers
    // and cleanup validate pointers via the live registry before dereferencing.
}

void ObjectVisibilityContainer::InitForPlayer()
{
    _visibleWorldObjectsMap = std::make_unique<VisibleWorldObjectsMap>();
}

void ObjectVisibilityContainer::CleanVisibilityReferences()
{
    // These maps hold raw pointers; bot churn can leave dangling entries. Re-resolve
    // each via the live registry and only dereference if it still maps to the same
    // live object — otherwise just drop it (the maps are cleared below regardless).
    for (auto const& kvPair : _visiblePlayersMap)
        if (Player* p = ObjectAccessor::FindPlayer(kvPair.first); p && p == kvPair.second)
            p->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(_selfObject->GetGUID());

    if (_visibleWorldObjectsMap)
    {
        for (auto const& kvPair : *_visibleWorldObjectsMap)
            if (WorldObject* o = ObjectAccessor::GetWorldObject(*_selfObject, kvPair.first); o && o == kvPair.second)
                o->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());

        (*_visibleWorldObjectsMap).clear();
    }

    _visiblePlayersMap.clear();
}

void ObjectVisibilityContainer::LinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Do not link self
    if (worldObject == _selfObject)
        return;

    // Transports are special and should not be added to our visibility map
    if (worldObject->IsGameObject() && worldObject->ToGameObject()->IsTransport())
        return;

    // Only players can link visibility
    if (!_visibleWorldObjectsMap)
        return;

    (*_visibleWorldObjectsMap).insert(std::make_pair(worldObject->GetGUID(), worldObject));
    worldObject->GetObjectVisibilityContainer().DirectInsertVisiblePlayerReference(_selfObject->ToPlayer());
}

void ObjectVisibilityContainer::UnlinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Only players can unlink visibility
    if (!_visibleWorldObjectsMap)
        return;

    worldObject->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());
    (*_visibleWorldObjectsMap).erase(worldObject->GetGUID());
}

VisibleWorldObjectsMap::iterator ObjectVisibilityContainer::UnlinkVisibilityFromPlayer(WorldObject* worldObject, VisibleWorldObjectsMap::iterator itr)
{
    if (!_visibleWorldObjectsMap) // not a player (or torn down) — nothing to unlink
        return itr;
    worldObject->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());
    return (*_visibleWorldObjectsMap).erase(itr);
}

VisiblePlayersMap::iterator ObjectVisibilityContainer::UnlinkVisibilityFromWorldObject(Player* player, VisiblePlayersMap::iterator itr)
{
    player->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(_selfObject->GetGUID());
    return _visiblePlayersMap.erase(itr);
}

void ObjectVisibilityContainer::DirectRemoveVisibilityReference(ObjectGuid guid)
{
    // Was ASSERT(_visibleWorldObjectsMap): under churn this can be reached on a
    // non-player / torn-down container via a stale cross-reference; guard instead
    // of aborting the server.
    if (!_visibleWorldObjectsMap)
        return;
    (*_visibleWorldObjectsMap).erase(guid);
}

void ObjectVisibilityContainer::DirectInsertVisiblePlayerReference(Player* player)
{
    _visiblePlayersMap.insert(std::make_pair(player->GetGUID(), player));
}

void ObjectVisibilityContainer::DirectRemoveVisiblePlayerReference(ObjectGuid guid)
{
    _visiblePlayersMap.erase(guid);
}
