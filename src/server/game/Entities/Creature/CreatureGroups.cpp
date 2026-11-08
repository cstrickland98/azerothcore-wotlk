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

#include "CreatureGroups.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "MoveSplineInit.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "Timer.h"
#include "WaypointMgr.h"
#include <algorithm>

FormationMgr::~FormationMgr()
{
}

FormationMgr* FormationMgr::instance()
{
    static FormationMgr instance;
    return &instance;
}

void FormationMgr::AddCreatureToGroup(uint32 groupId, Creature* member)
{
    Map* map = member->FindMap();
    if (!map)
    {
        return;
    }

    std::lock_guard<Acore::SpinLock> guard(_holderLock);
    CreatureGroupHolderType::iterator itr = map->CreatureGroupHolder.find(groupId);

    //Add member to an existing group
    if (itr != map->CreatureGroupHolder.end())
    {
        LOG_DEBUG("entities.unit", "Group found: {}, inserting creature {}, Group InstanceID {}", groupId, member->GetGUID().ToString(), member->GetInstanceId());
        itr->second->AddMember(member);
    }
    //Create new group
    else
    {
        LOG_DEBUG("entities.unit", "Group not found: {}. Creating new group.", groupId);
        CreatureGroup* group = new CreatureGroup(groupId);
        map->CreatureGroupHolder[groupId] = group;
        group->AddMember(member);
    }
}

void FormationMgr::RemoveCreatureFromGroup(CreatureGroup* group, Creature* member)
{
    LOG_DEBUG("entities.unit", "Deleting member pointer to spawnId: {} from group {}", member->GetSpawnId(), group->GetId());

    uint32 const groupId = group->GetId();
    Map* map = member->FindMap();
    CreatureGroup::MemberList followers;
    {
        // Removal, emptiness check and deletion are one step: another member's task may remove the last member
        std::lock_guard<Acore::SpinLock> guard(_holderLock);
        if (group->RemoveMember(member, followers) && map)
        {
            LOG_DEBUG("entities.unit", "Deleting group with InstanceID {}", member->GetInstanceId());
            map->CreatureGroupHolder.erase(groupId);
            delete group;
        }
    }

    // A leader left: end its followers' formation movement. The group may be gone by now, this only uses values.
    if (!followers.empty() && map)
        CreatureGroup::ReleaseFollowers(map, groupId, member, followers);
}

