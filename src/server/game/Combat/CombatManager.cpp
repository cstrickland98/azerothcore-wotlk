/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Ported from TrinityCore with modifications for AzerothCore
 * Original authors:
 *   Treeston <treeston.mmoc@gmail.com> - Combat/Threat system rewrite (PR #19930)
 *   Shauren <shauren.trinity@gmail.com>
 *   Spp <spp@jorge.gr>
 */

#include "CombatManager.h"
#include "Containers.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Map.h"
#include "MapThreading.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ThreatManager.h"
#include "Unit.h"
#include "UnitAI.h"

/*static*/ bool CombatManager::CanBeginCombat(Unit const* a, Unit const* b)
{
    // Checks combat validity before initial reference creation.
    // For the combat to be valid...
    // ...the two units need to be different
    if (a == b)
        return false;
    // ...the two units need to be in the world
    if (!a->IsInWorld() || !b->IsInWorld())
        return false;
    // ...the two units need to both be alive
    if (!a->IsAlive() || !b->IsAlive())
        return false;
    // ...the two units need to be on the same map
    if (a->GetMap() != b->GetMap())
        return false;
    // ...the two units need to be in the same phase
    if (!a->InSamePhase(b))
        return false;
    if (a->HasUnitState(UNIT_STATE_EVADE) || b->HasUnitState(UNIT_STATE_EVADE))
        return false;
    if (a->HasUnitState(UNIT_STATE_IN_FLIGHT) || b->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;
    // ... both units must be allowed to enter combat
    if (a->IsCombatDisallowed() || b->IsCombatDisallowed())
        return false;
    // ...not friendly, unless one side is hostile (asymmetric aggressor wins)
    if ((a->IsFriendlyTo(b) || b->IsFriendlyTo(a)) && !a->IsHostileTo(b) && !b->IsHostileTo(a))
        return false;
    Player const* playerA = a->GetCharmerOrOwnerPlayerOrPlayerItself();
    Player const* playerB = b->GetCharmerOrOwnerPlayerOrPlayerItself();
    // ...neither of the two units must be (owned by) a player with .gm on
    if ((playerA && playerA->IsGameMaster()) || (playerB && playerB->IsGameMaster()))
        return false;
    return true;
}

void CombatReference::EndCombat()
{
    MT_ASSERT_WRITABLE(first);
    MT_ASSERT_WRITABLE(second);

    // sequencing matters here - AI might do nasty stuff, so make sure refs are in a consistent state before you hand off!

    // first, get rid of any threat that still exists...
    first->GetThreatMgr().ClearThreat(second);
    second->GetThreatMgr().ClearThreat(first);

    // ...then, remove the references from both managers...
    first->GetCombatManager().PurgeReference(second->GetGUID(), _isPvP);
    second->GetCombatManager().PurgeReference(first->GetGUID(), _isPvP);

    // ...update the combat state, which will potentially remove IN_COMBAT...
    bool const needFirstAI = first->GetCombatManager().UpdateOwnerCombatState();
    bool const needSecondAI = second->GetCombatManager().UpdateOwnerCombatState();

    // ...and if that happened, also notify the AI of it...
    if (needFirstAI)
        if (UnitAI* firstAI = first->GetAI())
            firstAI->JustExitedCombat();
    if (needSecondAI)
        if (UnitAI* secondAI = second->GetAI())
            secondAI->JustExitedCombat();

    // ...and finally clean up the reference object
    delete this;
}

void CombatReference::Refresh()
{
    bool needFirstAI = false, needSecondAI = false;
    if (_suppressFirst)
    {
        _suppressFirst = false;
        needFirstAI = first->GetCombatManager().UpdateOwnerCombatState();
    }
    if (_suppressSecond)
    {
        _suppressSecond = false;
        needSecondAI = second->GetCombatManager().UpdateOwnerCombatState();
    }

    if (needFirstAI)
        CombatManager::NotifyAICombat(first, second);
    if (needSecondAI)
        CombatManager::NotifyAICombat(second, first);
}

void CombatReference::SuppressFor(Unit* who)
{
    Suppress(who);
    if (who->GetCombatManager().UpdateOwnerCombatState())
        if (UnitAI* ai = who->GetAI())
            ai->JustExitedCombat();
}

bool PvPCombatReference::Update(uint32 tdiff)
{
    if (_combatTimer <= tdiff)
    {
        _combatTimer = 0;
        return false;
    }
    _combatTimer -= tdiff;
    return true;
}

void PvPCombatReference::RefreshTimer()
{
    _combatTimer = PVP_COMBAT_TIMEOUT;
}

CombatManager::~CombatManager()
{
    ASSERT(_pveRefs.empty(), "CombatManager::~CombatManager - %s: we still have %zu PvE combat references, one of them is with %s", _owner->GetGUID().ToString().c_str(), _pveRefs.size(), _pveRefs.begin()->first.ToString().c_str());
    ASSERT(_pvpRefs.empty(), "CombatManager::~CombatManager - %s: we still have %zu PvP combat references, one of them is with %s", _owner->GetGUID().ToString().c_str(), _pvpRefs.size(), _pvpRefs.begin()->first.ToString().c_str());
}

void CombatManager::Update(uint32 tdiff)
{
    // Update evade timer
    if (_evadeTimer)
    {
        if (_evadeTimer <= tdiff)
        {
            _evadeTimer = 0;
            // Timer expired - trigger evade check via AI
            if (UnitAI* ai = _owner->GetAI())
                ai->EvadeTimerExpired();
        }
        else
            _evadeTimer -= tdiff;
    }

    Map* map = _owner->FindMap();
    bool const parallelPhase = map && map->IsParallelPhase();
    auto it = _pvpRefs.begin(), end = _pvpRefs.end();
    while (it != end)
    {
        PvPCombatReference* const ref = it->second;
        if (ref->first == _owner && !ref->Update(tdiff)) // only update if we're the first unit involved (otherwise double decrement)
        {
            // the other unit is outside the running task's area: the expired reference ends in the serial step
            if (parallelPhase && !map->CanWriteNow(ref->second))
            {
                // queued unconditionally: RunOnOwner on the (writable) owner would run it inline
                map->DeferToPhaseEnd([map, ownerGuid = _owner->GetGUID(), otherGuid = it->first]()
                {
                    WorldObject* target = map->FindObjectForDeferredOp(ownerGuid);
                    Unit* unit = target ? target->ToUnit() : nullptr;
                    if (!unit)
                        return;

                    // combat may have been refreshed since the phase saw the timer run out: end only if still expired
                    auto const& refs = unit->GetCombatManager()._pvpRefs;
                    if (PvPCombatReference* expired = Acore::Containers::MapGetValuePtr(refs, otherGuid))
                        if (expired->first == unit && expired->IsExpired())
                            expired->EndCombat();
                });
                ++it;
                continue;
            }

            it = _pvpRefs.erase(it), end = _pvpRefs.end(); // remove it from our refs first to prevent invalidation
            ref->EndCombat(); // this will remove it from the other side
        }
        else
            ++it;
    }
}

bool CombatManager::DeferCombatEnd(std::function<void(Unit*)> fn) const
{
    Map* map = _owner->FindMap();
    if (!map || !map->IsParallelPhase())
        return false;

    // Only a worker of this map can queue for its serial step; anything else goes straight through as before
    if (MapThreading::Current().map != map)
        return false;

    if (_pveRefs.empty() && _pvpRefs.empty())
        return false;

    // A unit leaving the world (CleanupBeforeRemoveFromMap) never ends combat from its own thread: its partners are
    // handled in the serial step, where the unit still resolves through the removed-this-tick fallback
    bool anyFar = !_owner->IsInWorld() || _owner->IsDuringRemoveFromWorld() || !map->CanWriteNow(_owner);
    for (auto const& pair : _pveRefs)
        anyFar = anyFar || !map->CanWriteNow(pair.second->GetOther(_owner));
    for (auto const& pair : _pvpRefs)
        anyFar = anyFar || !map->CanWriteNow(pair.second->GetOther(_owner));
    if (!anyFar)
        return false;

    // Always queued, never run inline: running fn now would only call back into here. The serial step runs outside
    // the phase, so fn goes straight through there; EndCombat does not need the owner to be in the world.
    map->DeferToPhaseEnd([map, ownerGuid = _owner->GetGUID(), fn = std::move(fn)]()
    {
        if (WorldObject* target = map->FindObjectForDeferredOp(ownerGuid))
            if (Unit* unit = target->ToUnit())
                fn(unit);
    });
    return true;
}

bool CombatManager::HasPvECombat() const
{
    for (auto const& [guid, ref] : _pveRefs)
        if (!ref->IsSuppressedFor(_owner))
            return true;
    return false;
}

bool CombatManager::HasPvECombatWithPlayers() const
{
    for (std::pair<ObjectGuid const, CombatReference*> const& reference : _pveRefs)
        if (!reference.second->IsSuppressedFor(_owner) && reference.second->GetOther(_owner)->IsPlayer())
            return true;

    return false;
}

bool CombatManager::HasPvPCombat() const
{
    for (auto const& pair : _pvpRefs)
        if (!pair.second->IsSuppressedFor(_owner))
            return true;
    return false;
}

Unit* CombatManager::GetAnyTarget() const
{
    for (auto const& pair : _pveRefs)
        if (!pair.second->IsSuppressedFor(_owner))
            return pair.second->GetOther(_owner);
    for (auto const& pair : _pvpRefs)
        if (!pair.second->IsSuppressedFor(_owner))
            return pair.second->GetOther(_owner);
    return nullptr;
}

bool CombatManager::SetInCombatWith(Unit* who, bool addSecondUnitSuppressed)
{
    Map* map = _owner->FindMap();
    // a combat reference writes both units: with this unit outside the running task's area (the original caster
    // of a proc, a far master) it is made on this unit's owner, with who outside it on who's owner
    if (map && !map->CanWriteNow(_owner))
    {
        map->RunOnOwner(_owner, [whoGuid = who->GetGUID(), addSecondUnitSuppressed](WorldObject* target)
        {
            Unit* owner = target->ToUnit();
            Unit* whoUnit = ObjectAccessor::GetUnit(*target, whoGuid);
            if (owner && whoUnit)
                owner->GetCombatManager().SetInCombatWith(whoUnit, addSecondUnitSuppressed);
        });
        return IsInCombatWith(who);
    }
    if (map && !map->CanWriteNow(who))
    {
        map->RunOnOwner(who, [ownerGuid = _owner->GetGUID(), addSecondUnitSuppressed](WorldObject* target)
        {
            Unit* whoUnit = target->ToUnit();
            Unit* owner = ObjectAccessor::GetUnit(*target, ownerGuid);
            if (whoUnit && owner)
                owner->GetCombatManager().SetInCombatWith(whoUnit, addSecondUnitSuppressed);
        });
        return IsInCombatWith(who);
    }

    // Are we already in combat? If yes, refresh pvp combat
    if (PvPCombatReference* existingPvpRef = Acore::Containers::MapGetValuePtr(_pvpRefs, who->GetGUID()))
    {
        // a ref kept alive past a death runs out on its own - don't extend it or pull the corpse back into combat
        if (!_owner->IsAlive() || !who->IsAlive())
            return true;
        existingPvpRef->RefreshTimer();
        existingPvpRef->Refresh();
        return true;
    }
    if (CombatReference* existingPveRef = Acore::Containers::MapGetValuePtr(_pveRefs, who->GetGUID()))
    {
        existingPveRef->Refresh();
        return true;
    }

    // Otherwise, check validity...
    if (!CombatManager::CanBeginCombat(_owner, who))
        return false;

    // ...then create new reference
    CombatReference* ref;
    if (_owner->IsControlledByPlayer() && who->IsControlledByPlayer())
        ref = new PvPCombatReference(_owner, who);
    else
        ref = new CombatReference(_owner, who);

    if (addSecondUnitSuppressed)
        ref->Suppress(who);

    // ...and insert it into both managers
    PutReference(who->GetGUID(), ref);
    who->GetCombatManager().PutReference(_owner->GetGUID(), ref);

    // now, sequencing is important - first we update the combat state, which will set both units in combat and do non-AI combat start stuff
    bool const needSelfAI  = UpdateOwnerCombatState();
    bool const needOtherAI = who->GetCombatManager().UpdateOwnerCombatState();

    // then, we finally notify the AI (if necessary) and let it safely do whatever it feels like
    if (needSelfAI)
        NotifyAICombat(_owner, who);
    if (needOtherAI)
        NotifyAICombat(who, _owner);

    return IsInCombatWith(who);
}

bool CombatManager::IsInCombatWith(ObjectGuid const& guid) const
{
    return (_pveRefs.find(guid) != _pveRefs.end()) || (_pvpRefs.find(guid) != _pvpRefs.end());
}

bool CombatManager::IsInCombatWith(Unit const* who) const
{
    return IsInCombatWith(who->GetGUID());
}

void CombatManager::InheritCombatStatesFrom(Unit const* who)
{
    CombatManager const& mgr = who->GetCombatManager();
    for (auto& ref : mgr._pveRefs)
    {
        if (!IsInCombatWith(ref.first))
        {
            Unit* target = ref.second->GetOther(who);
            if ((_owner->IsImmuneToPC() && target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)) ||
                (_owner->IsImmuneToNPC() && !target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)))
                continue;
            SetInCombatWith(target);
        }
    }
    for (auto& ref : mgr._pvpRefs)
    {
        Unit* target = ref.second->GetOther(who);
        if ((_owner->IsImmuneToPC() && target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)) ||
            (_owner->IsImmuneToNPC() && !target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)))
            continue;
        SetInCombatWith(target);
    }
}

