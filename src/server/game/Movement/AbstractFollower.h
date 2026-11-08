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

#ifndef ACORE_ABSTRACTFOLLOWER_H
#define ACORE_ABSTRACTFOLLOWER_H

#include <atomic>

class Unit;

// A follower is a movement generator of unit A targeting unit B. With intra-map threading A's map task writes it,
// which may not be B's task: B keeps its follower set under a lock (Unit::FollowerAdded / FollowerRemoved) and clears
// its followers' target from its own task when it leaves the world (Unit::RemoveAllFollowers), so _target is atomic.
class AbstractFollower
{
public:
    explicit AbstractFollower(Unit* target = nullptr) { SetTarget(target); }
    virtual ~AbstractFollower() { SetTarget(nullptr); }

    void SetTarget(Unit* unit);
    [[nodiscard]] Unit* GetTarget() const { return _target.load(std::memory_order_relaxed); }

    // Called by the target, under its followers lock, while it leaves the world: no callback into the target
    void TargetRemoved() { _target.store(nullptr, std::memory_order_relaxed); }

private:
    std::atomic<Unit*> _target{ nullptr };
};

#endif