void FormationMgr::LoadCreatureFormations()
{
    uint32 const oldMSTime = getMSTime();
    CreatureGroupMap.clear();
    CreatureGroupMembers.clear();

    //Get group data
    QueryResult result = WorldDatabase.Query("SELECT leaderGUID, memberGUID, dist, angle, groupAI, point_1, point_2 FROM creature_formations ORDER BY leaderGUID");
    if (!result)
    {
        LOG_WARN("server.loading", ">> Loaded 0 creatures in formations. DB table `creature_formations` is empty!");
        LOG_INFO("server.loading", " ");
        return;
    }

    uint32 count = 0;
    do
    {
        Field const* fields = result->Fetch();

        //Load group member data
        FormationInfo group_member;
        group_member.leaderGUID            = fields[0].Get<uint32>();
        ObjectGuid::LowType const memberGUID = fields[1].Get<uint32>();
        float const follow_dist             = fields[2].Get<float>();
        float const follow_angle            = fields[3].Get<float>() * (static_cast<float>(M_PI) / 180);
        group_member.groupAI               = fields[4].Get<uint16>();
        group_member.point_1               = fields[5].Get<uint16>();
        group_member.point_2               = fields[6].Get<uint16>();

        //If creature is group leader we may skip loading of dist/angle
        if (group_member.leaderGUID != memberGUID)
        {
            if (!group_member.HasGroupFlag(std::underlying_type_t<GroupAIFlags>(GroupAIFlags::GROUP_AI_FLAG_SUPPORTED)))
            {
                LOG_ERROR("sql.sql", "creature_formations table leader guid {} and member guid {} has unsupported GroupAI flag value ({}). Skipped", group_member.leaderGUID, memberGUID, group_member.groupAI);
                continue;
            }

            if (!group_member.HasGroupFlag(std::underlying_type_t<GroupAIFlags>(GroupAIFlags::GROUP_AI_FLAG_FOLLOW_LEADER)) && (follow_dist > 0.0f || follow_angle > 0.0f))
            {
                LOG_ERROR("sql.sql", "creature_formations table member guid {} and leader guid {} cannot have follow distance or follow angle because don't have GROUP_AI_FLAG_FOLLOW_LEADER flag. Values are not gonna be used", memberGUID, group_member.leaderGUID);
                group_member.follow_dist       = 0.0f;
                group_member.follow_angle      = 0.0f;
            }
            else
            {
                group_member.follow_dist       = follow_dist;
                group_member.follow_angle      = follow_angle;
            }
        }
        else
        {
            // Leader can have 0 AI flags - its allowed
            if (group_member.groupAI && !group_member.HasGroupFlag(std::underlying_type_t<GroupAIFlags>(GroupAIFlags::GROUP_AI_FLAG_SUPPORTED)))
            {
                LOG_ERROR("sql.sql", "creature_formations table leader guid {} and member guid {} has unsupported GroupAI flag value ({}). Skipped", group_member.leaderGUID, memberGUID, group_member.groupAI);
                continue;
            }

            group_member.follow_dist       = 0.0f;
            group_member.follow_angle      = 0.0f;
            if (follow_dist > 0.0f || follow_angle > 0.0f)
            {
                LOG_ERROR("sql.sql", "creature_formations table member guid {} and leader guid {} cannot have follow distance or follow angle. Values are not gonna be used", memberGUID, group_member.leaderGUID);
            }
        }

        if (!sObjectMgr->GetCreatureData(group_member.leaderGUID))
        {
            LOG_ERROR("sql.sql", "creature_formations table leader guid {} incorrect (does not exist). Skipped", group_member.leaderGUID);
            continue;
        }

        if (!sObjectMgr->GetCreatureData(memberGUID))
        {
            LOG_ERROR("sql.sql", "creature_formations table member guid {} incorrect (does not exist). Skipped", memberGUID);
            continue;
        }

        CreatureGroupMap[memberGUID] = group_member;
        CreatureGroupMembers[group_member.leaderGUID].push_back(memberGUID);
        ++count;
    } while (result->NextRow());

    LOG_INFO("server.loading", ">> Loaded {} Creatures In Formations in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    LOG_INFO("server.loading", " ");
}

namespace
{
    constexpr uint16 GroupFlag(GroupAIFlags flag)
    {
        return std::underlying_type_t<GroupAIFlags>(flag);
    }

    uint16 GetGroupAI(Creature const* member)
    {
        auto const itr = sFormationMgr->CreatureGroupMap.find(member->GetSpawnId());
        return itr != sFormationMgr->CreatureGroupMap.end() ? itr->second.groupAI : 0;
    }

    // Still a member of group groupId: a deferred write can find it out of the group, or the group deleted
    bool IsInGroup(Creature const* creature, uint32 groupId)
    {
        CreatureGroup const* group = creature->GetFormation();
        return group && group->GetId() == groupId;
    }

    // Runs write(member, other) for a formation member written from the task of another member (the leader
    // moving or resetting, a member engaging or evading). other is a second unit the write touches (the leader
    // whose follower list a formation movement change edits, an engaged target), or nullptr. In a parallel map
    // phase, when member or other lies in another grid task's area, the write runs in the serial step at the
    // phase end instead, with member and other resolved again by GUID; it is skipped when member left the world
    // or the group, and other is passed as nullptr when gone. write re-checks the member's state since that is a
    // read of the member. Outside a parallel phase this always runs inline: single-thread behaviour is unchanged.
    template <class Write>
    void WriteMember(Map* map, Creature* member, Unit* other, uint32 groupId, Write write)
    {
        if (map->CanWriteNow(member) && (!other || map->CanWriteNow(other)))
        {
            if (IsInGroup(member, groupId))
                write(member, other);

            return;
        }

        ObjectGuid const memberGuid = member->GetGUID();
        ObjectGuid const otherGuid = other ? other->GetGUID() : ObjectGuid::Empty;
        map->DeferToPhaseEnd([map, memberGuid, otherGuid, groupId, write]()
        {
            Creature* creature = map->GetCreature(memberGuid);
            if (!creature || !creature->IsInWorld() || !IsInGroup(creature, groupId))
                return;

            Unit* otherUnit = otherGuid ? ObjectAccessor::GetUnit(*creature, otherGuid) : nullptr;
            if (otherUnit && !otherUnit->IsInWorld())
                otherUnit = nullptr;

            write(creature, otherUnit);
        });
    }
}

bool CreatureGroup::IsEmpty() const
{
    std::lock_guard<Acore::SpinLock> guard(m_lock);
    return m_members.empty();
}

CreatureGroup::MemberList CreatureGroup::CopyMembers(Creature** leader /*= nullptr*/) const
{
    std::lock_guard<Acore::SpinLock> guard(m_lock);
    if (leader)
        *leader = m_leader;

    return MemberList(m_members.begin(), m_members.end());
}

void CreatureGroup::AddMember(Creature* member)
{
    LOG_DEBUG("entities.unit", "CreatureGroup::AddMember: Adding unit {}.", member->GetGUID().ToString());

    {
        std::lock_guard<Acore::SpinLock> guard(m_lock);

        //Check if it is a leader
        if (member->GetSpawnId() == m_groupID)
        {
            LOG_DEBUG("entities.unit", "Unit {} is formation leader. Adding group.", member->GetGUID().ToString());
            m_leader = member;
        }

        m_members[member] = sFormationMgr->CreatureGroupMap.find(member->GetSpawnId())->second;
    }

    member->SetFormation(this);
}

bool CreatureGroup::RemoveMember(Creature* member, MemberList& followers)
{
    bool empty;
    {
        std::lock_guard<Acore::SpinLock> guard(m_lock);
        if (m_leader == member)
        {
            m_leader = nullptr;
            for (auto const& itr : m_members)
                if (itr.first != member)
                    followers.emplace_back(itr.first, itr.second);
        }

        m_members.erase(member);
        empty = m_members.empty();
    }

    member->SetFormation(nullptr);
    return empty;
}

void CreatureGroup::RemoveMember(Creature* member)
{
    // Read before: once member is out, another member's task may remove the last member and delete this group
    uint32 const groupId = m_groupID;
    MemberList followers;
    RemoveMember(member, followers);
    if (!followers.empty())
        ReleaseFollowers(member->GetMap(), groupId, member, followers);
}

void CreatureGroup::MemberEngagingTarget(Creature* member, Unit* target)
{
    uint16 const groupAI = GetGroupAI(member);
    Creature* leader = nullptr;
    MemberList const members = CopyMembers(&leader);
    if (member == leader)
    {
        if (!(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_MEMBER_ASSIST_LEADER)))
        {
            return;
        }
    }
    else if (!(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_LEADER_ASSIST_MEMBER)))
    {
        return;
    }

    Map* map = member->GetMap();
    for (auto const& itr : members)
    {
        Creature* pMember = itr.first;
        if (!pMember || pMember == member)
        {
            continue;
        }

        if (pMember == leader && !(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_LEADER_ASSIST_MEMBER)))
        {
            continue;
        }

        // Engaging also writes the target's threat and combat lists
        WriteMember(map, pMember, target, m_groupID, [](Creature* creature, Unit* victim)
        {
            if (!victim || !creature->IsAlive() || creature->GetVictim())
                return;

            if (creature->IsValidAttackTarget(victim))
                creature->EngageWithTarget(victim);
        });
    }
}

