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
#include "ObjectGuid.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>

class Player;
class WorldObject;

// The set of world objects a player can see is stored as GUIDs (not raw pointers):
// readers re-resolve each GUID through the live object registry, so a freed object
// simply resolves to nullptr instead of leaving a dangling pointer to dereference.
typedef std::unordered_set<ObjectGuid> VisibleWorldObjectsSet;
typedef std::unordered_map<ObjectGuid, Player*> VisiblePlayersMap;

// Class that manages the visibility containers of a worldobject
class ObjectVisibilityContainer
{
public:
    ObjectVisibilityContainer(WorldObject* selfObject);
    ~ObjectVisibilityContainer();

    // Creates the _visibleWorldObjectsSet if we are a player
    void InitForPlayer();

    // Cleans up all visibility references from other worldobjects,
    // this is used before a worldobject is deleted to prevent any dangling references
    void CleanVisibilityReferences();

    void LinkWorldObjectVisibility(WorldObject* worldObject);
    void UnlinkWorldObjectVisibility(WorldObject* worldObject);

    // These helpers aren't ideal, but needed in a few spots for cleaning up references
    VisibleWorldObjectsSet::iterator UnlinkVisibilityFromPlayer(WorldObject* worldObject, VisibleWorldObjectsSet::iterator itr);
    VisiblePlayersMap::iterator UnlinkVisibilityFromWorldObject(Player* player, VisiblePlayersMap::iterator itr);

    // Returns a list of all players who can see us
    VisiblePlayersMap& GetVisiblePlayersMap() { return _visiblePlayersMap; }
    VisiblePlayersMap const& GetVisiblePlayersMap() const { return _visiblePlayersMap; }

    // Returns the GUIDs of all worldobjects we can see (resolve each via the live
    // registry before use — a stale GUID just resolves to nullptr).
    // Warning: This is for player objects only, all other objects will return a nullptr
    VisibleWorldObjectsSet* GetVisibleWorldObjectsSet()
    {
        if (!_visibleWorldObjectsSet)
            return nullptr;

        return _visibleWorldObjectsSet.get();
    }

    VisibleWorldObjectsSet const* GetVisibleWorldObjectsSet() const
    {
        if (!_visibleWorldObjectsSet)
            return nullptr;

        return _visibleWorldObjectsSet.get();
    }

private:

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

    // GUIDs of all worldobjects that are visible to us (including other players).
    // Only players contain this set, thus we will only allocate it as needed.
    std::unique_ptr<VisibleWorldObjectsSet> _visibleWorldObjectsSet;

    // List of players who are currently able to see this worldobject.
    // All worldobjects will contain this map
    VisiblePlayersMap _visiblePlayersMap;
};

#endif
