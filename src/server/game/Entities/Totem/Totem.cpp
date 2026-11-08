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

#include "Totem.h"
#include "Group.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TotemPackets.h"

Totem::Totem(SummonPropertiesEntry const* properties, ObjectGuid owner) : Minion(properties, owner)
{
    m_unitTypeMask |= UNIT_MASK_TOTEM;
    m_duration = 0;
    m_type = TOTEM_PASSIVE;
}

void Totem::Update(uint32 time)
{
    Unit* owner = GetOwner();

    if (!owner || !IsAlive() || m_duration <= time)
    {
        UnSummon();                                         // remove self
        return;
    }

    // If owner is dead and this is not a lightwell, despawn
    if (!owner->IsAlive() && !(m_Properties && m_Properties->Type == SUMMON_TYPE_LIGHTWELL))
    {
        UnSummon();
        return;
    }

    m_duration -= time;
    Creature::Update(time);
}

void Totem::InitStats(uint32 duration)
{
    // client requires SMSG_TOTEM_CREATED to be sent before adding to world and before removing old totem
    // Xinef: Set level for Unit totems
    if (Unit* owner = ObjectAccessor::GetUnit(*this, m_owner))
    {
        uint32 slot = m_Properties->Slot;
        if (owner->IsPlayer() && slot >= SUMMON_SLOT_TOTEM_FIRE && slot < MAX_TOTEM_SLOT)
        {
            WorldPackets::Totem::TotemCreated data;
            data.Totem = GetGUID();
            data.Slot = slot - SUMMON_SLOT_TOTEM_FIRE;
            data.Duration = duration;
            data.SpellID = GetUInt32Value(UNIT_CREATED_BY_SPELL);
            owner->ToPlayer()->SendDirectMessage(data.Write());

            // set display id depending on caster's race
            SetDisplayId(sObjectMgr->GetModelForTotem(SummonSlot(slot), Races(owner->getRace())));
        }

        SetLevel(owner->GetLevel());
    }

    Minion::InitStats(duration);

    // Get spell cast by totem
    if (SpellInfo const* totemSpell = sSpellMgr->GetSpellInfo(GetSpell()))
        if (totemSpell->CalcCastTime())   // If spell has cast time -> its an active totem
            m_type = TOTEM_ACTIVE;

    m_duration = duration;
}

void Totem::InitSummon()
{
    Minion::InitSummon();

    if (m_type == TOTEM_PASSIVE && GetSpell())
    {
        if (TotemSpellIds(GetUInt32Value(UNIT_CREATED_BY_SPELL)) == TotemSpellIds::FireTotemSpell)
        {
            m_Events.AddEventAtOffset([this]()
            {
                CastSpell(this, GetSpell(), true);
            }, 4s);
        }
        else
        {
            CastSpell(this, GetSpell(), true);
        }
    }

    // Some totems can have both instant effect and passive spell
    if (GetSpell(1))
    {
        CastSpell(this, GetSpell(1), true);
    }

    // xinef: this is better than the script, 100% sure to work
    if (GetEntry() == SENTRY_TOTEM_ENTRY)
    {
        SetReactState(REACT_AGGRESSIVE);
        GetOwner()->CastSpell(this, 6277, true);

        // Farsight objects should be active
        setActive(true);
        SetVisibilityDistanceOverride(VisibilityDistanceType::Infinite);
    }

    if (!IsInWater())
    {
        GetMotionMaster()->MoveFall();
    }
}