Unit* CreatureGroup::GetNewTargetForMember(Creature* member)
{
    uint16 const groupAI = GetGroupAI(member);
    if (!(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_ACQUIRE_NEW_TARGET_ON_EVADE)))
    {
        return nullptr;
    }

    Creature* leader = nullptr;
    MemberList const members = CopyMembers(&leader);
    if (member == leader)
    {
        if (!(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_MEMBER_ASSIST_LEADER)))
        {
            return nullptr;
        }
    }
    else if (!(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_LEADER_ASSIST_MEMBER)))
    {
        return nullptr;
    }

    Map* map = member->GetMap();
    for (auto const& itr : members)
    {
        Creature* pMember = itr.first;
        if (!pMember)
        {
            continue;
        }

        // A member in another grid task's area is not read: its victim is out of this task's reach as well
        if (pMember == member || !map->CanWriteNow(pMember) || !pMember->IsAlive() || !pMember->GetVictim())
        {
            continue;
        }

        if (pMember == leader && !(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_MEMBER_ASSIST_LEADER)))
        {
            continue;
        }

        if (member->IsValidAttackTarget(pMember->GetVictim()))
        {
            return pMember->GetVictim();
        }
    }

    return nullptr;
}

void CreatureGroup::MemberEvaded(Creature* member)
{
    uint16 const groupAI = GetGroupAI(member);
    if (!(groupAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_EVADE_MASK)))
    {
        return;
    }

    // Copy the member list first: Respawn() below takes the member out of the
    // world, which erases it from m_members and would invalidate this loop.
    Creature* leader = nullptr;
    MemberList const members = CopyMembers(&leader);
    Map* map = member->GetMap();
    uint32 const groupId = m_groupID;

    for (auto const& itr : members)
    {
        Creature* pMember = itr.first;
        // This should never happen
        if (!pMember || pMember == member)
            continue;

        uint16 const memberAI = itr.second.groupAI;
        if (!(memberAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_EVADE_MASK)))
            continue;

        // Evading or respawning ends the member's formation movement, which edits the leader's follower list
        WriteMember(map, pMember, leader, groupId, [memberAI, groupId](Creature* creature, Unit* /*leader*/)
        {
            // A previous member's Respawn() or EnterEvadeMode() may have removed
            // this one from the group already.
            if (!IsInGroup(creature, groupId) || creature->IsInEvadeMode())
                return;

            // EVADE_TOGETHER and RESPAWN_ON_EVADE are independent: living members evade, dead members respawn.
            if (creature->IsAlive())
            {
                if (!(memberAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_EVADE_TOGETHER)))
                    return;

                if (!creature->IsInCombat())
                    return;

                if (creature->IsAIEnabled)
                    if (CreatureAI* creatureAI = creature->AI())
                        creatureAI->EnterEvadeMode();
            }
            else
            {
                if (!(memberAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_RESPAWN_ON_EVADE)))
                    return;

                if ((memberAI & GroupFlag(GroupAIFlags::GROUP_AI_FLAG_DONT_RESPAWN_LEADER_ON_EVADE)) &&
                    creature->GetSpawnId() == groupId)
                    return;

                creature->Respawn();
            }
        });
    }

    RespawnRemovedMembers(map);
}

