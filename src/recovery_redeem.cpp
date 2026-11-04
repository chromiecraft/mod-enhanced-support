/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "EnhancedSupport.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "ItemRecovery.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

// Redeems an item recovery row for the website: the character pays back the copper a
// vendor paid for the item and the materials looted from disenchanting it, then the
// item is mailed through ItemRecovery::Restore. Destroyed rows cost nothing.
//
// Materials are taken from the bags first, then the bank.
//
// Online characters pay from their live inventory and money. Offline characters pay through
// a direct edit of their saved inventory, which is only safe while nothing else can
// write it: no session on the account (a login is added on the world thread, which is
// running this command) and a logout old enough for its save to have reached the DB.

namespace
{
    struct ItemStack
    {
        uint32 Guid;
        uint32 Count;
    };

    std::string FormatMoney(uint32 money)
    {
        return Acore::StringFormat("{}g {}s {}c", money / GOLD, (money % GOLD) / SILVER, money % SILVER);
    }

    std::string ItemName(uint32 entry)
    {
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
            return proto->Name1;

        return Acore::StringFormat("item {}", entry);
    }

    std::string DescribeCost(RecoveryItem const& recovery)
    {
        std::vector<std::string> parts;
        if (recovery.Money)
            parts.push_back(FormatMoney(recovery.Money));

        for (auto const& [entry, count] : recovery.Loot)
            parts.push_back(Acore::StringFormat("{}x {} ({})", count, ItemName(entry), entry));

        if (parts.empty())
            return "nothing";

        std::string cost = parts.front();
        for (std::size_t i = 1; i < parts.size(); ++i)
            cost += ", " + parts[i];

        return cost;
    }

