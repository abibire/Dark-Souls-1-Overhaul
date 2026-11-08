#include "CustomInvasionTypes.h"
#include "DarkSoulsOverhaulMod.h"
#include "SP/memory/injection/asm/x64.h"
#include "MainLoop.h"
#include "ModNetworking.h"
#include "GameEnum.h"
#include <tuple>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_set>

extern "C" {
    uint64_t Send_Type17_GeneralRequestTask_injection_return;
    void Send_Type17_GeneralRequestTask_injection();
    void Send_Type17_GeneralRequestTask_injection_helper(uint64_t RequestGetBreakInTargetList_Data);
    uint64_t BreakInTargetList_BlockCheck_injection_return;
    void BreakInTargetList_BlockCheck_injection();
    bool BreakInTargetList_BlockCheck_injection_helper(void* steamid_str);
}
static bool PublishCoopPhantomCount(void* unused);
static void RequestJollyHostList();

void CustomInvasionTypes::start()
{
    ConsoleWrite("Enabling Custom Invasions...");

    uint8_t *write_address = (uint8_t*)(CustomInvasionTypes::Send_Type17_GeneralRequestTask_offset + Game::ds1_base);
    sp::mem::code::x64::inject_jmp_14b(write_address, &Send_Type17_GeneralRequestTask_injection_return, 2, &Send_Type17_GeneralRequestTask_injection);
    //Filter the break in target list for the Jolly eye orb
    write_address = (uint8_t*)(CustomInvasionTypes::BreakInTargetList_BlockCheck_offset + Game::ds1_base);
    sp::mem::code::x64::inject_jmp_14b(write_address, &BreakInTargetList_BlockCheck_injection_return, 0, &BreakInTargetList_BlockCheck_injection);
    MainLoop::setup_mainloop_callback(PublishCoopPhantomCount, NULL, "PublishCoopPhantomCount");
}

//These only need to include 1 area id from each set that match all but the last digit
//i.e 100000 can invade all areas matching 10000X
//http://darksouls.wikidot.com/invasion-spawn-locations
static std::vector<std::tuple<uint32_t, std::wstring>> MultiPlayerRegionIDs;

//read the data from the text file that specifics the areas to search, and their names
//run when a character is loaded/reloaded from the save file
void CustomInvasionTypes::ReadSearchFile()
{
    FILE* fp = NULL;
    char line[80];
    uint32_t mpid;
    char area_name[80];

    fopen_s(&fp, "./searchlist.txt", "r");
    if (fp == NULL)
    {
        ConsoleWrite("WARNING: Searchlist.txt not found. Unable to use searching eye orb.");
        return;
    }

    MultiPlayerRegionIDs.clear();

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        //ignore comments
        if (line[0] != '#')
        {
            int matched = sscanf_s(line, "%lu:%79[^\r\n].*", &mpid, area_name, (unsigned)_countof(area_name));
            if (matched != 2)
            {
                ConsoleWrite("WARNING: Searchlist.txt has malformed line \"%s\"", line);
            }
            else
            {
                //convert area name to wstring
                const size_t size = std::strlen(area_name);
                std::wstring wstr;
                wstr.resize(size);
                mbstowcs_s(NULL, &wstr[0], wstr.capacity(), area_name, _TRUNCATE);

                //insert this entry
                auto entry = std::make_tuple(mpid, wstr);
                MultiPlayerRegionIDs.push_back(entry); //this is fine since a vector makes a copy of the string
            }
        }
    }

    fclose(fp);

    ConsoleWrite("Invasion search areas reloaded");
}

enum InvasionOrb
{
    AllAreas,
    InfiniteUp,
    AllAreasAndInfiniteUp,
    AllAreasJolly,
    NoCustomOrb,
};

static uint32_t last_send_count = 0;
static size_t current_mpregionid_offset = 0;
static uint32_t current_soullevel_offset = 0;
static InvasionOrb current_InvasionOrb = InvasionOrb::NoCustomOrb;
static std::atomic<uint64_t> last_jolly_search_time = 0;