void CombatManager::EndCombatBeyondRange(float range, bool includingPvP)
{
    auto deferred = [range, includingPvP](Unit* unit)
    {
        unit->GetCombatManager().EndCombatBeyondRange(range, includingPvP);
    };
    if (DeferCombatEnd(deferred))
        return;

    auto it = _pveRefs.begin(), end = _pveRefs.end();
    while (it != end)
    {
        CombatReference* const ref = it->second;
        if (!ref->first->IsWithinDistInMap(ref->second, range))
        {
            it = _pveRefs.erase(it), end = _pveRefs.end(); // erase manually here to avoid iterator invalidation
            ref->EndCombat();
        }
        else
            ++it;
    }

    if (!includingPvP)
        return;

    auto it2 = _pvpRefs.begin(), end2 = _pvpRefs.end();
    while (it2 != end2)
    {
        CombatReference* const ref = it2->second;
        if (!ref->first->IsWithinDistInMap(ref->second, range))
        {
            it2 = _pvpRefs.erase(it2), end2 = _pvpRefs.end(); // erase manually here to avoid iterator invalidation
            ref->EndCombat();
        }
        else
            ++it2;
    }
}

void CombatManager::SuppressPvPCombat()
{
    for (auto const& pair : _pvpRefs)
        pair.second->Suppress(_owner);
    if (UpdateOwnerCombatState())
        if (UnitAI* ownerAI = _owner->GetAI())
            ownerAI->JustExitedCombat();
}