    // Saved stacks of entry in the bag-0 slots [itemStart, itemEnd) and inside the bags
    // equipped in bag-0 slots [bagStart, bagEnd), appended to stacks.
    void LoadSavedStacks(std::vector<ItemStack>& stacks, uint32 ownerGuid, uint32 entry,
        uint8 itemStart, uint8 itemEnd, uint8 bagStart, uint8 bagEnd)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT ii.guid, ii.count FROM character_inventory ci "
            "INNER JOIN item_instance ii ON ii.guid = ci.item "
            "WHERE ci.guid = {} AND ii.itemEntry = {} AND ("
            "(ci.bag = 0 AND ci.slot >= {} AND ci.slot < {}) OR ci.bag IN ("
            "SELECT bags.item FROM character_inventory bags WHERE bags.guid = {} AND bags.bag = 0 AND bags.slot >= {} AND bags.slot < {}))",
            ownerGuid, entry, itemStart, itemEnd, ownerGuid, bagStart, bagEnd);

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                stacks.push_back({ fields[0].Get<uint32>(), fields[1].Get<uint32>() });
            } while (result->NextRow());
        }
    }

    // Backpack and bags first, then the bank, so carried stacks are spent before banked ones
    std::vector<ItemStack> LoadSavedStacks(uint32 ownerGuid, uint32 entry)
    {
        std::vector<ItemStack> stacks;
        LoadSavedStacks(stacks, ownerGuid, entry, INVENTORY_SLOT_ITEM_START, INVENTORY_SLOT_ITEM_END,
            INVENTORY_SLOT_BAG_START, INVENTORY_SLOT_BAG_END);
        LoadSavedStacks(stacks, ownerGuid, entry, BANK_SLOT_ITEM_START, BANK_SLOT_ITEM_END,
            BANK_SLOT_BAG_START, BANK_SLOT_BAG_END);
        return stacks;
    }

    // Player::DestroyItemCount(entry, ...) never looks in the bank
    void DestroyFromBank(Player* player, uint32 entry, uint32 count)
    {
        for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END && count; ++i)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i); item && item->GetEntry() == entry)
                player->DestroyItemCount(item, count, true);

        for (uint8 i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END && count; ++i)
            if (Bag* bag = player->GetBagByPos(i))
                for (uint8 j = 0; j < bag->GetBagSize() && count; ++j)
                    if (Item* item = bag->GetItemByPos(j); item && item->GetEntry() == entry)
                        player->DestroyItemCount(item, count, true);
    }

    uint32 CountStacks(std::vector<ItemStack> const& stacks)
    {
        uint32 total = 0;
        for (ItemStack const& stack : stacks)
            total += stack.Count;

        return total;
    }

    bool Fail(ChatHandler* handler, std::string const& message)
    {
        handler->SendSysMessage(message);
        handler->SetSentErrorMessage(true);
        return false;
    }

    bool RedeemOnline(ChatHandler* handler, RecoveryItem const& recovery, Player* player, bool redeem)
    {
        if (recovery.Source == RECOVERY_ITEM_SOURCE_VENDORED && ItemRecovery::IsInBuyback(player, recovery))
            return Fail(handler, Acore::StringFormat("Recovery {}: {} is still in {}'s vendor buyback list.",
                recovery.Id, ItemName(recovery.ItemEntry), player->GetName()));

        std::vector<std::string> missing;
        if (!player->HasEnoughMoney(recovery.Money))
            missing.push_back(FormatMoney(recovery.Money - player->GetMoney()));

        for (auto const& [entry, count] : recovery.Loot)
        {
            uint32 const owned = player->GetItemCount(entry, true);
            if (owned < count)
                missing.push_back(Acore::StringFormat("{}x {} ({})", count - owned, ItemName(entry), entry));
        }

        std::string const cost = DescribeCost(recovery);
        if (!missing.empty())
        {
            std::string list = missing.front();
            for (std::size_t i = 1; i < missing.size(); ++i)
                list += ", " + missing[i];

            return Fail(handler, Acore::StringFormat("Recovery {}: {} (online) cannot pay {}. Missing: {}.",
                recovery.Id, player->GetName(), cost, list));
        }

        if (!redeem)
        {
            handler->PSendSysMessage("Recovery {}: {} (online) can redeem {} for {}.",
                recovery.Id, player->GetName(), ItemName(recovery.ItemEntry), cost);
            return true;
        }

        // Restore first: it refuses a row already restored in this process, which keeps a
        // repeated request from charging the player twice. Nothing below can fail, since the
        // checks above ran on this same thread.
        if (!ItemRecovery::Restore(recovery, player))
            return Fail(handler, Acore::StringFormat("Recovery {} could not be restored.", recovery.Id));

        if (recovery.Money)
            player->ModifyMoney(-int32(recovery.Money));

        for (auto const& [entry, count] : recovery.Loot)
        {
            uint32 const carried = std::min(count, player->GetItemCount(entry, false));
            player->DestroyItemCount(entry, carried, true);
            DestroyFromBank(player, entry, count - carried);
        }

        ChatHandler(player->GetSession()).PSendSysMessage("Your {} has been restored and mailed to you. Paid: {}.",
            ItemName(recovery.ItemEntry), cost);
        handler->PSendSysMessage("Recovery {}: {} redeemed for {} (online), paid {}.",
            recovery.Id, ItemName(recovery.ItemEntry), player->GetName(), cost);
        return true;
    }

    bool RedeemOffline(ChatHandler* handler, RecoveryItem const& recovery, std::string const& name, bool redeem)
    {
        uint32 const accountId = sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid::Create<HighGuid::Player>(recovery.OwnerGuid));
        if (sWorldSessionMgr->FindSession(accountId))
            return Fail(handler, Acore::StringFormat("Recovery {}: {}'s account is in game or at the character selection screen. "
                "Log out completely, or log in with {}.", recovery.Id, name, name));

        QueryResult character = CharacterDatabase.Query("SELECT online, money, logout_time FROM characters WHERE guid = {}", recovery.OwnerGuid);
        if (!character)
            return Fail(handler, Acore::StringFormat("Recovery {}: character {} was not found.", recovery.Id, name));

        Field* fields = character->Fetch();
        bool const online = fields[0].Get<uint8>() != 0;
        uint32 const money = fields[1].Get<uint32>();
        uint32 const logoutTime = fields[2].Get<uint32>();

        if (online)
            return Fail(handler, Acore::StringFormat("Recovery {}: {} is still marked online. Try again shortly.", recovery.Id, name));

        // The logout save is written asynchronously; editing the rows before it lands would be overwritten
        if (uint32(GameTime::GetGameTime().count()) < logoutTime + 60)
            return Fail(handler, Acore::StringFormat("Recovery {}: {} logged out less than a minute ago. Try again shortly.",
                recovery.Id, name));

        std::vector<std::string> missing;
        if (money < recovery.Money)
            missing.push_back(FormatMoney(recovery.Money - money));

        std::vector<std::vector<ItemStack>> stacksPerLoot;
        for (auto const& [entry, count] : recovery.Loot)
        {
            stacksPerLoot.push_back(LoadSavedStacks(recovery.OwnerGuid, entry));
            uint32 const owned = CountStacks(stacksPerLoot.back());
            if (owned < count)
                missing.push_back(Acore::StringFormat("{}x {} ({})", count - owned, ItemName(entry), entry));
        }

        std::string const cost = DescribeCost(recovery);
        if (!missing.empty())
        {
            std::string list = missing.front();
            for (std::size_t i = 1; i < missing.size(); ++i)
                list += ", " + missing[i];

            return Fail(handler, Acore::StringFormat("Recovery {}: {} (offline) cannot pay {}. Missing: {}.",
                recovery.Id, name, cost, list));
        }

        if (!redeem)
        {
            handler->PSendSysMessage("Recovery {}: {} (offline) can redeem {} for {}.",
                recovery.Id, name, ItemName(recovery.ItemEntry), cost);
            return true;
        }

        // Restore first for the same reason as online: a repeated request stops here, unpaid
        if (!ItemRecovery::Restore(recovery, nullptr))
            return Fail(handler, Acore::StringFormat("Recovery {} could not be restored.", recovery.Id));

        // Raw SQL: core's prepared inventory statements are async-only and cannot run in a
        // synchronous transaction, and the edit must be on disk before this command returns.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (recovery.Money)
            trans->Append("UPDATE characters SET money = money - {} WHERE guid = {}", recovery.Money, recovery.OwnerGuid);

        for (std::size_t i = 0; i < recovery.Loot.size(); ++i)
        {
            uint32 remaining = recovery.Loot[i].second;
            for (ItemStack const& stack : stacksPerLoot[i])
            {
                if (!remaining)
                    break;

                if (stack.Count <= remaining)
                {
                    trans->Append("DELETE FROM character_inventory WHERE item = {}", stack.Guid);
                    trans->Append("DELETE FROM item_instance WHERE guid = {}", stack.Guid);
                    remaining -= stack.Count;
                }
                else
                {
                    trans->Append("UPDATE item_instance SET count = {} WHERE guid = {}", stack.Count - remaining, stack.Guid);
                    remaining = 0;
                }
            }
        }

        CharacterDatabase.DirectCommitTransaction(trans);

        handler->PSendSysMessage("Recovery {}: {} redeemed for {} (offline), paid {}.",
            recovery.Id, ItemName(recovery.ItemEntry), name, cost);
        return true;
    }
}

bool EnhancedSupport::RedeemRecovery(ChatHandler* handler, uint32 recoveryId, uint32 ownerGuid, std::string const& ownerName, bool redeem)
{
    std::optional<RecoveryItem> recovery = ItemRecovery::Load(recoveryId);
    if (!recovery || recovery->OwnerGuid != ownerGuid)
        return Fail(handler, Acore::StringFormat("Recovery {} does not belong to {}.", recoveryId, ownerName));

    if (!sObjectMgr->GetItemTemplate(recovery->ItemEntry))
        return Fail(handler, Acore::StringFormat("Recovery {}: item {} no longer exists.", recoveryId, recovery->ItemEntry));

    if (Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid)))
        return RedeemOnline(handler, *recovery, player, redeem);

    return RedeemOffline(handler, *recovery, ownerName, redeem);
}