void Send_Type17_GeneralRequestTask_injection_helper(uint64_t RequestGetBreakInTargetList_Data)
{
    wchar_t banner_msg[75];

    auto playerins_o = Game::get_PlayerIns();
    if (!playerins_o.has_value())
    {
        return;
    }

    //If we have the special speffect on, start the custom invasion state
    //This speffect only stays for the 1st frame/send. So we have to remember it
    //Support save and continue. So if we invade and search again it doesn't go back to the start
    if (Game::player_has_speffect((uint64_t)(playerins_o.value()), { CustomInvasionTypes::AllAreasJollyInvadingOrbSpEffect_Red, CustomInvasionTypes::AllAreasJollyInvadingOrbSpEffect_Blue }))
    {
        current_InvasionOrb = InvasionOrb::AllAreasJolly;
        last_send_count = 0;
    }
    else if (Game::player_has_speffect((uint64_t)(playerins_o.value()), { CustomInvasionTypes::AllAreasInvadingOrbSpEffect_Red, CustomInvasionTypes::AllAreasInvadingOrbSpEffect_Blue }))
    {
        current_InvasionOrb = InvasionOrb::AllAreas;
        last_send_count = 0;
    }
    else if (Game::player_has_speffect((uint64_t)(playerins_o.value()), { CustomInvasionTypes::InfiniteUpwardsInvadingOrbSpEffect_Red, CustomInvasionTypes::InfiniteUpwardsInvadingOrbSpEffect_Blue }))
    {
        current_InvasionOrb = InvasionOrb::InfiniteUp;
        last_send_count = 0;
    }
    else if (Game::player_has_speffect((uint64_t)(playerins_o.value()), { CustomInvasionTypes::AllAreasAndInfiniteUpwardsInvadingOrbSpEffect_Red, CustomInvasionTypes::AllAreasAndInfiniteUpwardsInvadingOrbSpEffect_Blue }))
    {
        current_InvasionOrb = InvasionOrb::AllAreasAndInfiniteUp;
        last_send_count = 0;
    }
    //If the player is doing any other normal multiplayer types, don't do this custom code
    else if (Game::player_has_speffect((uint64_t)(playerins_o.value()), { 4, 10, 11, 16, 26, 27, 15 }))
    {
        current_InvasionOrb = InvasionOrb::NoCustomOrb;
        last_send_count = 0;
        //reset the area id and SL offset
        current_mpregionid_offset = 0;
        current_soullevel_offset = 0;
    }

    if (current_InvasionOrb == InvasionOrb::AllAreas || current_InvasionOrb == InvasionOrb::AllAreasJolly)
    {
        //use the current area id in the list
        if (MultiPlayerRegionIDs.size() > 0)
        {
            *(uint32_t*)(RequestGetBreakInTargetList_Data) = std::get<0>(MultiPlayerRegionIDs[current_mpregionid_offset]);

            if (current_InvasionOrb == InvasionOrb::AllAreasJolly)
            {
                last_jolly_search_time = GetTickCount64();
                RequestJollyHostList();
                swprintf(banner_msg, sizeof(banner_msg) / sizeof(wchar_t), L"Searching area for co-op: %s", std::get<1>(MultiPlayerRegionIDs[current_mpregionid_offset]).c_str());
                ConsoleWrite("Jolly orb: area %d", std::get<0>(MultiPlayerRegionIDs[current_mpregionid_offset]));
            }
            else
            {
                swprintf(banner_msg, sizeof(banner_msg) / sizeof(wchar_t), L"Searching area: %s", std::get<1>(MultiPlayerRegionIDs[current_mpregionid_offset]).c_str());
                ConsoleWrite("Searching orb: area %d", std::get<0>(MultiPlayerRegionIDs[current_mpregionid_offset]));
            }
            Game::show_banner_message(banner_msg);

            last_send_count++;
            if (last_send_count >= 2)
            {
                last_send_count = 0;
            }

            //for some reason, the game calls this function twice for each request. Not sure if one is bad or good or what
            //so to be safe, we don't change from the 1st request
            //(the two requests are not on the same frame)
            if (last_send_count == 0)
            {
                current_mpregionid_offset++;
                if (current_mpregionid_offset >= MultiPlayerRegionIDs.size())
                {
                    current_mpregionid_offset = 0;
                }
            }

            //set the timer to be closer to the refresh time (30 seconds)
            Game::set_invasion_refresh_timer(26.0f);
            Game::set_blue_invasion_refresh_timer(26.0f);
        }
    }

    if (current_InvasionOrb == InvasionOrb::InfiniteUp)
    {
        //Add an offset to the SL we tell the server, so it returns a new range of connection results
        uint32_t pc_sl = *(uint32_t*)(RequestGetBreakInTargetList_Data + 28);
        *(uint32_t*)(RequestGetBreakInTargetList_Data + 28) += current_soullevel_offset;
        uint32_t pc_searched_sl = pc_sl + current_soullevel_offset;

        swprintf(banner_msg, sizeof(banner_msg) / sizeof(wchar_t), L"Using Soul Level: %d", pc_searched_sl);
        Game::show_banner_message(banner_msg);

        ConsoleWrite("Unbound orb: SL %d", pc_searched_sl);
        last_send_count++;
        if (last_send_count >= 2)
        {
            last_send_count = 0;
        }

        //for some reason, the game calls this function twice for each request. Not sure if one is bad or good or what
        //so to be safe, we don't change from the 1st request
        //(the two requests are not on the same frame)
        if (last_send_count == 0)
        {
            current_soullevel_offset += (uint32_t)(1.22222*pc_searched_sl + 22.2222) - pc_searched_sl; //increase by the search's upper and lower bounds
            //clamp the last search value to 713 if needed
            if (pc_sl + current_soullevel_offset > 713)
            {
                current_soullevel_offset = 713 - pc_sl;
            }
            if (pc_searched_sl >= 713)
            {
                current_soullevel_offset = 0;
            }
        }

        //set the timer to be closer to the refresh time (30 seconds)
        Game::set_invasion_refresh_timer(26.0f);
        Game::set_blue_invasion_refresh_timer(26.0f);
    }

    if (current_InvasionOrb == InvasionOrb::AllAreasAndInfiniteUp)
    {
        if (MultiPlayerRegionIDs.size() > 0)
        {
            *(uint32_t*)(RequestGetBreakInTargetList_Data) = std::get<0>(MultiPlayerRegionIDs[current_mpregionid_offset]);

            uint32_t pc_sl = *(uint32_t*)(RequestGetBreakInTargetList_Data + 28);
            *(uint32_t*)(RequestGetBreakInTargetList_Data + 28) += current_soullevel_offset;
            uint32_t pc_searched_sl = pc_sl + current_soullevel_offset;

            swprintf(banner_msg, sizeof(banner_msg) / sizeof(wchar_t), L"Searching area: %s at SL: %d", std::get<1>(MultiPlayerRegionIDs[current_mpregionid_offset]).c_str(), pc_searched_sl);
            Game::show_banner_message(banner_msg);

            ConsoleWrite("Twin eye orb: area %d SL %d", std::get<0>(MultiPlayerRegionIDs[current_mpregionid_offset]), pc_searched_sl);
            last_send_count++;
            if (last_send_count >= 2)
            {
                last_send_count = 0;
            }

            //for some reason, the game calls this function twice for each request. Not sure if one is bad or good or what
            //so to be safe, we don't change from the 1st request
            //(the two requests are not on the same frame)
            if (last_send_count == 0)
            {
                //increase the SL offset first
                current_soullevel_offset += (uint32_t)(1.22222*pc_searched_sl + 22.2222) - pc_searched_sl; //increase by the search's upper and lower bounds
                //clamp the last search value to 713 if needed
                if (pc_sl + current_soullevel_offset > 713)
                {
                    current_soullevel_offset = 713 - pc_sl;
                }
                if (pc_searched_sl >= 713)
                {
                    current_soullevel_offset = 0;
                }

                //once the SL offset has looped around, go to next area
                if (current_soullevel_offset == 0)
                {
                    current_mpregionid_offset++;
                }
                if (current_mpregionid_offset >= MultiPlayerRegionIDs.size())
                {
                    current_mpregionid_offset = 0;
                }
            }

            //set the timer to be closer to the refresh time (30 seconds)
            Game::set_invasion_refresh_timer(26.0f);
            Game::set_blue_invasion_refresh_timer(26.0f);
        }
    }
}