void CombatManager::RestartPvPCombatTimers()
{
    for (auto const& pair : _pvpRefs)
        pair.second->RefreshTimer();
}

void CombatManager::EndAllPvECombat()
{
    if (DeferCombatEnd([](Unit* unit) { unit->GetCombatManager().EndAllPvECombat(); }))
        return;

    // cannot have threat without combat
    _owner->GetThreatMgr().RemoveMeFromThreatLists();
    _owner->GetThreatMgr().ClearAllThreat();
    while (!_pveRefs.empty())
        _pveRefs.begin()->second->EndCombat();
}

void CombatManager::RevalidateCombat()
{
    if (DeferCombatEnd([](Unit* unit) { unit->GetCombatManager().RevalidateCombat(); }))
        return;

    // EndCombat's combat-exit handling (AI, scripts, aura removal) can end or create more of our refs, so no
    // iterator may be held across it: collect the invalid guids first, then look each one up again
    auto endInvalidCombat = [this](auto& refs)
    {
        std::vector<ObjectGuid> invalid;
        for (auto const& [guid, ref] : refs)
            if (!CanBeginCombat(_owner, ref->GetOther(_owner)))
                invalid.push_back(guid);

        for (ObjectGuid const& guid : invalid)
            if (auto* ref = Acore::Containers::MapGetValuePtr(refs, guid))
                if (!CanBeginCombat(_owner, ref->GetOther(_owner)))
                    ref->EndCombat();
    };

    endInvalidCombat(_pveRefs);
    endInvalidCombat(_pvpRefs);
}

