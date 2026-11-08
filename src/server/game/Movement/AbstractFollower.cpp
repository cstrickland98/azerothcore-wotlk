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

#include "AbstractFollower.h"
#include "Unit.h"

void AbstractFollower::SetTarget(Unit* unit)
{
    Unit* old = _target.load(std::memory_order_relaxed);
    if (unit == old)
        return;

    if (old)
        old->FollowerRemoved(this);

    // a unit leaving the world refuses followers (its RemoveAllFollowers may already have run): follow nothing then
    _target.store(unit && unit->FollowerAdded(this) ? unit : nullptr, std::memory_order_relaxed);
}
