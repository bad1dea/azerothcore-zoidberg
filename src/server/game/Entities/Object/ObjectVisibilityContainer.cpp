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
* the most important and mainly used container is 'VisibleWorldObjectsSet'
* which is only accessible for player objects. It stores GUIDs (not raw
* pointers): readers re-resolve each GUID through the live object registry, so a
* freed object resolves to nullptr instead of leaving a dangling pointer. The
* 'VisiblePlayersMap' still stores raw Player*, validated via the registry before
* use, since it feeds hot packet-delivery paths.
*/

ObjectVisibilityContainer::ObjectVisibilityContainer(WorldObject* selfObject) :
    _selfObject(selfObject)
{
}

ObjectVisibilityContainer::~ObjectVisibilityContainer()
{
    // NOTE: these were ASSERTs that the containers are empty at destruction. Under
    // heavy playerbot churn the bidirectional links can be left inconsistent (a stale
    // entry that cleanup validates-and-skips rather than unlinks), so asserting here
    // aborts the whole server on shutdown/logout. The world-objects side stores GUIDs
    // (no ownership) and the players side stores raw Player* validated via the live
    // registry before use, so leftover entries are harmless to destroy.
}

void ObjectVisibilityContainer::InitForPlayer()
{
    _visibleWorldObjectsSet = std::make_unique<VisibleWorldObjectsSet>();
}

void ObjectVisibilityContainer::CleanVisibilityReferences()
{
    // _visiblePlayersMap still holds raw Player*; bot churn can leave dangling entries,
    // so re-resolve each via the live registry and only dereference if it still maps to
    // the same live player — otherwise just drop it (cleared below regardless).
    for (auto const& kvPair : _visiblePlayersMap)
        if (Player* p = ObjectAccessor::FindPlayer(kvPair.first); p && p == kvPair.second)
            p->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(_selfObject->GetGUID());

    if (_visibleWorldObjectsSet)
    {
        // We see these objects by GUID; resolve each and drop our reverse-reference from
        // the live ones (a freed object resolves to nullptr and is simply skipped).
        for (ObjectGuid const& guid : *_visibleWorldObjectsSet)
            if (WorldObject* o = ObjectAccessor::GetWorldObject(*_selfObject, guid))
                o->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());

        _visibleWorldObjectsSet->clear();
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
    if (!_visibleWorldObjectsSet)
        return;

    _visibleWorldObjectsSet->insert(worldObject->GetGUID());
    worldObject->GetObjectVisibilityContainer().DirectInsertVisiblePlayerReference(_selfObject->ToPlayer());
}

void ObjectVisibilityContainer::UnlinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Only players can unlink visibility
    if (!_visibleWorldObjectsSet)
        return;

    worldObject->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());
    _visibleWorldObjectsSet->erase(worldObject->GetGUID());
}

VisibleWorldObjectsSet::iterator ObjectVisibilityContainer::UnlinkVisibilityFromPlayer(WorldObject* worldObject, VisibleWorldObjectsSet::iterator itr)
{
    if (!_visibleWorldObjectsSet) // not a player (or torn down) — nothing to unlink
        return itr;
    worldObject->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());
    return _visibleWorldObjectsSet->erase(itr);
}

VisiblePlayersMap::iterator ObjectVisibilityContainer::UnlinkVisibilityFromWorldObject(Player* player, VisiblePlayersMap::iterator itr)
{
    player->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(_selfObject->GetGUID());
    return _visiblePlayersMap.erase(itr);
}

void ObjectVisibilityContainer::DirectRemoveVisibilityReference(ObjectGuid guid)
{
    // Was ASSERT(_visibleWorldObjectsSet): under churn this can be reached on a
    // non-player / torn-down container via a stale cross-reference; guard instead
    // of aborting the server.
    if (!_visibleWorldObjectsSet)
        return;
    _visibleWorldObjectsSet->erase(guid);
}

void ObjectVisibilityContainer::DirectInsertVisiblePlayerReference(Player* player)
{
    _visiblePlayersMap.insert(std::make_pair(player->GetGUID(), player));
}

void ObjectVisibilityContainer::DirectRemoveVisiblePlayerReference(ObjectGuid guid)
{
    _visiblePlayersMap.erase(guid);
}