void CombatManager::EndAllPvPCombat()
{
    if (DeferCombatEnd([](Unit* unit) { unit->GetCombatManager().EndAllPvPCombat(); }))
        return;

    while (!_pvpRefs.empty())
        _pvpRefs.begin()->second->EndCombat();
}

/*static*/ void CombatManager::NotifyAICombat(Unit* me, Unit* other)
{
    if (UnitAI* ai = me->GetAI())
        ai->JustEnteredCombat(other);
}

void CombatManager::PutReference(ObjectGuid const& guid, CombatReference* ref)
{
    MT_ASSERT_WRITABLE(_owner);
    if (ref->_isPvP)
    {
        auto& inMap = _pvpRefs[guid];
        ASSERT(!inMap, "Duplicate combat state at %p being inserted for %s vs %s - memory leak!", (void*)ref, _owner->GetGUID().ToString().c_str(), guid.ToString().c_str());
        inMap = static_cast<PvPCombatReference*>(ref);
    }
    else
    {
        auto& inMap = _pveRefs[guid];
        ASSERT(!inMap, "Duplicate combat state at %p being inserted for %s vs %s - memory leak!", (void*)ref, _owner->GetGUID().ToString().c_str(), guid.ToString().c_str());
        inMap = ref;
    }
}

void CombatManager::PurgeReference(ObjectGuid const& guid, bool pvp)
{
    MT_ASSERT_WRITABLE(_owner);
    if (pvp)
        _pvpRefs.erase(guid);
    else
        _pveRefs.erase(guid);
}