// In dynamic respawn mode a creature is removed from the world once its corpse decays, and RemoveFromWorld drops it
// from this group, so members waiting in the map's respawn store are no longer in m_members. Move their stored
// respawn time to now: ProcessRespawns() recreates them on its next check and they rejoin the group when loaded.
void CreatureGroup::RespawnRemovedMembers(Map* map)
{
    auto const membersItr = sFormationMgr->CreatureGroupMembers.find(m_groupID);
    if (membersItr == sFormationMgr->CreatureGroupMembers.end())
        return;

    std::vector<ObjectGuid::LowType> inWorldSpawnIds;
    {
        std::lock_guard<Acore::SpinLock> guard(m_lock);
        inWorldSpawnIds.reserve(m_members.size());
        for (auto const& itr : m_members)
            inWorldSpawnIds.push_back(itr.first->GetSpawnId());
    }

    time_t now = GameTime::GetGameTime().count();
    for (ObjectGuid::LowType spawnId : membersItr->second)
    {
        if (std::find(inWorldSpawnIds.begin(), inWorldSpawnIds.end(), spawnId) != inWorldSpawnIds.end())
            continue;

        auto const infoItr = sFormationMgr->CreatureGroupMap.find(spawnId);
        if (infoItr == sFormationMgr->CreatureGroupMap.end())
            continue;

        FormationInfo const& info = infoItr->second;
        if (!info.HasGroupFlag(GroupFlag(GroupAIFlags::GROUP_AI_FLAG_RESPAWN_ON_EVADE)))
            continue;

        if (spawnId == m_groupID && info.HasGroupFlag(GroupFlag(GroupAIFlags::GROUP_AI_FLAG_DONT_RESPAWN_LEADER_ON_EVADE)))
            continue;

        // Only creatures with a respawn still pending on this map: one without a stored time (alive, never loaded)
        // is left alone, one already due (evade cascade of the same tick) is left to ProcessRespawns().
        if (map->GetCreatureRespawnTime(spawnId) <= now)
            continue;

        map->SaveCreatureRespawnTime(spawnId, now);
    }
}

