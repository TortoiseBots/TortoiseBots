#include "ClaimLifecycle.h"
#include "AhBotSellerAccount.h"
#include "policy/hire/ClaimedBotPolicy.h"

#include "../host/ModuleLog.h"

#include "Database/DatabaseEnv.h"
#include "Guild/Guild.h"
#include "Guild/GuildMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include "Log.h"

namespace TortoiseBots
{

ClaimLifecycle& ClaimLifecycle::Instance()
{
    static ClaimLifecycle instance;
    return instance;
}

void ClaimLifecycle::Initialize()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized)
        return;

    m_claimedByBotGuid.clear();

    std::unique_ptr<QueryResult> result(CharacterDatabase.Query(
        "SELECT `bot_guid`, `owner_account_id`, `owner_player_guid`, `guild_id` FROM `tortoise_bots_claimed`"));

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            ClaimedBotRecord record;
            record.botGuidLow = fields[0].GetUInt32();
            record.ownerAccountId = fields[1].GetUInt32();
            record.ownerPlayerGuidLow = fields[2].GetUInt32();
            record.guildId = fields[3].GetUInt32();

            if (record.botGuidLow)
                m_claimedByBotGuid[record.botGuidLow] = record;
        } while (result->NextRow());
    }

    m_initialized = true;
    TB_LOG_BASIC("TortoiseBots: loaded %zu claimed guild bot(s) from database.", m_claimedByBotGuid.size());
}

bool ClaimLifecycle::Claim(uint32_t botGuidLow, uint32_t ownerAccountId, uint32_t ownerPlayerGuidLow, uint32_t guildId)
{
    if (!botGuidLow || !ownerPlayerGuidLow || !guildId)
        return false;
    if (AhBotSellerAccount::Instance().IsSellerGuid(botGuidLow))
    {
        sLog.outError("TortoiseBots: claim refused for AH bot seller character %u", botGuidLow);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_claimedByBotGuid.find(botGuidLow) != m_claimedByBotGuid.end())
        {
            sLog.outError("TortoiseBots: claim refused for already-claimed bot %u (no steal)", botGuidLow);
            return false;
        }
    }

    ClaimedBotRecord record;
    record.botGuidLow = botGuidLow;
    record.ownerAccountId = ownerAccountId;
    record.ownerPlayerGuidLow = ownerPlayerGuidLow;
    record.guildId = guildId;

    if (!CharacterDatabase.DirectPExecute(
            "INSERT INTO `tortoise_bots_claimed` "
            "(`bot_guid`, `owner_account_id`, `owner_player_guid`, `guild_id`) "
            "VALUES ('%u', '%u', '%u', '%u')",
            botGuidLow, ownerAccountId, ownerPlayerGuidLow, guildId))
    {
        sLog.outError("TortoiseBots: failed to persist claim record for bot %u (guild %u)", botGuidLow, guildId);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_claimedByBotGuid[botGuidLow] = record;
    }

    TB_LOG_BASIC("TortoiseBots: bot %u successfully claimed by player %u into guild %u.",
        botGuidLow, ownerPlayerGuidLow, guildId);
    return true;
}

bool ClaimLifecycle::Unclaim(uint32_t botGuidLow)
{
    if (!botGuidLow)
        return false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_claimedByBotGuid.find(botGuidLow);
        if (it == m_claimedByBotGuid.end())
            return false;
        m_claimedByBotGuid.erase(it);
    }

    CharacterDatabase.DirectPExecute("DELETE FROM `tortoise_bots_claimed` WHERE `bot_guid` = '%u'", botGuidLow);
    TB_LOG_BASIC("TortoiseBots: bot %u unclaimed and returned to wandering pool.", botGuidLow);
    return true;
}

bool ClaimLifecycle::IsClaimed(uint32_t botGuidLow) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_claimedByBotGuid.find(botGuidLow) != m_claimedByBotGuid.end();
}