void Totem::UnSummon(Milliseconds msTime)
{
    // Called from the owner's update (RemoveAllControlled on teleport or death, totem replacement) this totem can
    // be far, in another task's area of a parallel map phase: it is written here, so unsummon on its owner.
    if (Map* map = FindMap(); map && map->IsParallelPhase() && !map->CanWriteNow(this))
    {
        map->RunOnOwner(this, [msTime](WorldObject* target)
        {
            if (Creature* creature = target->ToCreature())
                if (creature->IsInWorld())
                    if (TempSummon* summon = creature->ToTempSummon())
                        summon->UnSummon(msTime);
        });
        return;
    }

    if (msTime > 0ms)
    {
        m_Events.AddEventAtOffset(new ForcedUnsummonDelayEvent(*this), msTime);
        return;
    }

    CombatStop();
    RemoveAurasDueToSpell(GetSpell(), GetGUID());

    if (Unit* owner = GetOwner())
    {
        // the owner (and its group) can be far, in another task's area: it walked away from its totem. Its side is
        // then updated on its owner; the lambda captures values only, the totem is leaving the world.
        auto clearOwner = [totemGuid = GetGUID(), spellId = GetSpell(), entry = GetEntry(), createdBySpell = GetUInt32Value(UNIT_CREATED_BY_SPELL)](Unit* totemOwner)
        {
            // clear owner's totem slot
            for (uint8 i = SUMMON_SLOT_TOTEM_FIRE; i < MAX_TOTEM_SLOT; ++i)
            {
                if (totemOwner->m_SummonSlot[i] == totemGuid)
                {
                    totemOwner->m_SummonSlot[i].Clear();
                    break;
                }
            }

            totemOwner->RemoveAurasDueToSpell(spellId, totemGuid);

            // Remove Sentry Totem Aura
            if (entry == SENTRY_TOTEM_ENTRY)
                totemOwner->RemoveAurasDueToSpell(static_cast<uint32>(TotemSpellIds::SentryTotemSpell));

            //remove aura all party members too
            if (Player* player = totemOwner->ToPlayer())
            {
                // Player::SendAutoRepeatCancel(totem)
                WorldPacket data(SMSG_CANCEL_AUTO_REPEAT, totemGuid.WriteAsPacked().size());
                data << totemGuid.WriteAsPacked();
                player->SendMessageToSet(&data, true);

                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(createdBySpell))
                    player->SendCooldownEvent(spell, 0, nullptr, false);

                if (Group* group = player->GetGroup())
                {
                    Map* map = player->GetMap();
                    std::lock_guard<std::recursive_mutex> guard(group->GetLock());
                    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* target = itr->GetSource();
                        if (!target || !target->IsInMap(player) || !group->SameSubGroup(player, target))
                            continue;

                        if (map->CanWriteNow(target))
                            target->RemoveAurasDueToSpell(spellId, totemGuid);
                        else
                            map->RunOnOwner(target, [spellId, totemGuid](WorldObject* member)
                            {
                                if (Unit* unit = member->ToUnit())
                                    unit->RemoveAurasDueToSpell(spellId, totemGuid);
                            });
                    }
                }
            }
        };

        if (GetMap()->CanWriteNow(owner))
            clearOwner(owner);
        else
            GetMap()->RunOnOwner(owner, [clearOwner](WorldObject* target)
            {
                if (Unit* totemOwner = target->ToUnit())
                    clearOwner(totemOwner);
            });
    }

    AddObjectToRemoveList();
}

bool Totem::IsImmunedToSpellEffect(SpellInfo const* spellInfo, uint32 index, WorldObject const* caster /*= nullptr*/) const
{
    // xinef: immune to all positive spells, except of stoneclaw totem absorb, sentry totem bind sight and intervene
    // totems positive spells have unit_caster target
    if (spellInfo->Effects[index].Effect != SPELL_EFFECT_DUMMY &&
            spellInfo->Effects[index].Effect != SPELL_EFFECT_SCRIPT_EFFECT &&
            spellInfo->IsPositive() && spellInfo->Effects[index].TargetA.GetTarget() != TARGET_UNIT_CASTER &&
            spellInfo->Effects[index].TargetA.GetCheckType() != TARGET_CHECK_ENTRY &&
            spellInfo->Id != SPELL_STONECLAW && spellInfo->Id != SPELL_BIND_SIGHT && spellInfo->Id != SPELL_INTERVENE)
        return true;

    if (spellInfo->Effects[index].Effect == SPELL_EFFECT_ATTACK_ME ||
            spellInfo->Effects[index].ApplyAuraName == SPELL_AURA_MOD_TAUNT)
        return true;

    // Cyclone shouldn't be casted on totems
    if (spellInfo->Id == SPELL_CYCLONE)
    {
        return true;
    }

    switch (spellInfo->Effects[index].ApplyAuraName)
    {
        // i think its wrong (xinef)
        //case SPELL_AURA_PERIODIC_LEECH:
        case SPELL_AURA_PERIODIC_DAMAGE:
        case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
        case SPELL_AURA_MOD_FEAR:
        case SPELL_AURA_TRANSFORM:
            return true;
        default:
            break;
    }

    return Creature::IsImmunedToSpellEffect(spellInfo, index, caster);
}