/*
 * ===========JOLLY EYE ORB SECTION
 * Hosts publish their phantom counts in their persistent lobby data. Invaders use the lobby list to find
 * hosts with a coop phantom and no hostile phantoms, and skip all other break in targets.
 */
static const char* MOD_LOBBY_COOPCOUNT_KEY = "DarkSoulsOverhaulCoopPhantoms";
static const char* MOD_LOBBY_HOSTILECOUNT_KEY = "DarkSoulsOverhaulHostilePhantoms";
static const char* MOD_LOBBY_HOSTID_KEY = "DarkSoulsOverhaulHostId";

//If no search request has been sent in this time, the search has ended
static const uint64_t JOLLY_SEARCH_TIMEOUT_MS = 60000;
static const uint64_t JOLLY_HOSTLIST_REFRESH_MS = 5000;

bool CustomInvasionTypes::jolly_search_active()
{
    uint64_t last_time = last_jolly_search_time.load();
    return current_InvasionOrb == InvasionOrb::AllAreasJolly && last_time != 0 && GetTickCount64() - last_time < JOLLY_SEARCH_TIMEOUT_MS;
}

static std::mutex JollyHosts_Mtx;
static std::unordered_set<uint64_t> JollyHosts;
static std::atomic<uint64_t> last_hostlist_request_time = 0;