void CreatureGroup::FormationReset(bool dismiss, bool initMotionMaster)
{
    Creature* leader = nullptr;
    MemberList const members = CopyMembers(&leader);
    if (!members.empty() && !members.front().second.HasGroupFlag(GroupFlag(GroupAIFlags::GROUP_AI_FLAG_FOLLOW_LEADER)))
        return;

    if (initMotionMaster)
    {
        for (auto const& itr : members)
        {
            Creature* member = itr.first;
            if (!member || member == leader)
                continue;

            // Replacing the member's formation movement edits the leader's follower list
            WriteMember(member->GetMap(), member, leader, m_groupID, [dismiss](Creature* creature, Unit* /*leader*/)
            {
                if (!creature->IsAlive())
                    return;

                if (dismiss)
                    creature->GetMotionMaster()->MovementExpiredOnSlot(MOTION_SLOT_IDLE, false);
                else
                    creature->GetMotionMaster()->MoveIdle();

                LOG_DEBUG("entities.unit", "Set {} movement for member {}", dismiss ? "default" : "idle", creature->GetGUID().ToString());
            });
        }
    }

    m_Formed.store(!dismiss, std::memory_order_relaxed);
}

void CreatureGroup::LeaderStartedMoving()
{
    Creature* leader = nullptr;
    MemberList const members = CopyMembers(&leader);
    if (!leader)
        return;

    Map* map = leader->GetMap();
    for (auto const& itr : members)
    {
        Creature* member = itr.first;
        FormationInfo const& pFormationInfo = itr.second;
        if (member == leader || !pFormationInfo.HasGroupFlag(GroupFlag(GroupAIFlags::GROUP_AI_FLAG_FOLLOW_LEADER)))
            continue;

        float const followDist = pFormationInfo.follow_dist;
        float const followAngle = pFormationInfo.follow_angle;
        uint32 const point1 = pFormationInfo.point_1;
        uint32 const point2 = pFormationInfo.point_2;

        // MoveFormation registers the member as the leader's follower
        WriteMember(map, member, leader, m_groupID,
            [followDist, followAngle, point1, point2](Creature* creature, Unit* leaderUnit)
        {
            if (!leaderUnit || !creature->IsAlive() || creature->GetVictim())
                return;

            if (creature->HasUnitState(UNIT_STATE_NOT_MOVE) || creature->isPossessed() || creature->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED))
                return;

            if (!creature->HasUnitState(UNIT_STATE_FOLLOW_MOVE))
                creature->GetMotionMaster()->MoveFormation(leaderUnit, followDist, followAngle, point1, point2);
        });
    }
}