bool CombatManager::UpdateOwnerCombatState() const
{
    bool const combatState = HasCombat();
    if (combatState == _owner->IsInCombat())
        return false;

    if (combatState)
    {
        _owner->SetUnitFlag(UNIT_FLAG_IN_COMBAT);
        _owner->AtEnterCombat();
        if (!_owner->IsCreature())
            _owner->AtEngage(GetAnyTarget());

        if (Player* player = _owner->ToPlayer())
            sScriptMgr->OnPlayerEnterCombat(player, GetAnyTarget());
    }
    else
    {
        _owner->RemoveUnitFlag(UNIT_FLAG_IN_COMBAT);
        _owner->AtExitCombat();
        if (!_owner->IsCreature())
            _owner->AtDisengage();

        if (Player* player = _owner->ToPlayer())
            sScriptMgr->OnPlayerLeaveCombat(player);
    }

    if (Unit* master = _owner->GetCharmerOrOwner())
    {
        // the charmer/owner can be far, in another task's area: its pet-in-combat flag is recomputed on its owner
        Map* map = _owner->FindMap();
        if (!map || map->CanWriteNow(master))
            master->UpdatePetCombatState();
        else
            map->RunOnOwner(master, [](WorldObject* target)
            {
                if (Unit* unit = target->ToUnit(); unit && !unit->IsPet())
                    unit->UpdatePetCombatState();
            });
    }

    return true;
}

void CombatManager::SetEvadeState(EvadeState state)
{
    if (_evadeState == state)
        return;

    if (state == EVADE_STATE_NONE)
        _evadeTimer = 0;

    _evadeState = state;

    // Propagate evade state to controlled units (pets, guardians, etc.)
    if (!_owner->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED))
    {
        // a controlled unit can be far, in another task's area: its evade state is set on its owner
        Map* map = _owner->FindMap();
        for (Unit* controlled : _owner->m_Controlled)
        {
            if (!map || map->CanWriteNow(controlled))
                controlled->GetCombatManager().SetEvadeState(state);
            else
                map->RunOnOwner(controlled, [state](WorldObject* target)
                {
                    if (Unit* unit = target->ToUnit())
                        unit->GetCombatManager().SetEvadeState(state);
                });
        }
    }
}

void CombatManager::StopEvade()
{
    _evadeTimer = 0;
    SetEvadeState(EVADE_STATE_NONE);
}