class JollyHostListRequest
{
public:
    CCallResult<JollyHostListRequest, LobbyMatchList_t> CallResult;
    void LobbyMatchListCallback(LobbyMatchList_t* pCallback, bool bIOFailure);
};
static JollyHostListRequest jollyHostListRequest;

static void RequestJollyHostList()
{
    if (ModNetworking::SteamMatchmaking == nullptr || jollyHostListRequest.CallResult.IsActive())
    {
        return;
    }
    uint64_t now = GetTickCount64();
    if (now - last_hostlist_request_time.load() < JOLLY_HOSTLIST_REFRESH_MS)
    {
        return;
    }
    last_hostlist_request_time = now;

    //filters are cleared after every request
    ModNetworking::SteamMatchmaking->AddRequestLobbyListNumericalFilter(MOD_LOBBY_COOPCOUNT_KEY, 1, k_ELobbyComparisonEqualToOrGreaterThan);
    ModNetworking::SteamMatchmaking->AddRequestLobbyListNumericalFilter(MOD_LOBBY_HOSTILECOUNT_KEY, 0, k_ELobbyComparisonEqual);
    ModNetworking::SteamMatchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    ModNetworking::SteamMatchmaking->AddRequestLobbyListResultCountFilter(250);
    SteamAPICall_t call = ModNetworking::SteamMatchmaking->RequestLobbyList();
    jollyHostListRequest.CallResult.Set(call, &jollyHostListRequest, &JollyHostListRequest::LobbyMatchListCallback);
}

void JollyHostListRequest::LobbyMatchListCallback(LobbyMatchList_t* pCallback, bool bIOFailure)
{
    if (bIOFailure)
    {
        return;
    }
    std::unordered_set<uint64_t> hosts;
    for (uint32_t i = 0; i < pCallback->m_nLobbiesMatching; i++)
    {
        CSteamID lobby = ModNetworking::SteamMatchmaking->GetLobbyByIndex(i);
        const char* host_str = ModNetworking::SteamMatchmaking->GetLobbyData(lobby, MOD_LOBBY_HOSTID_KEY);
        uint64_t host = strtoull(host_str, NULL, 10);
        if (host != 0 && host != ULLONG_MAX)
        {
            hosts.insert(host);
        }
    }
    ConsoleWrite("Jolly orb: found %d hosts with only coop phantoms", hosts.size());
    JollyHosts_Mtx.lock();
    JollyHosts = std::move(hosts);
    JollyHosts_Mtx.unlock();
}