bool CreatureGroup::CanLeaderStartMoving() const
{
    // Reads only: the members' state is read under the member lock, no member is called into for a write
    std::lock_guard<Acore::SpinLock> guard(m_lock);
    for (auto const& itr : m_members)
    {
        if (itr.first && itr.first != m_leader && itr.first->IsAlive())
            if (itr.first->IsEngaged() || itr.first->IsInEvadeMode())
                return false;
    }

    return true;
}

void CreatureGroup::RemoveFormationMovement()
{
    Creature* leader = nullptr;
    MemberList members = CopyMembers(&leader);
    members.erase(std::remove_if(members.begin(), members.end(), [leader](auto const& itr)
    {
        return !itr.first || itr.first == leader;
    }), members.end());

    if (!members.empty())
        ReleaseFollowers(members.front().first->GetMap(), m_groupID, leader, members);
}

// Ends the formation movement of followers (members other than leader, which can be out of the group already)
void CreatureGroup::ReleaseFollowers(Map* map, uint32 groupId, Creature* leader, MemberList const& followers)
{
    for (auto const& itr : followers)
    {
        Creature* member = itr.first;
        if (!member || member == leader)
            continue;

        // Expiring the member's formation movement edits the leader's follower list
        WriteMember(map, member, leader, groupId, [](Creature* creature, Unit* /*leader*/)
        {
            if (creature->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_IDLE) == FORMATION_MOTION_TYPE)
                creature->GetMotionMaster()->MovementExpiredOnSlot(MOTION_SLOT_IDLE, false);
        });
    }
}

void CreatureGroup::DespawnFormation(Milliseconds timeToDespawn /*=0ms*/, Seconds forcedRespawnTimer /*=0s*/)
{
    // Copy the member list first: DespawnOrUnsummon() takes the member out of
    // the world, which erases it from m_members and would invalidate this loop.
    Creature* leader = nullptr;
    MemberList const members = CopyMembers(&leader);

    for (auto const& itr : members)
    {
        if (!itr.first)
            continue;

        // Despawning ends the member's formation movement, which edits the leader's follower list
        WriteMember(itr.first->GetMap(), itr.first, itr.first != leader ? leader : nullptr, m_groupID,
            [timeToDespawn, forcedRespawnTimer](Creature* creature, Unit* /*leader*/)
        {
            creature->DespawnOrUnsummon(timeToDespawn, forcedRespawnTimer);
        });
    }
}

void CreatureGroup::RespawnFormation(bool force)
{
    // Copy the member list first: Respawn() takes the member out of the world,
    // which erases it from m_members and would invalidate this loop.
    Creature* leader = nullptr;
    MemberList const members = CopyMembers(&leader);

    for (auto const& itr : members)
    {
        if (!itr.first)
            continue;

        WriteMember(itr.first->GetMap(), itr.first, itr.first != leader ? leader : nullptr, m_groupID,
            [force](Creature* creature, Unit* /*leader*/)
        {
            if (!creature->IsAlive())
                creature->Respawn(force);
        });
    }
}

bool CreatureGroup::IsFormationInCombat()
{
    std::lock_guard<Acore::SpinLock> guard(m_lock);
    for (auto const& itr : m_members)
    {
        if (itr.first && itr.first->IsInCombat())
        {
            return true;
        }
    }

    return false;
}

bool CreatureGroup::IsAnyMemberAlive(bool ignoreLeader /*= false*/)
{
    std::lock_guard<Acore::SpinLock> guard(m_lock);
    for (auto const& itr : m_members)
    {
        if (itr.first && itr.first->IsAlive())
        {
            if (!ignoreLeader || itr.first != m_leader)
            {
                return true;
            }
        }
    }

    return false;
}
