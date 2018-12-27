/*
 * Copyright (C) 2008-2018 TrinityCore <https://www.trinitycore.org/>
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

#include "Trainer.h"
#include "Creature.h"
#include "NPCPackets.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Language.h"

namespace Trainer
{
    Trainer::Trainer(uint32 trainerId, Type type, uint32 requirement, std::string greeting, std::vector<TrainerSpell> spells) : _trainerId(trainerId), _type(type), _requirement(requirement), _spells(std::move(spells))
    {
        _greeting[DEFAULT_LOCALE] = std::move(greeting);
    }

    void Trainer::SendSpells(Creature const* npc, Player const* player, LocaleConstant locale) const
    {
        float reputationDiscount = player->GetReputationPriceDiscount(npc);

        WorldPackets::NPC::TrainerList trainerList;
        trainerList.TrainerGUID = npc->GetGUID();
        trainerList.TrainerType = AsUnderlyingType(_type);
        trainerList.Greeting = GetGreeting(locale);
        if(trainerList.Greeting == "")
            trainerList.Greeting = sObjectMgr->GetTrinityString(LANG_NPC_TAINER_HELLO, locale);

        trainerList.Spells.reserve(_spells.size());
        for (TrainerSpell const& trainerSpell : _spells)
        {
            if (!player->IsSpellFitByClassAndRace(trainerSpell.SpellId))
                continue;

            bool primaryProfessionFirstRank = false;
            for (int32 reqAbility : trainerSpell.ReqAbility)
            {
                SpellInfo const* learnedSpellInfo = sSpellMgr->GetSpellInfo(reqAbility);
                if (learnedSpellInfo && learnedSpellInfo->IsPrimaryProfessionFirstRank())
                    primaryProfessionFirstRank = true;
            }

            trainerList.Spells.emplace_back();
            WorldPackets::NPC::TrainerListSpell& trainerListSpell = trainerList.Spells.back();
            trainerListSpell.SpellID = trainerSpell.SpellId;
            trainerListSpell.Usable = AsUnderlyingType(GetSpellState(player, &trainerSpell));
            trainerListSpell.MoneyCost = int32(trainerSpell.MoneyCost * reputationDiscount);
            trainerListSpell.ProfessionDialog = (primaryProfessionFirstRank && (player->GetFreePrimaryProfessionPoints() > 0) ? 1 : 0);
            trainerListSpell.ProfessionButton = (primaryProfessionFirstRank ? 1 : 0);
            trainerListSpell.ReqLevel = trainerSpell.ReqLevel;
            trainerListSpell.ReqSkillLine = trainerSpell.ReqSkillLine;
            trainerListSpell.ReqSkillRank = trainerSpell.ReqSkillRank;
            std::copy(trainerSpell.ReqAbility.begin(), trainerSpell.ReqAbility.end(), trainerListSpell.ReqAbility.begin());
        }

        player->SendDirectMessage(trainerList.Write());
    }

    void Trainer::TeachSpell(Creature const* npc, Player* player, uint32 spellId) const
    {
        if (!IsTrainerValidForPlayer(player))
            return;

        TrainerSpell const* trainerSpell = GetSpell(spellId);
        if (!trainerSpell)
        {
            SendTeachFailure(npc, player, spellId, FailReason::Unavailable);
            return;
        }

        if (!CanTeachSpell(player, trainerSpell))
        {
            SendTeachFailure(npc, player, spellId, FailReason::NotEnoughSkill);
            return;
        }

        float reputationDiscount = player->GetReputationPriceDiscount(npc);
        int32 moneyCost = int32(trainerSpell->MoneyCost * reputationDiscount);
        if (!player->HasEnoughMoney(moneyCost))
        {
            SendTeachFailure(npc, player, spellId, FailReason::NotEnoughMoney);
            return;
        }

        player->ModifyMoney(-moneyCost);

        npc->SendPlaySpellVisual(179);
        npc->SendPlaySpellImpact(player->GetGUID(), 362);

        // learn explicitly or cast explicitly
        if (trainerSpell->IsCastable())
            player->CastSpell(player, trainerSpell->SpellId, true);
        else
            player->LearnSpell(trainerSpell->SpellId, false);

        SendTeachSucceeded(npc, player, spellId);
    }

    TrainerSpell const* Trainer::GetSpell(uint32 spellId) const
    {
        auto itr = std::find_if(_spells.begin(), _spells.end(), [spellId](TrainerSpell const& trainerSpell)
        {
            return trainerSpell.SpellId == spellId;
        });

        if (itr != _spells.end())
            return &(*itr);

        return nullptr;
    }

    bool Trainer::CanTeachSpell(Player const* player, TrainerSpell const* trainerSpell) const
    {
        SpellState state = GetSpellState(player, trainerSpell);
        if (state != SpellState::Available)
            return false;

        SpellInfo const* trainerSpellInfo = sSpellMgr->AssertSpellInfo(trainerSpell->SpellId);
        if (trainerSpellInfo->IsPrimaryProfessionFirstRank() && !player->GetFreePrimaryProfessionPoints())
            return false;

        return true;
    }

    /*static*/ SpellState Trainer::GetSpellState(Player const* player, TrainerSpell const* trainerSpell)
    {
        if (player->HasSpell(trainerSpell->SpellId))
            return SpellState::Known;

        // check race/class requirement
        if (!player->IsSpellFitByClassAndRace(trainerSpell->SpellId))
            return SpellState::Unavailable;

        // check skill requirement
        if (trainerSpell->ReqSkillLine && player->GetBaseSkillValue(trainerSpell->ReqSkillLine) < trainerSpell->ReqSkillRank)
            return SpellState::Unavailable;

        for (int32 reqAbility : trainerSpell->ReqAbility)
            if (reqAbility && !player->HasSpell(reqAbility))
                return SpellState::Unavailable;

        // check level requirement
        if (player->GetLevel() < trainerSpell->ReqLevel)
            return SpellState::Unavailable;

        // check ranks
        if (uint32 previousRankSpellId = sSpellMgr->GetPrevSpellInChain(trainerSpell->LearnedSpellId))
            if (!player->HasSpell(previousRankSpellId))
                return SpellState::Unavailable;

        // check additional spell requirement
        for (auto const& requirePair : sSpellMgr->GetSpellsRequiredForSpellBounds(trainerSpell->SpellId))
            if (!player->HasSpell(requirePair.second))
                return SpellState::Unavailable;

        return SpellState::Available;
    }

    bool Trainer::IsTrainerValidForPlayer(Player const* player) const
    {
        // check class for class trainers
        if (player->GetClass() != GetTrainerRequirement() && (GetTrainerType() == Type::Class || GetTrainerType() == Type::Pet))
            return false;

        // check race for mount trainers
        if (player->GetRace() != GetTrainerRequirement() && GetTrainerType() == Type::Mount)
            return false;

        // check spell for profession trainers
        if (!player->HasSpell(GetTrainerRequirement()) && GetTrainerRequirement() != 0 && GetTrainerType() == Type::Tradeskill)
            return false;

        return true;
    }

    void Trainer::SendTeachFailure(Creature const* npc, Player const* player, uint32 spellId, FailReason reason) const
    {
        WorldPackets::NPC::TrainerBuyFailed trainerBuyFailed;
        trainerBuyFailed.TrainerGUID = npc->GetGUID();
        trainerBuyFailed.SpellID = spellId;
        trainerBuyFailed.TrainerFailedReason = AsUnderlyingType(reason);
        player->SendDirectMessage(trainerBuyFailed.Write());
    }

    void Trainer::SendTeachSucceeded(Creature const* npc, Player const* player, uint32 spellId) const
    {
        WorldPackets::NPC::TrainerBuySucceeded trainerBuySucceeded;
        trainerBuySucceeded.TrainerGUID = npc->GetGUID();
        trainerBuySucceeded.SpellID = spellId;
        player->SendDirectMessage(trainerBuySucceeded.Write());
    }

    std::string const& Trainer::GetGreeting(LocaleConstant locale) const
    {
        if (_greeting[locale].empty())
            return _greeting[DEFAULT_LOCALE];

        return _greeting[locale];
    }

    void Trainer::AddGreetingLocale(LocaleConstant locale, std::string greeting)
    {
        _greeting[locale] = std::move(greeting);
    }
}