//Returns true if this break in target should be skipped
bool BreakInTargetList_BlockCheck_injection_helper(void* steamid_str)
{
    if (!CustomInvasionTypes::jolly_search_active())
    {
        return false;
    }
    //std::string: small strings are stored inline
    const char* steamid_chars = (*(uint64_t*)((uint64_t)steamid_str + 0x18) >= 0x10) ? *(const char**)steamid_str : (const char*)steamid_str;
    uint64_t steamid = strtoull(steamid_chars, NULL, 16);
    JollyHosts_Mtx.lock();
    bool has_coop = JollyHosts.contains(steamid);
    JollyHosts_Mtx.unlock();
    if (!has_coop)
    {
        ConsoleWrite("Jolly orb: skipping target %llx", steamid);
    }
    return !has_coop;
}

static uint64_t published_coop_lobby = 0;
static int32_t published_coop_count = -1;
static int32_t published_hostile_count = -1;

//Publish the coop and hostile phantom counts in our world to our persistent lobby
static bool PublishCoopPhantomCount(void* unused)
{
    if (ModNetworking::SteamMatchmaking == nullptr || ModNetworking::SteamUser == nullptr || is_seamless_coop_present())
    {
        return true;
    }
    uint64_t lobby = ModNetworking::selfPersisantLobby.load();
    if (lobby == 0)
    {
        return true;
    }
    //Anyone who isn't a coop phantom is treated as hostile
    int32_t coop_count = 0;
    int32_t hostile_count = 0;
    auto playerins_o = Game::get_PlayerIns();
    if (Game::playerchar_is_loaded() && playerins_o.has_value() && playerins_o.value() != NULL &&
        Game::get_player_chr_type((uint64_t)playerins_o.value()) == PLAYER_STATUS::HUMAN)
    {
        for (uint32_t i = 1; i < 6; i++)
        {
            std::optional<uint64_t> guest_o = Game::get_connected_player(i);
            if (!guest_o.has_value())
            {
                break;
            }
            if (guest_o.value() == 0)
            {
                continue;
            }
            if (Game::get_player_chr_type(guest_o.value()) == PLAYER_STATUS::COOP)
            {
                coop_count++;
            }
            else
            {
                hostile_count++;
            }
        }
    }

    //only write on change, since every write is sent to everyone in the lobby
    if (lobby != published_coop_lobby || coop_count != published_coop_count || hostile_count != published_hostile_count)
    {
        //Invaders can't see the lobby owner without joining, so publish our id
        std::string hostid_str = std::to_string(ModNetworking::SteamUser->GetSteamID().ConvertToUint64());
        std::string coop_str = std::to_string(coop_count);
        std::string hostile_str = std::to_string(hostile_count);
        if (ModNetworking::SteamMatchmaking->SetLobbyData(lobby, MOD_LOBBY_HOSTID_KEY, hostid_str.c_str()) &&
            ModNetworking::SteamMatchmaking->SetLobbyData(lobby, MOD_LOBBY_COOPCOUNT_KEY, coop_str.c_str()) &&
            ModNetworking::SteamMatchmaking->SetLobbyData(lobby, MOD_LOBBY_HOSTILECOUNT_KEY, hostile_str.c_str()))
        {
            ConsoleWrite("Set lobby coop phantom count = %d, hostile phantom count = %d", coop_count, hostile_count);
            published_coop_lobby = lobby;
            published_coop_count = coop_count;
            published_hostile_count = hostile_count;
        }
    }
    return true;
}