bool ClaimLifecycle::IsClaimedByAccount(uint32_t botGuidLow, uint32_t accountId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_claimedByBotGuid.find(botGuidLow);
    return it != m_claimedByBotGuid.end() && it->second.ownerAccountId == accountId;
}

namespace
{

bool RankEditsOfficerNotes(Player* requester)
{
    Guild* guild = requester->GetGuildId() ? sGuildMgr.GetGuildById(requester->GetGuildId()) : nullptr;
    return guild && guild->HasRankRight(requester->GetRank(), GR_RIGHT_EOFFNOTE);
}

bool IsRealPlayer(Player* requester)
{
    return requester->GetSession() && requester->GetSession()->HasNetworkTransport();
}

} // namespace

bool ClaimLifecycle::MayControl(uint32_t botGuidLow, Player* requester) const
{
    if (!requester)
        return false;
    ClaimedBotRecord record;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_claimedByBotGuid.find(botGuidLow);
        if (it == m_claimedByBotGuid.end())
            return false;
        record = it->second;
    }
    bool const isClaimer = record.ownerPlayerGuidLow == requester->GetGUIDLow();
    return MayControlClaimedBot(isClaimer, record.guildId, requester->GetGuildId(),
        isClaimer || IsRealPlayer(requester), isClaimer || RankEditsOfficerNotes(requester));
}

std::vector<uint32_t> ClaimLifecycle::GetControlledBots(Player* requester) const
{
    std::vector<uint32_t> result;
    if (!requester)
        return result;
    uint32_t const playerGuidLow = requester->GetGUIDLow();
    bool const officer = requester->GetGuildId() && IsRealPlayer(requester) && RankEditsOfficerNotes(requester);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto const& pair : m_claimedByBotGuid)
    {
        ClaimedBotRecord const& record = pair.second;
        bool const isClaimer = record.ownerPlayerGuidLow == playerGuidLow;
        if (MayControlClaimedBot(isClaimer, record.guildId, requester->GetGuildId(), officer, officer))
            result.push_back(pair.first);
    }
    return result;
}

uint32_t ClaimLifecycle::GetOwnerPlayerGuid(uint32_t botGuidLow) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_claimedByBotGuid.find(botGuidLow);
    return it != m_claimedByBotGuid.end() ? it->second.ownerPlayerGuidLow : 0;
}

uint32_t ClaimLifecycle::GetOwnerAccountId(uint32_t botGuidLow) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_claimedByBotGuid.find(botGuidLow);
    return it != m_claimedByBotGuid.end() ? it->second.ownerAccountId : 0;
}

uint32_t ClaimLifecycle::GetGuildId(uint32_t botGuidLow) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_claimedByBotGuid.find(botGuidLow);
    return it != m_claimedByBotGuid.end() ? it->second.guildId : 0;
}

std::vector<uint32_t> ClaimLifecycle::GetAllClaimedBots() const
{
    std::vector<uint32_t> result;
    std::lock_guard<std::mutex> lock(m_mutex);
    result.reserve(m_claimedByBotGuid.size());
    for (auto const& pair : m_claimedByBotGuid)
        result.push_back(pair.first);
    return result;
}

std::vector<uint32_t> ClaimLifecycle::GetClaimedBotsForGuild(uint32_t guildId) const
{
    std::vector<uint32_t> result;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto const& pair : m_claimedByBotGuid)
    {
        if (pair.second.guildId == guildId)
            result.push_back(pair.first);
    }
    return result;
}

void ClaimLifecycle::OnGuildDelMember(uint32_t botGuidLow)
{
    if (IsClaimed(botGuidLow))
        Unclaim(botGuidLow);
}

void ClaimLifecycle::OnGuildDisband(uint32_t guildId)
{
    std::vector<uint32_t> botsToUnclaim;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto const& pair : m_claimedByBotGuid)
        {
            if (pair.second.guildId == guildId)
                botsToUnclaim.push_back(pair.first);
        }
    }

    for (uint32_t botGuid : botsToUnclaim)
        Unclaim(botGuid);
}

} // namespace TortoiseBots
