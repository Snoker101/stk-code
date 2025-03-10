//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013-2015 SuperTuxKart-Team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "network/protocols/server_lobby.hpp"

#include "addons/addon.hpp"
#include "config/user_config.hpp"
#include "items/network_item_manager.hpp"
#include "items/powerup_manager.hpp"
#include "karts/abstract_kart.hpp"
#include "karts/controller/player_controller.hpp"
#include "karts/kart_properties.hpp"
#include "karts/kart_properties_manager.hpp"
#include "karts/official_karts.hpp"
#include "modes/capture_the_flag.hpp"
#include "modes/linear_world.hpp"
#include "modes/soccer_world.hpp"
#include "network/crypto.hpp"
#include "network/database_connector.hpp"
#include "network/event.hpp"
#include "network/game_setup.hpp"
#include "network/network.hpp"
#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/peer_vote.hpp"
#include "network/protocol_manager.hpp"
#include "network/protocols/connect_to_peer.hpp"
#include "network/protocols/game_protocol.hpp"
#include "network/protocols/game_events_protocol.hpp"
#include "network/protocols/ranking.hpp"
#include "network/race_event_manager.hpp"
#include "network/server_config.hpp"
#include "network/socket_address.hpp"
#include "network/stk_host.hpp"
#include "network/stk_ipv6.hpp"
#include "network/stk_peer.hpp"
#include "online/online_profile.hpp"
#include "online/request_manager.hpp"
#include "online/xml_request.hpp"
#include "race/race_manager.hpp"
#include "tracks/check_manager.hpp"
#include "tracks/track.hpp"
#include "tracks/track_manager.hpp"
#include "utils/log.hpp"
#include "utils/random_generator.hpp"
#include "utils/string_utils.hpp"
#include "utils/time.hpp"
#include "utils/translation.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

#include <sstream>      // for std::stringstream
#include <string>       // for std::string
#include <vector>
#include <cstdlib>
#include <unordered_map>
#include <iomanip>
#include "sqlite3.h" //  You will have to install the package libsqlite3-dev. For ubuntu: "sudo apt install libsqlite3-dev"
                    // and build game with sqlite3 on: "cmake .. -DNO_SHADERC=on ENABLE_SQLITE3"

struct PlayerStats
{
    int rank;
    std::string name;
    float scoringPoints;
    float attackingPoints;
    float defendingPoints;
    float badPlayPoints;
    float total;
    float matchesPlayed;
    int matches_participated;
    int matchesWon;
    float teamMembersCount;
    float minutes_played_count;
};

// Helper function to retrieve a player by partial name from file
bool getPlayerFromFile(const std::string& fileName, const std::string& partialName, PlayerStats& outStats)
{
    std::ifstream file(fileName);
    if (!file.is_open())
    {
        std::cerr << "Could not open file: " << fileName << std::endl;
        return false;
    }

    // Skip header line
    std::string headerLine;
    if (!std::getline(file, headerLine))
    {
        file.close();
        return false;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty()) continue; // Skip empty lines

        std::stringstream ss(line);
        PlayerStats temp;
        ss >> temp.rank     // Rank
           >> temp.name     // Player
           >> temp.scoringPoints
           >> temp.attackingPoints
           >> temp.defendingPoints
           >> temp.badPlayPoints
           >> temp.total
           >> temp.matchesPlayed
           >> temp.matches_participated
           >> temp.matchesWon
           >> temp.teamMembersCount
           >> temp.minutes_played_count;

        // Check if the player's name contains the "partialName" substring
        // Example: "snoker" matches "sno", "gavr" matches "gav", etc.
        if (temp.name.find(partialName) != std::string::npos)
        {
            outStats = temp;
            file.close();
            return true; // Return the first match found
        }
    }
    file.close();
    return false;
}

// Maps a player's name to whether they have voted (true) for /mix
std::unordered_map<std::string, bool> m_mix_voters;

std::vector<std::string> tips; // vector to store the tips
std::string random_tip = "no tips for today, enjoy!";
  // List of joining messages
const std::vector<std::string> JOINED_MESSAGES = {
    "landed!",
    "arrived!",
    "dropped in!",
    "got here!",
    "rolled in!",
    "showed up!",
    "touched down!",
    "appeared!",
    "got in!",
    "turned up!",
    "disembarked!"
};

// Function to generate a random message
std::string generateRandomMessage(const std::vector<std::string> Messages) {
    std::srand(std::time(0));
    int randIndex = std::rand() % Messages.size();
    return Messages[randIndex];
}
std::string lastJoinedName;
std::string lastLeftName;

struct Player {
    std::string name;
    int rank;
    int score;
};

std::pair<int, int> getPlayerInfo(std::string playerName) {
    // First try: search in "soccer_ranking.txt"
    {
        std::ifstream file("soccer_ranking.txt");
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            int rank;
            std::string name;
            int score;
            if (!(iss >> rank >> name >> score)) {
                continue;
            }
            if (name == playerName) {
                return {rank, score};
            }
        }
    }

    // Second try: search in "soccer_ranking_2.txt"
    {
        std::ifstream file("soccer_ranking_fixed.txt");
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            int rank;
            std::string name;
            int score;
            if (!(iss >> rank >> name >> score)) {
                continue;
            }
            if (name == playerName) {
                return {rank, score};
            }
        }
    }

    // Return {-1, -1} if the player is not found in either file.
    return {-1, -1};
}

std::pair<int, int> getPlayerInfoCurrentRanking(std::string playerName) {
    std::vector<Player> players;
    std::ifstream file("soccer_ranking.txt");
    std::string line;
    while(std::getline(file, line)){
        std::istringstream iss(line);
        int rank;
        std::string name;
        int score;
        if(!(iss >> rank >> name >> score)) { continue; }
        players.push_back(Player{name, rank, score});
    }
    file.close();

    for(const auto& player : players) {
        if(player.name == playerName) {
            return {player.rank, player.score};
        }
    }

    return {-1, -1};  // return {-1, -1} if player not found
}

int ServerLobby::m_fixed_laps = -1;
unsigned int playerlimit = 10;
// ========================================================================
class SubmitRankingRequest : public Online::XMLRequest
{
public:
    SubmitRankingRequest(const RankingEntry& entry,
                         const std::string& country_code)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY)
    {
        addParameter("id", entry.online_id);
        addParameter("scores", entry.score);
        addParameter("max-scores", entry.max_score);
        addParameter("num-races-done", entry.races);
        addParameter("raw-scores", entry.raw_score);
        addParameter("rating-deviation", entry.deviation);
        addParameter("disconnects", entry.disconnects);
        addParameter("country-code", country_code);
    }
    virtual void afterOperation()
    {
        Online::XMLRequest::afterOperation();
        const XMLNode* result = getXMLData();
        std::string rec_success;
        if (!(result->get("success", &rec_success) &&
            rec_success == "yes"))
        {
            Log::error("ServerLobby", "Failed to submit scores.");
        }
    }
};   // UpdatePlayerRankingRequest
// ========================================================================

// We use max priority for all server requests to avoid downloading of addons
// icons blocking the poll request in all-in-one graphical client server

/** This is the central game setup protocol running in the server. It is
 *  mostly a finite state machine. Note that all nodes in ellipses and light
 *  grey background are actual states; nodes in boxes and white background 
 *  are functions triggered from a state or triggering potentially a state
 *  change.
 \dot
 digraph interaction {
 node [shape=box]; "Server Constructor"; "playerTrackVote"; "connectionRequested"; 
                   "signalRaceStartToClients"; "startedRaceOnClient"; "loadWorld";
 node [shape=ellipse,style=filled,color=lightgrey];

 "Server Constructor" -> "INIT_WAN" [label="If WAN game"]
 "Server Constructor" -> "WAITING_FOR_START_GAME" [label="If LAN game"]
 "INIT_WAN" -> "GETTING_PUBLIC_ADDRESS" [label="GetPublicAddress protocol callback"]
 "GETTING_PUBLIC_ADDRESS" -> "WAITING_FOR_START_GAME" [label="Register server"]
 "WAITING_FOR_START_GAME" -> "connectionRequested" [label="Client connection request"]
 "connectionRequested" -> "WAITING_FOR_START_GAME"
 "WAITING_FOR_START_GAME" -> "SELECTING" [label="Start race from authorised client"]
 "SELECTING" -> "SELECTING" [label="Client selects kart, #laps, ..."]
 "SELECTING" -> "playerTrackVote" [label="Client selected track"]
 "playerTrackVote" -> "SELECTING" [label="Not all clients have selected"]
 "playerTrackVote" -> "LOAD_WORLD" [label="All clients have selected; signal load_world to clients"]
 "LOAD_WORLD" -> "loadWorld"
 "loadWorld" -> "WAIT_FOR_WORLD_LOADED" 
 "WAIT_FOR_WORLD_LOADED" -> "WAIT_FOR_WORLD_LOADED" [label="Client or server loaded world"]
 "WAIT_FOR_WORLD_LOADED" -> "signalRaceStartToClients" [label="All clients and server ready"]
 "signalRaceStartToClients" -> "WAIT_FOR_RACE_STARTED"
 "WAIT_FOR_RACE_STARTED" ->  "startedRaceOnClient" [label="Client has started race"]
 "startedRaceOnClient" -> "WAIT_FOR_RACE_STARTED" [label="Not all clients have started"]
 "startedRaceOnClient" -> "DELAY_SERVER" [label="All clients have started"]
 "DELAY_SERVER" -> "DELAY_SERVER" [label="Not done waiting"]
 "DELAY_SERVER" -> "RACING" [label="Server starts race now"]
 }
 \enddot


 *  It starts with detecting the public ip address and port of this
 *  host (GetPublicAddress).
 */
ServerLobby::ServerLobby() : LobbyProtocol()
{
    m_client_server_host_id.store(0);
    m_lobby_players.store(0);
    m_current_ai_count.store(0);
    std::vector<int> all_t =
        track_manager->getTracksInGroup("standard");
    std::vector<int> all_arenas =
        track_manager->getArenasInGroup("standard", false);
    std::vector<int> all_soccers =
        track_manager->getArenasInGroup("standard", true);
    all_t.insert(all_t.end(), all_arenas.begin(), all_arenas.end());
    all_t.insert(all_t.end(), all_soccers.begin(), all_soccers.end());

    m_official_kts.first = OfficialKarts::getOfficialKarts();
    for (int track : all_t)
    {
        Track* t = track_manager->getTrack(track);
        if (!t->isAddon())
            m_official_kts.second.insert(t->getIdent());
    }
    updateAddons();

    m_rs_state.store(RS_NONE);
    m_last_success_poll_time.store(StkTime::getMonoTimeMs() + 30000);
    m_last_unsuccess_poll_time = StkTime::getMonoTimeMs();
    m_server_owner_id.store(-1);
    m_registered_for_once_only = false;
    setHandleDisconnections(true);
    m_state = SET_PUBLIC_ADDRESS;
    m_save_server_config = true;
    if (ServerConfig::m_ranked)
    {
        Log::info("ServerLobby", "This server will submit ranking scores to "
            "the STK addons server. Don't bother hosting one without the "
            "corresponding permissions, as they would be rejected.");

        m_ranking = std::make_shared<Ranking>();
    }
    m_result_ns = getNetworkString();
    m_result_ns->setSynchronous(true);
    m_items_complete_state = new BareNetworkString();
    m_server_id_online.store(0);
    m_difficulty.store(ServerConfig::m_server_difficulty);
    m_game_mode.store(ServerConfig::m_server_mode);
    m_default_vote = new PeerVote();

#ifdef ENABLE_SQLITE3
    m_db_connector = new DatabaseConnector();
    m_db_connector->initDatabase();
#endif
}   // ServerLobby

//-----------------------------------------------------------------------------
/** Destructor.
 */
ServerLobby::~ServerLobby()
{
    if (m_server_id_online.load() != 0)
    {
        // For child process the request manager will keep on running
        unregisterServer(m_process_type == PT_MAIN ? true : false/*now*/);
    }
    delete m_result_ns;
    delete m_items_complete_state;
    if (m_save_server_config)
        ServerConfig::writeServerConfigToDisk();
    delete m_default_vote;

#ifdef ENABLE_SQLITE3
    m_db_connector->destroyDatabase();
    delete m_db_connector;
#endif
}   // ~ServerLobby

//-----------------------------------------------------------------------------

void ServerLobby::initServerStatsTable()
{
#ifdef ENABLE_SQLITE3
    m_db_connector->initServerStatsTable();
#endif
}   // initServerStatsTable

//-----------------------------------------------------------------------------
void ServerLobby::updateAddons()
{
    m_addon_kts.first.clear();
    m_addon_kts.second.clear();
    m_addon_arenas.clear();
    m_addon_soccers.clear();

    std::set<std::string> total_addons;
    for (unsigned i = 0; i < kart_properties_manager->getNumberOfKarts(); i++)
    {
        const KartProperties* kp =
            kart_properties_manager->getKartById(i);
        if (kp->isAddon())
            total_addons.insert(kp->getIdent());
    }
    for (unsigned i = 0; i < track_manager->getNumberOfTracks(); i++)
    {
        const Track* track = track_manager->getTrack(i);
        if (track->isAddon())
            total_addons.insert(track->getIdent());
    }

    for (auto& addon : total_addons)
    {
        const KartProperties* kp = kart_properties_manager->getKart(addon);
        if (kp && kp->isAddon())
        {
            m_addon_kts.first.insert(kp->getIdent());
            continue;
        }
        Track* t = track_manager->getTrack(addon);
        if (!t || !t->isAddon() || t->isInternal())
            continue;
        if (t->isArena())
            m_addon_arenas.insert(t->getIdent());
        else if (t->isSoccer())
            m_addon_soccers.insert(t->getIdent());
        else
            m_addon_kts.second.insert(t->getIdent());
    }

    std::vector<std::string> all_k;
    for (unsigned i = 0; i < kart_properties_manager->getNumberOfKarts(); i++)
    {
        const KartProperties* kp = kart_properties_manager->getKartById(i);
        if (kp->isAddon())
            all_k.push_back(kp->getIdent());
    }
    std::set<std::string> oks = OfficialKarts::getOfficialKarts();
    if (all_k.size() >= 65536 - (unsigned)oks.size())
        all_k.resize(65535 - (unsigned)oks.size());
    for (const std::string& k : oks)
        all_k.push_back(k);
    if (ServerConfig::m_live_players)
        m_available_kts.first = m_official_kts.first;
    else
        m_available_kts.first = { all_k.begin(), all_k.end() };
}   // updateAddons

//-----------------------------------------------------------------------------
/** Called whenever server is reset or game mode is changed.
 */
void ServerLobby::updateTracksForMode()
{
    set_powerup_multiplier(1);
    auto all_t = track_manager->getAllTrackIdentifiers();
    if (all_t.size() >= 65536)
        all_t.resize(65535);
    m_available_kts.second = { all_t.begin(), all_t.end() };
    RaceManager::MinorRaceModeType m =
        ServerConfig::getLocalGameMode(m_game_mode.load()).first;

    switch (m)
    {
        case RaceManager::MINOR_MODE_NORMAL_RACE:
        case RaceManager::MINOR_MODE_TIME_TRIAL:
        case RaceManager::MINOR_MODE_FOLLOW_LEADER:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (t->isArena() || t->isSoccer() || t->isInternal())
                {
                    it = m_available_kts.second.erase(it);
                }
                else
                    it++;
            }
            break;
        }
        case RaceManager::MINOR_MODE_FREE_FOR_ALL:
        case RaceManager::MINOR_MODE_CAPTURE_THE_FLAG:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (RaceManager::get()->getMinorMode() ==
                    RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
                {
                    if (!t->isCTF() || t->isInternal())
                    {
                        it = m_available_kts.second.erase(it);
                    }
                    else
                        it++;
                }
                else
                {
                    if (!t->isArena() ||  t->isInternal())
                    {
                        it = m_available_kts.second.erase(it);
                    }
                    else
                        it++;
                }
            }
            break;
        }
        case RaceManager::MINOR_MODE_SOCCER:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (!t->isSoccer() || t->isInternal())
                {
                    it = m_available_kts.second.erase(it);
                }
                else
                    it++;
            }
            break;
        }
        default:
            assert(false);
            break;
    }
    lastJoinedName = "";
    lastLeftName = "";
    m_mix_voters.clear();

    // Read tips from the file
    std::ifstream file("tips.txt");
    if (file.is_open())
    {
        std::string tip;
        while (std::getline(file, tip))
        {
            tips.push_back(tip);
        }
        file.close();
    }
    else
    {
        Log::warn("ServerLobby", "tips.txt file not found, creating it with sample tips");

        // Create the file with sample tips
        std::ofstream new_file("tips.txt");
        new_file << "Practice your drifting skills to get better at corners." << std::endl;
        new_file << "Use nitro wisely, don't burn it all at once." << std::endl;
        new_file << "Collect nitro bottles for a speed boost." << std::endl;
        new_file << "Use bananas and other projectiles to slow down opponents." << std::endl;
        new_file << "Pay attention to the mini-map to plan your route." << std::endl;
        new_file.close();

        // Read the newly created file
        std::ifstream new_file_read("tips.txt");
        std::string tip;
        while (std::getline(new_file_read, tip))
        {
            tips.push_back(tip);
        }
        new_file_read.close();
    }

    random_tip = generateRandomMessage(tips);

 // --------------------------------------------------------------------------
// Fetch all rows from the “players” table, compute, and write them to text files
// 1) detailed file: soccer_ranking_detailed.txt
// 2) summary file:  soccer_ranking.txt
// 3) full dump:     db_table_copy.txt  (no min. minutes filter)
// --------------------------------------------------------------------------
{
    sqlite3* db2 = nullptr;
    int rc2 = sqlite3_open("soccer_ranking_detailed.db", &db2);
    if (rc2 != SQLITE_OK)
    {
        Log::warn("SoccerWorld",
                  (std::string("Cannot open SQLite DB for fetch: ") +
                   sqlite3_errmsg(db2)).c_str());
        if (db2) sqlite3_close(db2);
    }
    else
    {
        // Open the detailed output file
        std::ofstream out_file_detailed("soccer_ranking_detailed.txt");
        if (!out_file_detailed.is_open())
        {
            Log::warn("SoccerWorld",
                      "Cannot open soccer_ranking_detailed.txt for writing.");
            sqlite3_close(db2);
            return;
        }

        // Open the summary output file
        std::ofstream out_file_short("soccer_ranking.txt");
        if (!out_file_short.is_open())
        {
            Log::warn("SoccerWorld",
                      "Cannot open soccer_ranking.txt for writing.");
            sqlite3_close(db2);
            out_file_detailed.close();
            return;
        }

        //
        // NEW SECTION: Open the "db_table_copy.txt" file
        //
        std::ofstream out_file_copy("db_table_copy.txt");
        if (!out_file_copy.is_open())
        {
            Log::warn("SoccerWorld",
                      "Cannot open db_table_copy.txt for writing.");
            sqlite3_close(db2);
            out_file_detailed.close();
            out_file_short.close();
            return;
        }

        // Prepare the select statement
        const char* select_sql =
            "SELECT "
            "  PlayerName, "
            "  ScoringPts, "
            "  AttackingPts, "
            "  DefendingPts, "
            "  IndDefendingPts, "
            "  BadPlayPts, "
            "  Total, "
            "  Rank, "
            "  matches_played, "
            "  matches_participated, "
            "  matches_won, "
            "  team_members_count, "
            "  minutes_played_count "
            "FROM players "
            "ORDER BY Total DESC;";

        sqlite3_stmt* stmt_fetch = nullptr;
        rc2 = sqlite3_prepare_v2(db2, select_sql, -1, &stmt_fetch, nullptr);
        if (rc2 != SQLITE_OK)
        {
            Log::warn("SoccerWorld",
                      (std::string("Failed to prepare SELECT statement: ") +
                       sqlite3_errmsg(db2)).c_str());
            sqlite3_close(db2);
            return;
        }

        // We'll store data in a custom struct for sorting and ranking
        struct PlayerRow
        {
            std::string playerName;
            float scoringPtsPerMatch;
            float attackingPtsPerMatch;
            float defendingPtsPerMatch;
            float badPlayPtsPerMatch;
            float totalPerMatch;
            float matchesPlayed;
            int   matchesParticipated;
            int   matchesWon;
            float teamMembersCountPerMatch;
            float minutesPlayedCount;
            int   rank; // We'll assign rank after sorting

            // For the "col" script logic (raw stats):
            float rawScoringPts;
            float rawAttackingPts;
            float rawDefendingPts;
            float rawBadPlayPts;

            // Percentages computed by col script:
            float allPercent;
            float scorePercent;
            float attackPercent;
            float defendPercent;
            float badPlayPercent;
        };

        std::vector<PlayerRow> allPlayers;

        //
        // Write a header line into db_table_copy.txt
        //
        out_file_copy
            << "PlayerName,ScoringPts,AttackingPts,DefendingPts,IndDefendingPts,"
            << "BadPlayPts,Total,Rank,matches_played,matches_participated,"
            << "matches_won,team_members_count,minutes_played_count\n";

        // Fetch rows and compute per-minute metrics
        while ((rc2 = sqlite3_step(stmt_fetch)) == SQLITE_ROW)
        {
            const unsigned char* playerName = sqlite3_column_text(stmt_fetch, 0);
            float scoringPts         = (float)sqlite3_column_int(stmt_fetch, 1);
            float attackingPts       = (float)sqlite3_column_int(stmt_fetch, 2);
            float defendingPts       = (float)sqlite3_column_int(stmt_fetch, 3);
            float inddefendingPts       = (float)sqlite3_column_int(stmt_fetch, 4);
            float badPlayPts         = (float)sqlite3_column_int(stmt_fetch, 5);
            float matchesPlayed      = (float)sqlite3_column_double(stmt_fetch, 8);
            int   matchesParticipated= sqlite3_column_int(stmt_fetch, 9);
            int   matchesWon         = sqlite3_column_int(stmt_fetch, 10);
            float teamMembersCount   = (float)sqlite3_column_int(stmt_fetch, 11);
            float minutesPlayedCount = (float)sqlite3_column_double(stmt_fetch, 12);

            // -------------------------------------------------
            // Write the unfiltered row directly to db_table_copy.txt
            // (no minutesPlayedCount >= 30 condition)
            // -------------------------------------------------
            {
                // If you simply want to copy the columns as they are:
                // (No per-minute calculations for db_table_copy.txt)
                out_file_copy
                    << (playerName ? reinterpret_cast<const char*>(playerName) : "(null)") << ","
                    << scoringPts       << ","
                    << attackingPts     << ","
                    << defendingPts     << ","
                    << inddefendingPts     << ","
                    << badPlayPts       << ","
                    << /* 6 = total */     /* Whether or not you want to output it by reading? */
                    sqlite3_column_int(stmt_fetch, 5) << ","
                    << /* 7 = rank  */       sqlite3_column_int(stmt_fetch, 6) << ","
                    << matchesPlayed    << ","
                    << matchesParticipated << ","
                    << matchesWon       << ","
                    << teamMembersCount << ","
                    << minutesPlayedCount << "\n";
            }

            //
            // Keep the old (minutesPlayedCount >= 30.0f) logic for ranking:
            //
            if ((minutesPlayedCount >= 30.0f) && (matchesPlayed >= 8))
            {
                float scoringPtsPerMatch     =
                    (matchesPlayed == 0.0f) ? 0.0f :
                    std::round(scoringPts / matchesPlayed * 100.0f) / 100.0f;
                float attackingPtsPerMatch   =
                    (matchesPlayed == 0.0f) ? 0.0f :
                    std::round(attackingPts / matchesPlayed * 100.0f) / 100.0f;
                float defendingPtsPerMatch   =
                    (matchesPlayed == 0.0f) ? 0.0f :
                    std::round((defendingPts + inddefendingPts) / matchesPlayed * 100.0f) / 100.0f;
                float badPlayPtsPerMatch     =
                    (matchesPlayed == 0.0f) ? 0.0f :
                    std::round(badPlayPts / matchesPlayed * 100.0f) / 100.0f;
                float teamMembersCountPerMatch =
                    (static_cast<float>(matchesParticipated) == 0.0f) ? 0.0f :
                    std::round(teamMembersCount / static_cast<float>(matchesParticipated) * 100.0f) / 100.0f;

                float totalPerMatch =
                    std::round(
                        (scoringPtsPerMatch
                        + attackingPtsPerMatch
                        + defendingPtsPerMatch
                        + badPlayPtsPerMatch)
                        * teamMembersCountPerMatch
                        * 100.0f
                    ) / 100.0f;

                PlayerRow row;
                row.playerName                = (playerName
                                                 ? reinterpret_cast<const char*>(playerName)
                                                 : "(null)");
                row.scoringPtsPerMatch      = scoringPtsPerMatch;
                row.attackingPtsPerMatch    = attackingPtsPerMatch;
                row.defendingPtsPerMatch    = defendingPtsPerMatch;
                row.badPlayPtsPerMatch      = badPlayPtsPerMatch;
                row.totalPerMatch           = totalPerMatch;
                row.matchesPlayed             = matchesPlayed;
                row.matchesParticipated       = matchesParticipated;
                row.matchesWon                = matchesWon;
                row.teamMembersCountPerMatch= teamMembersCountPerMatch;
                row.minutesPlayedCount        = minutesPlayedCount;
                row.rank = 0; // placeholder (will assign after sorting)

                 // Save raw stats for col script:
                row.rawScoringPts   = scoringPts;
                row.rawAttackingPts = attackingPts;
                row.rawDefendingPts = defendingPts;
                row.rawBadPlayPts   = badPlayPts;
                // Initialize col script percentages
                row.allPercent      = 0.0f;
                row.scorePercent    = 0.0f;
                row.attackPercent   = 0.0f;
                row.defendPercent   = 0.0f;
                row.badPlayPercent  = 0.0f;

                allPlayers.push_back(row);
            }
        }

        if (rc2 != SQLITE_DONE)
        {
            Log::warn("SoccerWorld",
                      (std::string("Select statement did not finish successfully: ") +
                       sqlite3_errmsg(db2)).c_str());
        }

        sqlite3_finalize(stmt_fetch);
        sqlite3_close(db2);

        // Sort the collected players by totalPerMatch (descending)
        std::sort(allPlayers.begin(), allPlayers.end(),
                  [](const PlayerRow& a, const PlayerRow& b)
                  {
                      return a.totalPerMatch > b.totalPerMatch;
                  });

        // Assign ranks (players with the same totalPerMatch get the same rank)
        if (!allPlayers.empty())
        {
            int currentRank = 1;
            allPlayers[0].rank = currentRank;

            for (size_t i = 1; i < allPlayers.size(); ++i)
            {
                if (std::fabs(allPlayers[i].totalPerMatch
                              - allPlayers[i - 1].totalPerMatch) < 1e-6f)
                {
                    // Same totalPerMatch => same rank
                    allPlayers[i].rank = allPlayers[i - 1].rank;
                }
                else
                {
                    // Different totalPerMatch => new rank is i+1 (1-based)
                    allPlayers[i].rank = static_cast<int>(i + 1);
                }
            }
        }

        // 1) Write the detailed header row
        out_file_detailed
            << "Rank PlayerName ScoringPts/match AttackingPts/match DefendingPts/match "
            << "BadPlayPts/match totalPerMatch matches_played matches_participated "
            << "matches_won teamMembersCount/match minutes_played_count\n";

        // 2) Write the summary header row
        out_file_short << "Rank PlayerName Points\n";

        // Write the data with rank (detailed and summary)
        for (const auto& row : allPlayers)
        {
            // soccer_ranking_detailed.txt
            out_file_detailed
                << row.rank << " "
                << row.playerName << " "
                << row.scoringPtsPerMatch << " "
                << row.attackingPtsPerMatch << " "
                << row.defendingPtsPerMatch << " "
                << row.badPlayPtsPerMatch << " "
                << row.totalPerMatch << " "
                << row.matchesPlayed << " "
                << row.matchesParticipated << " "
                << row.matchesWon << " "
                << row.teamMembersCountPerMatch << " "
                << row.minutesPlayedCount << "\n";

            // soccer_ranking.txt (just rank, name, totalPerMatch)
            out_file_short
                << row.rank << " "
                << row.playerName << " "
                << row.totalPerMatch << "\n";
        }

        // Close all files
        out_file_detailed.close();
        out_file_short.close();
        out_file_copy.close();

        Log::info("SoccerWorld",
                  "Data written to soccer_ranking_detailed.txt, soccer_ranking.txt, "
                  "and db_table_copy.txt successfully.");
 // -----------------------------------------------------------------
        // 2) COL SCRIPT LOGIC: same filter, compute means, factors, etc.
        //    Then sort by All% and write to "colallstats.txt"
        // -----------------------------------------------------------------
        if (!allPlayers.empty())
        {
            // Compute sums
            float sumScoring  = 0.0f;
            float sumAttack   = 0.0f;
            float sumDefend   = 0.0f;
            float sumBadPlay  = 0.0f;
            float sumMinutes  = 0.0f;

            for (const auto& p : allPlayers)
            {
                sumScoring += p.rawScoringPts;
                sumAttack  += p.rawAttackingPts;
                sumDefend  += p.rawDefendingPts;
                sumBadPlay += p.rawBadPlayPts;
                sumMinutes += p.minutesPlayedCount;
            }
            const int N = static_cast<int>(allPlayers.size());
            if (N == 0)
            {
                Log::info("SoccerWorld", "No players meet criteria for colallstats.txt");
            }
            else
            {
                // Averages
                float MedianScore   = sumScoring  / N;
                float MedianAttack  = sumAttack   / N;
                float MedianDefend  = sumDefend   / N;
                float MedianBadPlay = sumBadPlay  / N;
                float MedianTime    = sumMinutes  / N;

                // Factors (watch out for zero)
                float FactorScore   = (MedianScore  == 0.0f) ? 0.0f : (MedianAttack / MedianScore);
                float FactorAttack  =  MedianAttack / 100.0f;
                float FactorDefend  = (MedianDefend == 0.0f) ? 0.0f : (MedianAttack / MedianDefend);
                float FactorBadPlay = (MedianBadPlay== 0.0f) ? 0.0f : (MedianAttack / MedianBadPlay);
                float Equalizer100  = (MedianAttack == 0.0f) ? 0.0f : ((1.0f / MedianAttack) * MedianTime);

                // Compute each player's percentages
                for (auto &p : allPlayers)
                {
                    float scoring = p.rawScoringPts;
                    float attack  = p.rawAttackingPts;
                    float defend  = p.rawDefendingPts;
                    float badplay = p.rawBadPlayPts;
                    float minutes = p.minutesPlayedCount;

                    if (minutes < 1e-6f)
                    {
                        // Avoid division by zero
                        p.scorePercent   = 0.0f;
                        p.attackPercent  = 0.0f;
                        p.defendPercent  = 0.0f;
                        p.badPlayPercent = 0.0f;
                        p.allPercent     = 0.0f;
                    }
                    else
                    {
                        float denom       = (minutes / 100.0f);
                        p.scorePercent    = (FactorScore   * Equalizer100 * scoring)  / denom;
                        p.attackPercent   = (1.0f          * Equalizer100 * attack )  / denom;
                        p.defendPercent   = (FactorDefend  * Equalizer100 * defend )  / denom;
                        p.badPlayPercent  = (FactorBadPlay * Equalizer100 * badplay)  / denom;

                        p.allPercent
                            = (((5.0f / 3.0f)
                                * (p.scorePercent + p.attackPercent + p.defendPercent))
                               - p.badPlayPercent
                              ) / 4.0f;
                    }
                }

                // Sort by All% descending
std::sort(allPlayers.begin(), allPlayers.end(),
          [](const PlayerRow& a, const PlayerRow& b){
              return a.allPercent > b.allPercent;
          });

// Write results to "colallstats.txt"
std::ofstream out_file_col("colallstats.txt");
if (!out_file_col.is_open())
{
    Log::warn("SoccerWorld", "Cannot open colallstats.txt for writing.");
}
else
{
    // Set a reasonable precision for floats:
    out_file_col << std::fixed << std::setprecision(2);

    // Print header row with aligned columns including the new Rank column.
    // We use a width of 5 characters for Rank.
    out_file_col
        << std::left  << std::setw(5)  << "Rank"
        << std::left  << std::setw(15) << "PlayerName"
        << std::right << std::setw(10) << "All%"
        << std::right << std::setw(10) << "Score%"
        << std::right << std::setw(10) << "Attack%"
        << std::right << std::setw(10) << "Defend%"
        << std::right << std::setw(10) << "BadPlay%"
        << "\n";

    // (Optional) print a separator line:
    // Total width = 5 (Rank) + 15 (PlayerName) + 5*10 = 70 characters.
    //out_file_col << std::string(70, '-') << "\n";

    // Use an index-based loop to calculate and output ranks.
    int currentRank = 1;
    // Using index so that we can compare with the previous player's All%
    for (size_t i = 0; i < allPlayers.size(); i++)
    {
        // For the first player, rank is 1.
        // For later players, if the current player's All% is different from the previous,
        // update the rank to i+1 (since the vector is 0-indexed).
        if (i > 0 && allPlayers[i].allPercent != allPlayers[i-1].allPercent)
        {
            currentRank = i + 1;
        }
        // For badPlayPercent, create a formatted string with a "-" sign prefixed.
            std::ostringstream ossBadPlay;
            ossBadPlay << "-" << allPlayers[i].badPlayPercent;
            std::string badPlayStr = ossBadPlay.str();
        out_file_col
            << std::left  << std::setw(5)  << currentRank
            << std::left  << std::setw(15) << allPlayers[i].playerName
            << std::right << std::setw(10) << allPlayers[i].allPercent
            << std::right << std::setw(10) << allPlayers[i].scorePercent
            << std::right << std::setw(10) << allPlayers[i].attackPercent
            << std::right << std::setw(10) << allPlayers[i].defendPercent
            << std::right << std::setw(10) << badPlayStr
            << "\n";
    }
    out_file_col.close();
}

            }
        }

    // -----------------------------------------------------------------
    // Part 3: NEW LOGIC -- Read from soccer_ranking_detailed_sw.txt,
    //         force minutes=1, compute COL results => collastmatchstats.txt
    // -----------------------------------------------------------------
    {
        std::ifstream fLastMatch("soccer_ranking_detailed_sw.txt");
        if (!fLastMatch.is_open())
        {
            Log::warn("SoccerWorld",
                      "Cannot open soccer_ranking_detailed_sw.txt for reading last-match data.");
            return;  // or continue if you want
        }

        // We'll store last-match data with forced minutes=1
        struct LastMatchRow
        {
            std::string playerName;
            float scoringPts;
            float attackingPts;
            float defendingPts;
            float badPlayPts;
            float minutes; // forced = 1
            // Computed for the COL logic
            float allPercent;
            float scorePercent;
            float attackPercent;
            float defendPercent;
            float badPlayPercent;
        };

        std::vector<LastMatchRow> lastMatchPlayers;

        // Skip header line
        std::string header;
        if (!std::getline(fLastMatch, header))
        {
            Log::warn("SoccerWorld", "soccer_ranking_detailed_sw.txt is empty.");
            fLastMatch.close();
            return;
        }

        // Parse lines
        while (true)
        {
            std::string line;
            if (!std::getline(fLastMatch, line)) break;
            if (line.empty()) continue;

            // Example line:
            // "1 mahmoudtark25 3 9 1 -1 12"
            std::stringstream ss(line);

            int rank;
            std::string name;
            float scoring, attacking, defending, badPlay, total;
            ss >> rank >> name >> scoring >> attacking >> defending >> badPlay >> total;
            if (ss.fail()) continue; // skip if parse error

            LastMatchRow row;
            row.playerName     = name;
            row.scoringPts     = scoring;
            row.attackingPts   = attacking;
            row.defendingPts   = defending;
            row.badPlayPts     = badPlay;
            row.minutes        = 1.0f;  // forced
            row.allPercent     = 0.0f;
            row.scorePercent   = 0.0f;
            row.attackPercent  = 0.0f;
            row.defendPercent  = 0.0f;
            row.badPlayPercent = 0.0f;

            lastMatchPlayers.push_back(row);
        }
        fLastMatch.close();

        if (lastMatchPlayers.empty())
        {
            Log::info("SoccerWorld", "No last-match players found in soccer_ranking_detailed_sw.txt.");
            return;
        }

        // -- COL algorithm on lastMatchPlayers (everyone has minutes=1) --
        float sumScoring = 0.0f;
        float sumAttack  = 0.0f;
        float sumDefend  = 0.0f;
        float sumBadPlay = 0.0f;
        float sumMinutes = 0.0f;

        for (auto &p : lastMatchPlayers)
        {
            sumScoring += p.scoringPts;
            sumAttack  += p.attackingPts;
            sumDefend  += p.defendingPts;
            sumBadPlay += p.badPlayPts;
            sumMinutes += p.minutes;  // always 1
        }

        int N = static_cast<int>(lastMatchPlayers.size());
        if (N == 0)
        {
            Log::info("SoccerWorld", "No valid players in last match file.");
            return;
        }

        float MedianScore   = sumScoring  / N;
        float MedianAttack  = sumAttack   / N;
        float MedianDefend  = sumDefend   / N;
        float MedianBadPlay = sumBadPlay  / N;
        float MedianTime    = sumMinutes  / N; // ~1 if we forced 1

        float FactorScore   = (MedianScore  == 0.0f) ? 0.0f : (MedianAttack / MedianScore);
        float FactorAttack  =  MedianAttack / 100.0f;
        float FactorDefend  = (MedianDefend == 0.0f) ? 0.0f : (MedianAttack / MedianDefend);
        float FactorBadPlay = (MedianBadPlay== 0.0f) ? 0.0f : (MedianAttack / MedianBadPlay);
        float Equalizer100  = (MedianAttack == 0.0f) ? 0.0f : ((1.0f / MedianAttack) * MedianTime);

        for (auto &p : lastMatchPlayers)
        {
            float scoring = p.scoringPts;
            float attack  = p.attackingPts;
            float defend  = p.defendingPts;
            float badPlay = p.badPlayPts;
            float minutes = p.minutes; // 1

            // Denominator = minutes / 100 => 0.01
            float denom = (minutes / 100.0f);
            if (denom < 1e-9f)
            {
                // Should not happen if minutes=1, but just in case:
                p.scorePercent   = 0.0f;
                p.attackPercent  = 0.0f;
                p.defendPercent  = 0.0f;
                p.badPlayPercent = 0.0f;
                p.allPercent     = 0.0f;
            }
            else
            {
                p.scorePercent   = (FactorScore   * Equalizer100 * scoring ) / denom;
                p.attackPercent  = (1.0f          * Equalizer100 * attack  ) / denom;
                p.defendPercent  = (FactorDefend  * Equalizer100 * defend  ) / denom;
                p.badPlayPercent = (FactorBadPlay * Equalizer100 * badPlay ) / denom;

                p.allPercent
                    = (((5.0f / 3.0f)
                        * (p.scorePercent + p.attackPercent + p.defendPercent))
                       - p.badPlayPercent
                      ) / 4.0f;
            }
        }

// Sort by All% descending
std::sort(lastMatchPlayers.begin(), lastMatchPlayers.end(),
          [](const LastMatchRow &a, const LastMatchRow &b){
              return a.allPercent > b.allPercent;
          });

// Write to collastmatchstats.txt
std::ofstream outLast("collastmatchstats.txt");
if (!outLast.is_open())
{
    Log::warn("SoccerWorld", "Cannot open collastmatchstats.txt for writing.");
    return;
}

outLast << std::fixed << std::setprecision(2);

// Print header row with Rank column added.
outLast
    << std::left  << std::setw(5)  << "Rank"
    << std::left  << std::setw(15) << "PlayerName"
    << std::right << std::setw(10) << "All%"
    << std::right << std::setw(10) << "Score%"
    << std::right << std::setw(10) << "Attack%"
    << std::right << std::setw(10) << "Defend%"
    << std::right << std::setw(10) << "BadPlay%"
    << "\n";

// Total width = 5 (Rank) + 15 (PlayerName) + 5*10 = 70 characters.
//outLast << std::string(70, '-') << "\n";

// Calculate rank and output each row.
// Tied players (equal allPercent) share the same rank.
int currentRank = 1;
for (size_t i = 0; i < lastMatchPlayers.size(); i++)
{
    // For players beyond the first one, update rank if current player's All% is lower than previous.
    if (i > 0 && lastMatchPlayers[i].allPercent != lastMatchPlayers[i - 1].allPercent)
    {
        currentRank = i + 1;
    }

    // Format badPlayPercent with a preceding "-" sign.
    std::ostringstream ossBadPlay;
    ossBadPlay << "-" << lastMatchPlayers[i].badPlayPercent;
    std::string badPlayStr = ossBadPlay.str();

    outLast
        << std::left  << std::setw(5)  << currentRank
        << std::left  << std::setw(15) << lastMatchPlayers[i].playerName
        << std::right << std::setw(10) << lastMatchPlayers[i].allPercent
        << std::right << std::setw(10) << lastMatchPlayers[i].scorePercent
        << std::right << std::setw(10) << lastMatchPlayers[i].attackPercent
        << std::right << std::setw(10) << lastMatchPlayers[i].defendPercent
        << std::right << std::setw(10) << badPlayStr
        << "\n";
}

outLast.close();

        Log::info("SoccerWorld",
                  "Last-match COL stats saved to collastmatchstats.txt successfully.");
    }

    // Done
    Log::info("SoccerWorld",
              "Finished updateTracksForMode with last-match COL stats.");


        Log::info("SoccerWorld",
                  "Data written to soccer_ranking_detailed.txt, soccer_ranking.txt, "
                  "db_table_copy.txt, and colallstats.txt successfully.");
    }

}


} // updateTracksForMode


//-----------------------------------------------------------------------------
void ServerLobby::setup()
{
    LobbyProtocol::setup();
    m_battle_hit_capture_limit = 0;
    m_battle_time_limit = 0.0f;
    m_item_seed = 0;
    m_winner_peer_id = 0;
    m_client_starting_time = 0;
    m_ai_count = 0;
    auto players = STKHost::get()->getPlayersForNewGame();
    if (m_game_setup->isGrandPrix() && !m_game_setup->isGrandPrixStarted())
    {
        for (auto player : players)
            player->resetGrandPrixData();
    }
    if (!m_game_setup->isGrandPrix() || !m_game_setup->isGrandPrixStarted())
    {
        for (auto player : players)
            player->setKartName("");
    }
    if (auto ai = m_ai_peer.lock())
    {
        for (auto player : ai->getPlayerProfiles())
            player->setKartName("");
    }
    for (auto ai : m_ai_profiles)
        ai->setKartName("");

    StateManager::get()->resetActivePlayers();
    // We use maximum 16bit unsigned limit
    auto all_k = kart_properties_manager->getAllAvailableKarts();
    if (all_k.size() >= 65536)
        all_k.resize(65535);
    if (ServerConfig::m_live_players)
        m_available_kts.first = m_official_kts.first;
    else
        m_available_kts.first = { all_k.begin(), all_k.end() };
    NetworkConfig::get()->setTuxHitboxAddon(ServerConfig::m_live_players);
    updateTracksForMode();

    m_server_has_loaded_world.store(false);

    // Initialise the data structures to detect if all clients and 
    // the server are ready:
    resetPeersReady();
    m_timeout.store(std::numeric_limits<int64_t>::max());
    m_server_started_at = m_server_delay = 0;
    Log::info("ServerLobby", "Resetting the server to its initial state.");
}   // setup

//-----------------------------------------------------------------------------
bool ServerLobby::notifyEvent(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() != EVENT_TYPE_MESSAGE)
        return true;

    NetworkString &data = event->data();
    assert(data.size()); // message not empty
    uint8_t message_type;
    message_type = data.getUInt8();
    Log::info("ServerLobby", "Synchronous message of type %d received.",
              message_type);
    switch (message_type)
    {
    case LE_RACE_FINISHED_ACK: playerFinishedResult(event);   break;
    case LE_LIVE_JOIN:         liveJoinRequest(event);        break;
    case LE_CLIENT_LOADED_WORLD: finishedLoadingLiveJoinClient(event); break;
    case LE_KART_INFO: handleKartInfo(event); break;
    case LE_CLIENT_BACK_LOBBY: clientInGameWantsToBackLobby(event); break;
    default: Log::error("ServerLobby", "Unknown message of type %d - ignored.",
                        message_type);
             break;
    }   // switch message_type
    return true;
}   // notifyEvent

//-----------------------------------------------------------------------------
void ServerLobby::handleChat(Event* event)
{
    if (!checkDataSize(event, 1) || !ServerConfig::m_chat) return;

    // Update so that the peer is not kicked
    event->getPeer()->updateLastActivity();
    const bool sender_in_game = event->getPeer()->isWaitingForGame();

    int64_t last_message = event->getPeer()->getLastMessage();
    int64_t elapsed_time = (int64_t)StkTime::getMonoTimeMs() - last_message;

    // Read ServerConfig for formula and details
    if (ServerConfig::m_chat_consecutive_interval > 0 &&
        elapsed_time < ServerConfig::m_chat_consecutive_interval * 1000)
        event->getPeer()->updateConsecutiveMessages(true);
    else
        event->getPeer()->updateConsecutiveMessages(false);

    if (ServerConfig::m_chat_consecutive_interval > 0 &&
        event->getPeer()->getConsecutiveMessages() >
        ServerConfig::m_chat_consecutive_interval / 2)
    {
        NetworkString* chat = getNetworkString();
        chat->setSynchronous(true);
        core::stringw warn = "Spam detected";
        chat->addUInt8(LE_CHAT).encodeString16(warn);
        event->getPeer()->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }

    core::stringw message;
    event->data().decodeString16(&message, 360/*max_len*/);

    // Check if the message starts with "(the name of main profile): " to prevent
    // impersonation, see #5121.
    std::string message_utf8 = StringUtils::wideToUtf8(message);
    std::string prefix = StringUtils::wideToUtf8(
        event->getPeer()->getPlayerProfiles()[0]->getName()) + ": ";

    if (!StringUtils::startsWith(message_utf8, prefix))
    {
        NetworkString* chat = getNetworkString();
        chat->setSynchronous(true);
        core::stringw warn = "Don't try to impersonate others!";
        chat->addUInt8(LE_CHAT).encodeString16(warn);
        event->getPeer()->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }

    KartTeam target_team = KART_TEAM_NONE;
    if (event->data().size() > 0)
        target_team = (KartTeam)event->data().getUInt8();

    if (message.size() > 0)
    {
        // Red or blue square emoji
        if (target_team == KART_TEAM_RED)
            message = StringUtils::utf32ToWide({0x1f7e5, 0x20}) + message;
        else if (target_team == KART_TEAM_BLUE)
            message = StringUtils::utf32ToWide({0x1f7e6, 0x20}) + message;

        NetworkString* chat = getNetworkString();
        chat->setSynchronous(true);
        chat->addUInt8(LE_CHAT).encodeString16(message);
        const bool game_started = m_state.load() != WAITING_FOR_START_GAME;
        core::stringw sender_name =
            event->getPeer()->getPlayerProfiles()[0]->getName();
        STKHost::get()->sendPacketToAllPeersWith(
            [game_started, sender_in_game, target_team, sender_name, this]
            (STKPeer* p)
            {
                if (game_started)
                {
                    if (p->isWaitingForGame() && !sender_in_game)
                        return false;
                    if (!p->isWaitingForGame() && sender_in_game)
                        return false;
                    if (target_team != KART_TEAM_NONE)
                    {
                        if (p->isSpectator())
                            return false;
                        for (auto& player : p->getPlayerProfiles())
                        {
                            if (player->getTeam() == target_team)
                                return true;
                        }
                        return false;
                    }
                }
                for (auto& peer : m_peers_muted_players)
                {
                    if (auto peer_sp = peer.first.lock())
                    {
                        if (peer_sp.get() == p &&
                            peer.second.find(sender_name) != peer.second.end())
                            return false;
                    }
                }
                return true;
            }, chat);
            event->getPeer()->updateLastMessage();
        delete chat;
    }
}   // handleChat

//-----------------------------------------------------------------------------
void ServerLobby::changeTeam(Event* event)
{
    if (!ServerConfig::m_team_choosing ||
        !RaceManager::get()->teamEnabled())
        return;
    if (!checkDataSize(event, 1)) return;
    NetworkString& data = event->data();
    uint8_t local_id = data.getUInt8();
    auto& player = event->getPeer()->getPlayerProfiles().at(local_id);
    auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
    // At most 7 players on each team (for live join)
    if (player->getTeam() == KART_TEAM_BLUE)
    {
        if (red_blue.first >= 7)
            return;
        player->setTeam(KART_TEAM_RED);
    }
    else
    {
        if (red_blue.second >= 7)
            return;
        player->setTeam(KART_TEAM_BLUE);
    }
    updatePlayerList();
}   // changeTeam

//-----------------------------------------------------------------------------
void ServerLobby::kickHost(Event* event)
{
    if (m_server_owner.lock() != event->getPeerSP())
        return;
    if (!checkDataSize(event, 4)) return;
    NetworkString& data = event->data();
    uint32_t host_id = data.getUInt32();
    std::shared_ptr<STKPeer> peer = STKHost::get()->findPeerByHostId(host_id);
    // Ignore kicking ai peer if ai handling is on
    if (peer && (!ServerConfig::m_ai_handling || !peer->isAIPeer()))
        peer->kick();
}   // kickHost

//-----------------------------------------------------------------------------
bool ServerLobby::notifyEventAsynchronous(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() == EVENT_TYPE_MESSAGE)
    {
        NetworkString &data = event->data();
        assert(data.size()); // message not empty
        uint8_t message_type;
        message_type = data.getUInt8();
        Log::info("ServerLobby", "Message of type %d received.",
                  message_type);
        switch(message_type)
        {
        case LE_CONNECTION_REQUESTED: connectionRequested(event); break;
        case LE_KART_SELECTION: kartSelectionRequested(event);    break;
        case LE_CLIENT_LOADED_WORLD: finishedLoadingWorldClient(event); break;
        case LE_VOTE: handlePlayerVote(event);                    break;
        case LE_KICK_HOST: kickHost(event);                       break;
        case LE_CHANGE_TEAM: changeTeam(event);                   break;
        case LE_REQUEST_BEGIN: startSelection(event);             break;
        case LE_CHAT: handleChat(event);                          break;
        case LE_CONFIG_SERVER: handleServerConfiguration(event);  break;
        case LE_CHANGE_HANDICAP: changeHandicap(event);           break;
        case LE_CLIENT_BACK_LOBBY:
            clientSelectingAssetsWantsToBackLobby(event);         break;
        case LE_REPORT_PLAYER: writePlayerReport(event);          break;
        case LE_ASSETS_UPDATE:
            handleAssets(event->data(), event->getPeer());        break;
        case LE_COMMAND:
            handleServerCommand(event, event->getPeerSP());       break;
        default:                                                  break;
        }   // switch
    } // if (event->getType() == EVENT_TYPE_MESSAGE)
    else if (event->getType() == EVENT_TYPE_DISCONNECTED)
    {
        clientDisconnected(event);
    } // if (event->getType() == EVENT_TYPE_DISCONNECTED)
    return true;
}   // notifyEventAsynchronous

//-----------------------------------------------------------------------------
#ifdef ENABLE_SQLITE3
/* Every 1 minute STK will poll database:
 * 1. Set disconnected time to now for non-exists host.
 * 2. Clear expired player reports if necessary
 * 3. Kick active peer from ban list
 */
void ServerLobby::pollDatabase()
{
    if (!ServerConfig::m_sql_management || !m_db_connector->hasDatabase())
        return;

    if (!m_db_connector->isTimeToPoll())
        return;

    m_db_connector->updatePollTime();

    std::vector<DatabaseConnector::IpBanTableData> ip_ban_list =
            m_db_connector->getIpBanTableData();
    std::vector<DatabaseConnector::Ipv6BanTableData> ipv6_ban_list =
            m_db_connector->getIpv6BanTableData();
    std::vector<DatabaseConnector::OnlineIdBanTableData> online_id_ban_list =
            m_db_connector->getOnlineIdBanTableData();

    for (std::shared_ptr<STKPeer>& p : STKHost::get()->getPeers())
    {
        if (p->isAIPeer())
            continue;
        bool is_kicked = false;
        std::string address = "";
        std::string reason = "";
        std::string description = "";

        if (p->getAddress().isIPv6())
        {
            address = p->getAddress().toString(false);
            if (address.empty())
                continue;
            for (auto& item: ipv6_ban_list)
            {
                if (insideIPv6CIDR(item.ipv6_cidr.c_str(), address.c_str()) == 1)
                {
                    is_kicked = true;
                    reason = item.reason;
                    description = item.description;
                    break;
                }
            }
        }
        else
        {
            uint32_t peer_addr = p->getAddress().getIP();
            address = p->getAddress().toString();
            for (auto& item: ip_ban_list)
            {
                if (item.ip_start <= peer_addr && item.ip_end >= peer_addr)
                {
                    is_kicked = true;
                    reason = item.reason;
                    description = item.description;
                    break;
                }
            }
        }
        if (!is_kicked && !p->getPlayerProfiles().empty())
        {
            uint32_t online_id = p->getPlayerProfiles()[0]->getOnlineId();
            for (auto& item: online_id_ban_list)
            {
                if (item.online_id == online_id)
                {
                    is_kicked = true;
                    reason = item.reason;
                    description = item.description;
                    break;
                }
            }
        }
        if (is_kicked)
        {
            Log::info("ServerLobby", "Kick %s, reason: %s, description: %s",
                address.c_str(), reason.c_str(), description.c_str());
            p->kick();
        }
    } // for p in peers

    m_db_connector->clearOldReports();

    auto peers = STKHost::get()->getPeers();
    std::vector<uint32_t> hosts;
    if (!peers.empty())
    {
        for (auto& peer : peers)
        {
            if (!peer->isValidated())
                continue;
            hosts.push_back(peer->getHostId());
        }
    }
    m_db_connector->setDisconnectionTimes(hosts);
}   // pollDatabase
#endif
//-----------------------------------------------------------------------------
void ServerLobby::writePlayerReport(Event* event)
{
#ifdef ENABLE_SQLITE3
    if (!m_db_connector->hasDatabase() || !m_db_connector->hasPlayerReportsTable())
        return;
    STKPeer* reporter = event->getPeer();
    if (!reporter->hasPlayerProfiles())
        return;
    auto reporter_npp = reporter->getPlayerProfiles()[0];

    uint32_t reporting_host_id = event->data().getUInt32();
    core::stringw info;
    event->data().decodeString16(&info);
    if (info.empty())
        return;

    auto reporting_peer = STKHost::get()->findPeerByHostId(reporting_host_id);
    if (!reporting_peer || !reporting_peer->hasPlayerProfiles())
        return;
    auto reporting_npp = reporting_peer->getPlayerProfiles()[0];

    bool written = m_db_connector->writeReport(reporter, reporter_npp,
            reporting_peer.get(), reporting_npp, info);
    if (written)
    {
        NetworkString* success = getNetworkString();
        success->setSynchronous(true);
        success->addUInt8(LE_REPORT_PLAYER).addUInt8(1)
            .encodeString(reporting_npp->getName());
        event->getPeer()->sendPacket(success, true/*reliable*/);
        delete success;
    }
#endif
}   // writePlayerReport

//-----------------------------------------------------------------------------
/** Find out the public IP server or poll STK server asynchronously. */
void ServerLobby::asynchronousUpdate()
{
    if (m_rs_state.load() == RS_ASYNC_RESET)
    {
        resetVotingTime();
        resetServer();
        m_rs_state.store(RS_NONE);
    }

    for (auto it = m_peers_muted_players.begin();
        it != m_peers_muted_players.end();)
    {
        if (it->first.expired())
            it = m_peers_muted_players.erase(it);
        else
            it++;
    }

#ifdef ENABLE_SQLITE3
    pollDatabase();
#endif

    // Check if server owner has left
    updateServerOwner();

    if (ServerConfig::m_ranked && m_state.load() == WAITING_FOR_START_GAME)
        m_ranking->cleanup();

    if (allowJoinedPlayersWaiting() || (m_game_setup->isGrandPrix() &&
        m_state.load() == WAITING_FOR_START_GAME))
    {
        // Only poll the STK server if server has been registered.
        if (m_server_id_online.load() != 0 &&
            m_state.load() != REGISTER_SELF_ADDRESS)
            checkIncomingConnectionRequests();
        handlePendingConnection();
    }

    if (m_server_id_online.load() != 0 &&
        allowJoinedPlayersWaiting() &&
        StkTime::getMonoTimeMs() > m_last_unsuccess_poll_time &&
        StkTime::getMonoTimeMs() > m_last_success_poll_time.load() + 30000)
    {
        Log::warn("ServerLobby", "Trying auto server recovery.");
        // For auto server recovery wait 3 seconds for next try
        m_last_unsuccess_poll_time = StkTime::getMonoTimeMs() + 3000;
        registerServer(false/*first_time*/);
    }

    switch (m_state.load())
    {
    case SET_PUBLIC_ADDRESS:
    {
        // In case of LAN we don't need our public address or register with the
        // STK server, so we can directly go to the accepting clients state.
        if (NetworkConfig::get()->isLAN())
        {
            m_state = WAITING_FOR_START_GAME;
            updatePlayerList();
            STKHost::get()->startListening();
            return;
        }
        auto ip_type = NetworkConfig::get()->getIPType();
        // Set the IPv6 address first for possible IPv6 only server
        if (isIPv6Socket() && ip_type >= NetworkConfig::IP_V6)
        {
            STKHost::get()->setPublicAddress(AF_INET6);
        }
        if (ip_type == NetworkConfig::IP_V4 ||
            ip_type == NetworkConfig::IP_DUAL_STACK)
        {
            STKHost::get()->setPublicAddress(AF_INET);
        }
        if (STKHost::get()->getPublicAddress().isUnset() &&
            STKHost::get()->getPublicIPv6Address().empty())
        {
            m_state = ERROR_LEAVE;
        }
        else
        {
            STKHost::get()->startListening();
            m_state = REGISTER_SELF_ADDRESS;
        }
        break;
    }
    case REGISTER_SELF_ADDRESS:
    {
        if (m_game_setup->isGrandPrixStarted() || m_registered_for_once_only)
        {
            m_state = WAITING_FOR_START_GAME;
            updatePlayerList();
            break;
        }
        // Register this server with the STK server. This will block
        // this thread, because there is no need for the protocol manager
        // to react to any requests before the server is registered.
        if (m_server_registering.expired() && m_server_id_online.load() == 0)
            registerServer(true/*first_time*/);

        if (m_server_registering.expired())
        {
            // Finished registering server
            if (m_server_id_online.load() != 0)
            {
                // For non grand prix server we only need to register to stk
                // addons once
                if (allowJoinedPlayersWaiting())
                    m_registered_for_once_only = true;
                m_state = WAITING_FOR_START_GAME;
                updatePlayerList();
            }
        }
        break;
    }
    case WAITING_FOR_START_GAME:
    {
        if (ServerConfig::m_owner_less)
        {
            // Ensure that a game can auto-start if the server meets the config's starting limit or if it's already full.
            int starting_limit = std::min((int)ServerConfig::m_min_start_game_players, (int)ServerConfig::m_server_max_players);
            if (ServerConfig::m_max_players_in_game > 0) // 0 here means it's not the limit
                starting_limit = std::min(starting_limit, (int)ServerConfig::m_max_players_in_game);

            unsigned players = 0;
            STKHost::get()->updatePlayers(&players);
            if (((int)players >= starting_limit ||
                m_game_setup->isGrandPrixStarted()) &&
                m_timeout.load() == std::numeric_limits<int64_t>::max())
            {
                m_timeout.store((int64_t)StkTime::getMonoTimeMs() +
                    (int64_t)
                    (ServerConfig::m_start_game_counter * 1000.0f));
            }
            else if ((int)players < starting_limit &&
                !m_game_setup->isGrandPrixStarted())
            {
                resetPeersReady();
                if (m_timeout.load() != std::numeric_limits<int64_t>::max())
                    updatePlayerList();
                m_timeout.store(std::numeric_limits<int64_t>::max());
            }
            if (m_timeout.load() < (int64_t)StkTime::getMonoTimeMs() ||
                (checkPeersReady(true/*ignore_ai_peer*/) &&
                (int)players >= starting_limit))
            {
                resetPeersReady();
                startSelection();
                return;
            }
        }
        break;
    }
    case ERROR_LEAVE:
    {
        requestTerminate();
        m_state = EXITING;
        STKHost::get()->requestShutdown();
        break;
    }
    case WAIT_FOR_WORLD_LOADED:
    {
        // For WAIT_FOR_WORLD_LOADED and SELECTING make sure there are enough
        // players to start next game, otherwise exiting and let main thread
        // reset
        if (m_end_voting_period.load() == 0)
            return;

        unsigned player_in_game = 0;
        STKHost::get()->updatePlayers(&player_in_game);
        // Reset lobby will be done in main thread
        if ((player_in_game == 1 && ServerConfig::m_ranked) ||
            player_in_game == 0)
        {
            resetVotingTime();
            return;
        }

        // m_server_has_loaded_world is set by main thread with atomic write
        if (m_server_has_loaded_world.load() == false)
            return;
        if (!checkPeersReady(
            ServerConfig::m_ai_handling && m_ai_count == 0/*ignore_ai_peer*/))
            return;
        // Reset for next state usage
        resetPeersReady();
        configPeersStartTime();
        break;
    }
    case SELECTING:
    {
        if (m_end_voting_period.load() == 0)
            return;
        unsigned player_in_game = 0;
        STKHost::get()->updatePlayers(&player_in_game);
        if ((player_in_game == 1 && ServerConfig::m_ranked) ||
            player_in_game == 0)
        {
            resetVotingTime();
            return;
        }

        PeerVote winner_vote;
        m_winner_peer_id = std::numeric_limits<uint32_t>::max();
        bool go_on_race = false;
        if (ServerConfig::m_track_voting)
            go_on_race = handleAllVotes(&winner_vote, &m_winner_peer_id);
        else if (m_game_setup->isGrandPrixStarted() || isVotingOver())
        {
            winner_vote = *m_default_vote;
            go_on_race = true;
        }
        if (go_on_race)
        {
            *m_default_vote = winner_vote;
            m_item_seed = (uint32_t)StkTime::getTimeSinceEpoch();
            ItemManager::updateRandomSeed(m_item_seed);
            m_game_setup->setRace(winner_vote);

            // For spectators that don't have the track, remember their
            // spectate mode and don't load the track
            std::string track_name = winner_vote.m_track_name;
            auto peers = STKHost::get()->getPeers();
            std::map<std::shared_ptr<STKPeer>,
                    AlwaysSpectateMode> previous_spectate_mode;
            for (auto peer : peers)
            {
                if (peer->alwaysSpectate() &&
                    peer->getClientAssets().second.count(track_name) == 0)
                {
                    previous_spectate_mode[peer] = peer->getAlwaysSpectate();
                    peer->setAlwaysSpectate(ASM_NONE);
                    peer->setWaitingForGame(true);
                    m_peers_ready.erase(peer);
                }
            }
            bool has_always_on_spectators = false;
            auto players = STKHost::get()
                ->getPlayersForNewGame(&has_always_on_spectators);
            for (auto& p: previous_spectate_mode)
                if (p.first)
                    p.first->setAlwaysSpectate(p.second);
            auto ai_instance = m_ai_peer.lock();
            if (supportsAI())
            {
                if (ai_instance)
                {
                    auto ai_profiles = ai_instance->getPlayerProfiles();
                    if (m_ai_count > 0)
                    {
                        ai_profiles.resize(m_ai_count);
                        players.insert(players.end(), ai_profiles.begin(),
                            ai_profiles.end());
                    }
                }
                else if (!m_ai_profiles.empty())
                {
                    players.insert(players.end(), m_ai_profiles.begin(),
                        m_ai_profiles.end());
                }
            }
            m_game_setup->sortPlayersForGrandPrix(players);
            m_game_setup->sortPlayersForGame(players);
            for (unsigned i = 0; i < players.size(); i++)
            {
                std::shared_ptr<NetworkPlayerProfile>& player = players[i];
                std::shared_ptr<STKPeer> peer = player->getPeer();
                if (peer)
                    peer->clearAvailableKartIDs();
            }
            for (unsigned i = 0; i < players.size(); i++)
            {
                std::shared_ptr<NetworkPlayerProfile>& player = players[i];
                std::shared_ptr<STKPeer> peer = player->getPeer();
                if (peer)
                    peer->addAvailableKartID(i);
            }
            getHitCaptureLimit();

            // Add placeholder players for live join
            addLiveJoinPlaceholder(players);
            // If player chose random / hasn't chose any kart
            for (unsigned i = 0; i < players.size(); i++)
            {
                if (players[i]->getKartName().empty())
                {
                    RandomGenerator rg;
                    std::set<std::string>::iterator it =
                        m_available_kts.first.begin();
                    std::advance(it,
                        rg.get((int)m_available_kts.first.size()));
                    players[i]->setKartName(*it);
                }
            }

            NetworkString* load_world_message = getLoadWorldMessage(players,
                false/*live_join*/);
            m_game_setup->setHitCaptureTime(m_battle_hit_capture_limit,
                m_battle_time_limit);
            uint16_t flag_return_time = (uint16_t)stk_config->time2Ticks(
                ServerConfig::m_flag_return_timeout);
            RaceManager::get()->setFlagReturnTicks(flag_return_time);
            uint16_t flag_deactivated_time = (uint16_t)stk_config->time2Ticks(
                ServerConfig::m_flag_deactivated_time);
            RaceManager::get()->setFlagDeactivatedTicks(flag_deactivated_time);
            configRemoteKart(players, 0);

            // Reset for next state usage
            resetPeersReady();
            m_state = LOAD_WORLD;
            sendMessageToPeers(load_world_message);
            // updatePlayerList so the in lobby players (if any) can see always
            // spectators join the game
            if (has_always_on_spectators)
                updatePlayerList();
            delete load_world_message;
        }
        break;
    }
    default:
        break;
    }

}   // asynchronousUpdate

//-----------------------------------------------------------------------------
void ServerLobby::encodePlayers(BareNetworkString* bns,
        std::vector<std::shared_ptr<NetworkPlayerProfile> >& players) const
{
    bns->addUInt8((uint8_t)players.size());
    for (unsigned i = 0; i < players.size(); i++)
    {
        std::shared_ptr<NetworkPlayerProfile>& player = players[i];
        bns->encodeString(player->getName())
            .addUInt32(player->getHostId())
            .addFloat(player->getDefaultKartColor())
            .addUInt32(player->getOnlineId())
            .addUInt8(player->getHandicap())
            .addUInt8(player->getLocalPlayerId())
            .addUInt8(
            RaceManager::get()->teamEnabled() ? player->getTeam() : KART_TEAM_NONE)
            .encodeString(player->getCountryCode());
        bns->encodeString(player->getKartName());
    }
}   // encodePlayers

//-----------------------------------------------------------------------------
NetworkString* ServerLobby::getLoadWorldMessage(
    std::vector<std::shared_ptr<NetworkPlayerProfile> >& players,
    bool live_join) const
{
    NetworkString* load_world_message = getNetworkString();
    load_world_message->setSynchronous(true);
    load_world_message->addUInt8(LE_LOAD_WORLD);
    load_world_message->addUInt32(m_winner_peer_id);
    m_default_vote->encode(load_world_message);
    load_world_message->addUInt8(live_join ? 1 : 0);
    encodePlayers(load_world_message, players);
    load_world_message->addUInt32(m_item_seed);
    if (RaceManager::get()->isBattleMode())
    {
        load_world_message->addUInt32(m_battle_hit_capture_limit)
            .addFloat(m_battle_time_limit);
        uint16_t flag_return_time = (uint16_t)stk_config->time2Ticks(
            ServerConfig::m_flag_return_timeout);
        load_world_message->addUInt16(flag_return_time);
        uint16_t flag_deactivated_time = (uint16_t)stk_config->time2Ticks(
            ServerConfig::m_flag_deactivated_time);
        load_world_message->addUInt16(flag_deactivated_time);
    }
    for (unsigned i = 0; i < players.size(); i++)
        players[i]->getKartData().encode(load_world_message);
    return load_world_message;
}   // getLoadWorldMessage

//-----------------------------------------------------------------------------
/** Returns true if server can be live joined or spectating
 */
bool ServerLobby::canLiveJoinNow() const
{
    bool live_join = ServerConfig::m_live_players && worldIsActive();
    if (!live_join)
        return false;
    if (RaceManager::get()->modeHasLaps())
    {
        // No spectate when fastest kart is nearly finish, because if there
        // is endcontroller the spectating remote may not be knowing this
        // on time
        LinearWorld* w = dynamic_cast<LinearWorld*>(World::getWorld());
        if (!w)
            return false;
        AbstractKart* fastest_kart = NULL;
        for (unsigned i = 0; i < w->getNumKarts(); i++)
        {
            fastest_kart = w->getKartAtPosition(i + 1);
            if (fastest_kart && !fastest_kart->isEliminated())
                break;
        }
        if (!fastest_kart)
            return false;
        float progress = w->getOverallDistance(
            fastest_kart->getWorldKartId()) /
            (Track::getCurrentTrack()->getTrackLength() *
            (float)RaceManager::get()->getNumLaps());
        if (progress > 0.9f)
            return false;
    }
    return live_join;
}   // canLiveJoinNow

//-----------------------------------------------------------------------------
/** Returns true if world is active for clients to live join, spectate or
 *  going back to lobby live
 */
bool ServerLobby::worldIsActive() const
{
    return World::getWorld() && RaceEventManager::get()->isRunning() &&
        !RaceEventManager::get()->isRaceOver() &&
        World::getWorld()->getPhase() == WorldStatus::RACE_PHASE;
}   // worldIsActive

//-----------------------------------------------------------------------------
/** \ref STKPeer peer will be reset back to the lobby with reason
 *  \ref BackLobbyReason blr
 */
void ServerLobby::rejectLiveJoin(STKPeer* peer, BackLobbyReason blr)
{
    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(blr);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;
}   // rejectLiveJoin

//-----------------------------------------------------------------------------
/** This message is like kartSelectionRequested, but it will send the peer
 *  load world message if he can join the current started game.
 */
void ServerLobby::liveJoinRequest(Event* event)
{
    STKPeer* peer = event->getPeer();
    const NetworkString& data = event->data();

    if (!canLiveJoinNow())
    {
        rejectLiveJoin(peer, BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    bool spectator = data.getUInt8() == 1;
    if (RaceManager::get()->modeHasLaps() && !spectator)
    {
        // No live join for linear race
        rejectLiveJoin(peer, BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }

    peer->clearAvailableKartIDs();
    if (!spectator)
    {
        auto spectators_by_limit = getSpectatorsByLimit();
        setPlayerKarts(data, peer);

        std::vector<int> used_id;
        for (unsigned i = 0; i < peer->getPlayerProfiles().size(); i++)
        {
            int id = getReservedId(peer->getPlayerProfiles()[i], i);
            if (id == -1)
                break;
            used_id.push_back(id);
        }
        if ((used_id.size() != peer->getPlayerProfiles().size()) ||
            (spectators_by_limit.find(event->getPeerSP()) != spectators_by_limit.end()))
        {
            for (unsigned i = 0; i < peer->getPlayerProfiles().size(); i++)
                peer->getPlayerProfiles()[i]->setKartName("");
            for (unsigned i = 0; i < used_id.size(); i++)
            {
                RemoteKartInfo& rki = RaceManager::get()->getKartInfo(used_id[i]);
                rki.makeReserved();
            }
            Log::info("ServerLobby", "Too many players (%d) try to live join",
                (int)peer->getPlayerProfiles().size());
            rejectLiveJoin(peer, BLR_NO_PLACE_FOR_LIVE_JOIN);
            return;
        }

        for (int id : used_id)
        {
            Log::info("ServerLobby", "%s live joining with reserved kart id %d.",
                peer->getAddress().toString().c_str(), id);
            peer->addAvailableKartID(id);
        }
    }
    else
    {
        Log::info("ServerLobby", "%s spectating now.",
            peer->getAddress().toString().c_str());
    }

    std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
        getLivePlayers();
    NetworkString* load_world_message = getLoadWorldMessage(players,
        true/*live_join*/);
    peer->sendPacket(load_world_message, true/*reliable*/);
    delete load_world_message;
    peer->updateLastActivity();
}   // liveJoinRequest

//-----------------------------------------------------------------------------
/** Get a list of current ingame players for live join or spectate.
 */
std::vector<std::shared_ptr<NetworkPlayerProfile> >
                                            ServerLobby::getLivePlayers() const
{
    std::vector<std::shared_ptr<NetworkPlayerProfile> > players;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        std::shared_ptr<NetworkPlayerProfile> player =
            rki.getNetworkPlayerProfile().lock();
        if (!player)
        {
            if (RaceManager::get()->modeHasLaps())
            {
                player = std::make_shared<NetworkPlayerProfile>(
                    nullptr, rki.getPlayerName(),
                    std::numeric_limits<uint32_t>::max(),
                    rki.getDefaultKartColor(),
                    rki.getOnlineId(), rki.getHandicap(),
                    rki.getLocalPlayerId(), KART_TEAM_NONE,
                    rki.getCountryCode());
                player->setKartName(rki.getKartName());
            }
            else
            {
                player = NetworkPlayerProfile::getReservedProfile(
                    RaceManager::get()->getMinorMode() ==
                    RaceManager::MINOR_MODE_FREE_FOR_ALL ?
                    KART_TEAM_NONE : rki.getKartTeam());
            }
        }
        players.push_back(player);
    }
    return players;
}   // getLivePlayers

//-----------------------------------------------------------------------------
/** Decide where to put the live join player depends on his team and game mode.
 */
int ServerLobby::getReservedId(std::shared_ptr<NetworkPlayerProfile>& p,
                               unsigned local_id) const
{
    const bool is_ffa =
        RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL;
    int red_count = 0;
    int blue_count = 0;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        if (rki.isReserved())
            continue;
        bool disconnected = rki.disconnected();
        if (RaceManager::get()->getKartInfo(i).getKartTeam() == KART_TEAM_RED &&
            !disconnected)
            red_count++;
        else if (RaceManager::get()->getKartInfo(i).getKartTeam() ==
            KART_TEAM_BLUE && !disconnected)
            blue_count++;
    }
    KartTeam target_team = red_count > blue_count ? KART_TEAM_BLUE :
        KART_TEAM_RED;

    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        std::shared_ptr<NetworkPlayerProfile> player =
            rki.getNetworkPlayerProfile().lock();
        if (!player)
        {
            if (is_ffa)
            {
                rki.copyFrom(p, local_id);
                return i;
            }
            if (ServerConfig::m_team_choosing)
            {
                if ((p->getTeam() == KART_TEAM_RED &&
                    rki.getKartTeam() == KART_TEAM_RED) ||
                    (p->getTeam() == KART_TEAM_BLUE &&
                    rki.getKartTeam() == KART_TEAM_BLUE))
                {
                    rki.copyFrom(p, local_id);
                    return i;
                }
            }
            else
            {
                if (rki.getKartTeam() == target_team)
                {
                    p->setTeam(target_team);
                    rki.copyFrom(p, local_id);
                    return i;
                }
            }
        }
    }
    return -1;
}   // getReservedId

//-----------------------------------------------------------------------------
/** Finally put the kart in the world and inform client the current world
 *  status, (including current confirmed item state, kart scores...)
 */
void ServerLobby::finishedLoadingLiveJoinClient(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    if (!canLiveJoinNow())
    {
        rejectLiveJoin(peer.get(), BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    bool live_joined_in_time = true;
    for (const int id : peer->getAvailableKartIDs())
    {
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        if (rki.isReserved())
        {
            live_joined_in_time = false;
            break;
        }
    }
    if (!live_joined_in_time)
    {
        Log::warn("ServerLobby", "%s can't live-join in time.",
            peer->getAddress().toString().c_str());
        rejectLiveJoin(peer.get(), BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    World* w = World::getWorld();
    assert(w);

    uint64_t live_join_start_time = STKHost::get()->getNetworkTimer();

    // Instead of using getTicksSinceStart we caculate the current world ticks
    // only from network timer, because if the server hangs in between the
    // world ticks may not be up to date
    // 2000 is the time for ready set, remove 3 ticks after for minor
    // correction (make it more looks like getTicksSinceStart if server has no
    // hang
    int cur_world_ticks = stk_config->time2Ticks(
        (live_join_start_time - m_server_started_at - 2000) / 1000.f) - 3;
    // Give 3 seconds for all peers to get new kart info
    m_last_live_join_util_ticks =
        cur_world_ticks + stk_config->time2Ticks(3.0f);
    live_join_start_time -= m_server_delay;
    live_join_start_time += 3000;

    bool spectator = false;
    for (const int id : peer->getAvailableKartIDs())
    {
        World::getWorld()->addReservedKart(id);
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        addLiveJoiningKart(id, rki, m_last_live_join_util_ticks);
        Log::info("ServerLobby", "%s succeeded live-joining with kart id %d.",
            peer->getAddress().toString().c_str(), id);
    }
    if (peer->getAvailableKartIDs().empty())
    {
        Log::info("ServerLobby", "%s spectating succeeded.",
            peer->getAddress().toString().c_str());
        spectator = true;
    }

    const uint8_t cc = (uint8_t)Track::getCurrentTrack()->getCheckManager()->getCheckStructureCount();
    NetworkString* ns = getNetworkString(10);
    ns->setSynchronous(true);
    ns->addUInt8(LE_LIVE_JOIN_ACK).addUInt64(m_client_starting_time)
        .addUInt8(cc).addUInt64(live_join_start_time)
        .addUInt32(m_last_live_join_util_ticks);

    NetworkItemManager* nim = dynamic_cast<NetworkItemManager*>
        (Track::getCurrentTrack()->getItemManager());
    assert(nim);
    nim->saveCompleteState(ns);
    nim->addLiveJoinPeer(peer);

    w->saveCompleteState(ns, peer.get());
    if (RaceManager::get()->supportsLiveJoining())
    {
        // Only needed in non-racing mode as no need players can added after
        // starting of race
        std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
            getLivePlayers();
        encodePlayers(ns, players);
        for (unsigned i = 0; i < players.size(); i++)
            players[i]->getKartData().encode(ns);
    }

    m_peers_ready[peer] = false;
    peer->setWaitingForGame(false);
    peer->setSpectator(spectator);

    peer->sendPacket(ns, true/*reliable*/);
    delete ns;
    updatePlayerList();
    peer->updateLastActivity();

    if ((!spectator) && (m_state.load() == RACING) && (powerup_multiplier_value() == 3) && (!losing_team_weaker()))
        {
            set_powerup_multiplier(1);
            send_message("Powerupper is OFF (automatically)");
        }
    else if ((!spectator) && (m_state.load() == RACING) && (powerup_multiplier_value() == 1) && (losing_team_weaker()) && (abs(getSoccerScoreDifference()) > 3))
        {
            set_powerup_multiplier(3);
            send_message("Powerupper is ON (automatically)");
        }

}   // finishedLoadingLiveJoinClient

//-----------------------------------------------------------------------------
/** Simple finite state machine.  Once this
 *  is known, register the server and its address with the stk server so that
 *  client can find it.
 */
void ServerLobby::update(int ticks)
{
    World* w = World::getWorld();
    bool world_started = m_state.load() >= WAIT_FOR_WORLD_LOADED &&
        m_state.load() <= RACING && m_server_has_loaded_world.load();
    bool all_players_in_world_disconnected = (w != NULL && world_started);
    int sec = ServerConfig::m_kick_idle_player_seconds;
    if (world_started)
    {
        for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
        {
            RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
            std::shared_ptr<NetworkPlayerProfile> player =
                rki.getNetworkPlayerProfile().lock();
            if (player)
            {
                if (w)
                    all_players_in_world_disconnected = false;
            }
            else
                continue;
            auto peer = player->getPeer();
            if (!peer)
                continue;

            if (peer->idleForSeconds() > 60 && w &&
                w->getKart(i)->isEliminated())
            {
                // Remove loading world too long (60 seconds) live join peer
                Log::info("ServerLobby", "%s hasn't live-joined within"
                    " 60 seconds, remove it.",
                    peer->getAddress().toString().c_str());
                rki.makeReserved();
                continue;
            }
            if (!peer->isAIPeer() &&
                sec > 0 && peer->idleForSeconds() > sec &&
                !peer->isDisconnected() && NetworkConfig::get()->isWAN())
            {
                if (w && w->getKart(i)->hasFinishedRace())
                    continue;
                // Don't kick in game GUI server host so he can idle in game
                if (m_process_type == PT_CHILD &&
                    peer->getHostId() == m_client_server_host_id.load())
                    continue;
                Log::info("ServerLobby", "%s %s has been idle for more than"
                    " %d seconds, kick.",
                    peer->getAddress().toString().c_str(),
                    StringUtils::wideToUtf8(rki.getPlayerName()).c_str(), sec);
                peer->kick();
            }
        }
    }
    if (w)
        setGameStartedProgress(w->getGameStartedProgress());
    else
        resetGameStartedProgress();

    if (w && w->getPhase() == World::RACE_PHASE)
    {
        storePlayingTrack(RaceManager::get()->getTrackName());
    }
    else
        storePlayingTrack("");

    // Reset server to initial state if no more connected players
    if (m_rs_state.load() == RS_WAITING)
    {
        if ((RaceEventManager::get() &&
            !RaceEventManager::get()->protocolStopped()) ||
            !GameProtocol::emptyInstance())
            return;

        exitGameState();
        m_rs_state.store(RS_ASYNC_RESET);
    }

    STKHost::get()->updatePlayers();
    if (m_rs_state.load() == RS_NONE &&
        (m_state.load() > WAITING_FOR_START_GAME ||
        m_game_setup->isGrandPrixStarted()) &&
        (STKHost::get()->getPlayersInGame() == 0 ||
        all_players_in_world_disconnected))
    {
        if (RaceEventManager::get() &&
            RaceEventManager::get()->isRunning())
        {
            // Send a notification to all players who may have start live join
            // or spectate to go back to lobby
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;

            RaceEventManager::get()->stop();
            RaceEventManager::get()->getProtocol()->requestTerminate();
            GameProtocol::lock()->requestTerminate();
        }
        else if (auto ai = m_ai_peer.lock())
        {
            // Reset AI peer for empty server, which will delete world
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            ai->sendPacket(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;
        }

        resetVotingTime();
        m_game_setup->stopGrandPrix();
        m_rs_state.store(RS_WAITING);
        return;
    }

    if (m_rs_state.load() != RS_NONE)
        return;

    // Reset for ranked server if in kart / track selection has only 1 player
    if (ServerConfig::m_ranked &&
        m_state.load() == SELECTING &&
        STKHost::get()->getPlayersInGame() == 1)
    {
        NetworkString* back_lobby = getNetworkString(2);
        back_lobby->setSynchronous(true);
        back_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_ONE_PLAYER_IN_RANKED_MATCH);
        sendMessageToPeers(back_lobby, /*reliable*/true);
        delete back_lobby;
        resetVotingTime();
        m_game_setup->stopGrandPrix();
        m_rs_state.store(RS_ASYNC_RESET);
    }

    handlePlayerDisconnection();

    switch (m_state.load())
    {
    case SET_PUBLIC_ADDRESS:
    case REGISTER_SELF_ADDRESS:
    case WAITING_FOR_START_GAME:
    case WAIT_FOR_WORLD_LOADED:
    case WAIT_FOR_RACE_STARTED:
    {
        // Waiting for asynchronousUpdate
        break;
    }
    case SELECTING:
        // The function playerTrackVote will trigger the next state
        // once all track votes have been received.
        break;
    case LOAD_WORLD:
        Log::info("ServerLobbyRoom", "Starting the race loading.");
        // This will create the world instance, i.e. load track and karts
        loadWorld();
        m_state = WAIT_FOR_WORLD_LOADED;
        break;
    case RACING:
        if (World::getWorld() && RaceEventManager::get() &&
            RaceEventManager::get()->isRunning())
        {
            checkRaceFinished();
        }
        break;
    case WAIT_FOR_RACE_STOPPED:
        if (!RaceEventManager::get()->protocolStopped() ||
            !GameProtocol::emptyInstance())
            return;

        // This will go back to lobby in server (and exit the current race)
        exitGameState();
        // Reset for next state usage
        resetPeersReady();
        // Set the delay before the server forces all clients to exit the race
        // result screen and go back to the lobby
        m_timeout.store((int64_t)StkTime::getMonoTimeMs() + 15000);
        m_state = RESULT_DISPLAY;
        sendMessageToPeers(m_result_ns, /*reliable*/ true);
        Log::info("ServerLobby", "End of game message sent");
        break;
    case RESULT_DISPLAY:
        if (checkPeersReady(true/*ignore_ai_peer*/) ||
            (int64_t)StkTime::getMonoTimeMs() > m_timeout.load())
        {
            // Send a notification to all clients to exit
            // the race result screen
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;
            m_rs_state.store(RS_ASYNC_RESET);
        }
        break;
    case ERROR_LEAVE:
    case EXITING:
        break;
    }
}   // update

//-----------------------------------------------------------------------------
/** Register this server (i.e. its public address) with the STK server
 *  so that clients can find it. It blocks till a response from the
 *  stk server is received (this function is executed from the 
 *  ProtocolManager thread). The information about this client is added
 *  to the table 'server'.
 */
void ServerLobby::registerServer(bool first_time)
{
    // ========================================================================
    class RegisterServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
        bool m_first_time;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string rec_success;
            auto sl = m_server_lobby.lock();
            if (!sl)
                return;

            if (result->get("success", &rec_success) &&
                rec_success == "yes")
            {
                const XMLNode* server = result->getNode("server");
                assert(server);
                const XMLNode* server_info = server->getNode("server-info");
                assert(server_info);
                unsigned server_id_online = 0;
                server_info->get("id", &server_id_online);
                assert(server_id_online != 0);
                bool is_official = false;
                server_info->get("official", &is_official);
                if (!is_official && ServerConfig::m_ranked)
                {
                    Log::fatal("ServerLobby", "You don't have permission to "
                        "host a ranked server.");
                }
                Log::info("ServerLobby",
                    "Server %d is now online.", server_id_online);
                sl->m_server_id_online.store(server_id_online);
                sl->m_last_success_poll_time.store(StkTime::getMonoTimeMs());
                return;
            }
            Log::error("ServerLobby", "%s",
                StringUtils::wideToUtf8(getInfo()).c_str());
            // Exit now if failed to register to stk addons for first time
            if (m_first_time)
                sl->m_state.store(ERROR_LEAVE);
        }
    public:
        RegisterServerRequest(std::shared_ptr<ServerLobby> sl, bool first_time)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl), m_first_time(first_time) {}
    };   // RegisterServerRequest

    auto request = std::make_shared<RegisterServerRequest>(
        std::dynamic_pointer_cast<ServerLobby>(shared_from_this()), first_time);
    NetworkConfig::get()->setServerDetails(request, "create");
    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address",      addr.getIP()        );
    request->addParameter("port",         addr.getPort()      );
    request->addParameter("private_port",
                                    STKHost::get()->getPrivatePort()      );
    request->addParameter("name", m_game_setup->getServerNameUtf8());
    request->addParameter("max_players", ServerConfig::m_server_max_players);
    int difficulty = m_difficulty.load();
    request->addParameter("difficulty", difficulty);
    int game_mode = m_game_mode.load();
    request->addParameter("game_mode", game_mode);
    const std::string& pw = ServerConfig::m_private_server_password;
    request->addParameter("password", (unsigned)(!pw.empty()));
    request->addParameter("version", (unsigned)ServerConfig::m_server_version);

    bool ipv6_only = addr.isUnset();
    if (!ipv6_only)
    {
        Log::info("ServerLobby", "Public IPv4 server address %s",
            addr.toString().c_str());
    }
    if (!STKHost::get()->getPublicIPv6Address().empty())
    {
        request->addParameter("address_ipv6",
            STKHost::get()->getPublicIPv6Address());
        Log::info("ServerLobby", "Public IPv6 server address %s",
            STKHost::get()->getPublicIPv6Address().c_str());
    }
    request->queue();
    m_server_registering = request;
}   // registerServer

//-----------------------------------------------------------------------------
/** Unregister this server (i.e. its public address) with the STK server,
 *  currently when karts enter kart selection screen it will be done or quit
 *  stk.
 */
void ServerLobby::unregisterServer(bool now, std::weak_ptr<ServerLobby> sl)
{
    // ========================================================================
    class UnRegisterServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string rec_success;

            if (result->get("success", &rec_success) &&
                rec_success == "yes")
            {
                // Clear the server online for next register
                // For grand prix server
                if (auto sl = m_server_lobby.lock())
                    sl->m_server_id_online.store(0);
                return;
            }
            Log::error("ServerLobby", "%s",
                StringUtils::wideToUtf8(getInfo()).c_str());
        }
    public:
        UnRegisterServerRequest(std::weak_ptr<ServerLobby> sl)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl) {}
    };   // UnRegisterServerRequest
    auto request = std::make_shared<UnRegisterServerRequest>(sl);
    NetworkConfig::get()->setServerDetails(request, "stop");

    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address", addr.getIP());
    request->addParameter("port", addr.getPort());
    bool ipv6_only = addr.isUnset();
    if (!ipv6_only)
    {
        Log::info("ServerLobby", "Unregister server address %s",
            addr.toString().c_str());
    }
    else
    {
        Log::info("ServerLobby", "Unregister server address %s",
            STKHost::get()->getValidPublicAddress().c_str());
    }

    // No need to check for result as server will be auto-cleared anyway
    // when no polling is done
    if (now)
    {
        request->executeNow();
    }
    else
        request->queue();

}   // unregisterServer

//-----------------------------------------------------------------------------
/** Instructs all clients to start the kart selection. If event is NULL,
 *  the command comes from the owner less server.
 */
void ServerLobby::startSelection(const Event *event)
{
    if (event != NULL)
    {
        if (m_state != WAITING_FOR_START_GAME)
        {
            Log::warn("ServerLobby",
                "Received startSelection while being in state %d.",
                m_state.load());
            return;
        }
        if (ServerConfig::m_owner_less)
        {
            m_peers_ready.at(event->getPeerSP()) =
                !m_peers_ready.at(event->getPeerSP());
            updatePlayerList();
            return;
        }
        if (event->getPeerSP() != m_server_owner.lock())
        {
            Log::warn("ServerLobby",
                "Client %d is not authorised to start selection.",
                event->getPeer()->getHostId());
            return;
        }
    }


    if (!ServerConfig::m_owner_less && ServerConfig::m_team_choosing &&
        RaceManager::get()->teamEnabled())
    {
        auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
        if ((red_blue.first == 0 || red_blue.second == 0) &&
            red_blue.first + red_blue.second != 1)
        {
            Log::warn("ServerLobby", "Bad team choosing.");
            if (event)
            {
                NetworkString* bt = getNetworkString();
                bt->setSynchronous(true);
                bt->addUInt8(LE_BAD_TEAM);
                event->getPeer()->sendPacket(bt, true/*reliable*/);
                delete bt;
            }
            return;
        }
    }

    // Find if there are peers playing the game
    auto peers = STKHost::get()->getPeers();
    std::set<STKPeer*> always_spectate_peers;
    bool has_peer_plays_game = false;
    for (auto peer : peers)
    {
        if (!peer->isValidated() || peer->isWaitingForGame())
            continue;
        if (peer->alwaysSpectate())
            always_spectate_peers.insert(peer.get());
        else if (!peer->isAIPeer())
            has_peer_plays_game = true;
    }

    // Disable always spectate peers if no players join the game
    if (!has_peer_plays_game)
    {
        return;
        //for (STKPeer* peer : always_spectate_peers)
       //     peer->setAlwaysSpectate(ASM_NONE);
       // always_spectate_peers.clear();
    }
    else
    {
        // We make those always spectate peer waiting for game so it won't
        // be able to vote, this will be reset in STKHost::getPlayersForNewGame
        // This will also allow a correct number of in game players for max
        // arena players handling
        for (STKPeer* peer : always_spectate_peers)
            peer->setWaitingForGame(true);
    }

    unsigned max_player = 0;
    STKHost::get()->updatePlayers(&max_player);

    // Set late coming player to spectate if too many players
    auto spectators_by_limit = getSpectatorsByLimit();
    if (spectators_by_limit.size() == peers.size())
    {
        Log::error("ServerLobby", "Too many players and cannot set "
            "spectate for late coming players!");
        return;
    }
    for (auto &peer : spectators_by_limit)
    {
        peer->setAlwaysSpectate(ASM_FULL);
        peer->setWaitingForGame(true);
        always_spectate_peers.insert(peer.get());
    }

    // Remove karts / tracks from server that are not supported
    // on all clients that are playing
    std::set<std::string> karts_erase, tracks_erase;
    for (auto peer : peers)
    {
        // Spectators won't remove maps as they are already waiting for game
        if (!peer->isValidated() || peer->isWaitingForGame())
            continue;
        peer->eraseServerKarts(m_available_kts.first, karts_erase);
        peer->eraseServerTracks(m_available_kts.second, tracks_erase);
    }

    for (const std::string& kart_erase : karts_erase)
    {
        m_available_kts.first.erase(kart_erase);
    }
    for (const std::string& track_erase : tracks_erase)
    {
        m_available_kts.second.erase(track_erase);
    }

    max_player = 0;
    STKHost::get()->updatePlayers(&max_player);
    if (auto ai = m_ai_peer.lock())
    {
        if (supportsAI())
        {
            unsigned total_ai_available =
                (unsigned)ai->getPlayerProfiles().size();
            m_ai_count = max_player > total_ai_available ?
                0 : total_ai_available - max_player + 1;
            // Disable ai peer for this game
            if (m_ai_count == 0)
                ai->setValidated(false);
            else
                ai->setValidated(true);
        }
        else
        {
            ai->setValidated(false);
            m_ai_count = 0;
        }
    }
    else
        m_ai_count = 0;

    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        auto it = m_available_kts.second.begin();
        while (it != m_available_kts.second.end())
        {
            Track* t =  track_manager->getTrack(*it);
            if (t->getMaxArenaPlayers() < max_player)
            {
                it = m_available_kts.second.erase(it);
            }
            else
                it++;
        }
    }

    if (m_available_kts.second.empty())
    {
        Log::error("ServerLobby", "No tracks for playing!");
        return;
    }

    RandomGenerator rg;
    std::set<std::string>::iterator it = m_available_kts.second.begin();
    std::advance(it, rg.get((int)m_available_kts.second.size()));
    m_default_vote->m_track_name = *it;
    switch (RaceManager::get()->getMinorMode())
    {
        case RaceManager::MINOR_MODE_NORMAL_RACE:
        case RaceManager::MINOR_MODE_TIME_TRIAL:
        case RaceManager::MINOR_MODE_FOLLOW_LEADER:
        {
            Track* t = track_manager->getTrack(*it);
            assert(t);
            m_default_vote->m_num_laps = t->getDefaultNumberOfLaps();
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                m_default_vote->m_num_laps =
                    (uint8_t)(fmaxf(1.0f, (float)t->getDefaultNumberOfLaps() *
                    ServerConfig::m_auto_game_time_ratio));
            }
            else if (m_fixed_laps != -1)
                m_default_vote->m_num_laps = m_fixed_laps;
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        case RaceManager::MINOR_MODE_FREE_FOR_ALL:
        {
            m_default_vote->m_num_laps = 0;
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        case RaceManager::MINOR_MODE_CAPTURE_THE_FLAG:
        {
            m_default_vote->m_num_laps = 0;
            m_default_vote->m_reverse = 0;
            break;
        }
        case RaceManager::MINOR_MODE_SOCCER:
        {
            if (m_game_setup->isSoccerGoalTarget())
            {
                m_default_vote->m_num_laps =
                    (uint8_t)(UserConfigParams::m_num_goals);
                if (m_default_vote->m_num_laps > 10)
                    m_default_vote->m_num_laps = (uint8_t)5;
            }
            else
            {
                m_default_vote->m_num_laps =
                    (uint8_t)(UserConfigParams::m_soccer_time_limit);
                if (m_default_vote->m_num_laps > 15)
                    m_default_vote->m_num_laps = (uint8_t)7;
            }
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        default:
            assert(false);
            break;
    }

    if (!allowJoinedPlayersWaiting())
    {
        ProtocolManager::lock()->findAndTerminate(PROTOCOL_CONNECTION);
        if (m_server_id_online.load() != 0)
        {
            unregisterServer(false/*now*/,
                std::dynamic_pointer_cast<ServerLobby>(shared_from_this()));
        }
    }

    startVotingPeriod(ServerConfig::m_voting_timeout);
    NetworkString *ns = getNetworkString(1);
    // Start selection - must be synchronous since the receiver pushes
    // a new screen, which must be done from the main thread.
    ns->setSynchronous(true);
    ns->addUInt8(LE_START_SELECTION)
       .addFloat(ServerConfig::m_voting_timeout)
       .addUInt8(m_game_setup->isGrandPrixStarted() ? 1 : 0)
       .addUInt8((ServerConfig::m_auto_game_time_ratio > 0.0f ||
        m_fixed_laps != -1) ? 1 : 0)
       .addUInt8(ServerConfig::m_track_voting ? 1 : 0);

    const auto& all_k = m_available_kts.first;
    const auto& all_t = m_available_kts.second;
    ns->addUInt16((uint16_t)all_k.size()).addUInt16((uint16_t)all_t.size());
    for (const std::string& kart : all_k)
    {
        ns->encodeString(kart);
    }
    for (const std::string& track : all_t)
    {
        ns->encodeString(track);
    }

    sendMessageToPeers(ns, /*reliable*/true);
    delete ns;

    m_state = SELECTING;
    if (!always_spectate_peers.empty())
    {
        NetworkString* back_lobby = getNetworkString(2);
        back_lobby->setSynchronous(true);
        back_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_SPECTATING_NEXT_GAME);
        STKHost::get()->sendPacketToAllPeersWith(
            [always_spectate_peers](STKPeer* peer) {
            return always_spectate_peers.find(peer) !=
            always_spectate_peers.end(); }, back_lobby, /*reliable*/true);
        delete back_lobby;
        updatePlayerList();
    }

    if (!allowJoinedPlayersWaiting())
    {
        // Drop all pending players and keys if doesn't allow joinning-waiting
        for (auto& p : m_pending_connection)
        {
            if (auto peer = p.first.lock())
                peer->disconnect();
        }
        m_pending_connection.clear();
        std::unique_lock<std::mutex> ul(m_keys_mutex);
        m_keys.clear();
        ul.unlock();
    }

    // Will be changed after the first vote received
    m_timeout.store(std::numeric_limits<int64_t>::max());
}   // startSelection

//-----------------------------------------------------------------------------
/** Query the STK server for connection requests. For each connection request
 *  start a ConnectToPeer protocol.
 */
void ServerLobby::checkIncomingConnectionRequests()
{
    // First poll every 5 seconds. Return if no polling needs to be done.
    const uint64_t POLL_INTERVAL = 5000;
    static uint64_t last_poll_time = 0;
    if (StkTime::getMonoTimeMs() < last_poll_time + POLL_INTERVAL ||
        StkTime::getMonoTimeMs() > m_last_success_poll_time.load() + 30000)
        return;

    // Keep the port open, it can be sent to anywhere as we will send to the
    // correct peer later in ConnectToPeer.
    if (ServerConfig::m_firewalled_server)
    {
        BareNetworkString data;
        data.addUInt8(0);
        const SocketAddress* stun_v4 = STKHost::get()->getStunIPv4Address();
        const SocketAddress* stun_v6 = STKHost::get()->getStunIPv6Address();
        if (stun_v4)
            STKHost::get()->sendRawPacket(data, *stun_v4);
        if (stun_v6)
            STKHost::get()->sendRawPacket(data, *stun_v6);
    }

    // Now poll the stk server
    last_poll_time = StkTime::getMonoTimeMs();

    // ========================================================================
    class PollServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
        std::weak_ptr<ProtocolManager> m_protocol_manager;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string success;

            if (!result->get("success", &success) || success != "yes")
            {
                Log::error("ServerLobby", "Poll server request failed: %s",
                    StringUtils::wideToUtf8(getInfo()).c_str());
                return;
            }

            // Now start a ConnectToPeer protocol for each connection request
            const XMLNode * users_xml = result->getNode("users");
            std::map<uint32_t, KeyData> keys;
            auto sl = m_server_lobby.lock();
            if (!sl)
                return;
            sl->m_last_success_poll_time.store(StkTime::getMonoTimeMs());
            if (sl->m_state.load() != WAITING_FOR_START_GAME &&
                !sl->allowJoinedPlayersWaiting())
            {
                sl->replaceKeys(keys);
                return;
            }

            sl->removeExpiredPeerConnection();
            for (unsigned int i = 0; i < users_xml->getNumNodes(); i++)
            {
                uint32_t addr, id;
                uint16_t port;
                std::string ipv6;
                users_xml->getNode(i)->get("ip", &addr);
                users_xml->getNode(i)->get("ipv6", &ipv6);
                users_xml->getNode(i)->get("port", &port);
                users_xml->getNode(i)->get("id", &id);
                users_xml->getNode(i)->get("aes-key", &keys[id].m_aes_key);
                users_xml->getNode(i)->get("aes-iv", &keys[id].m_aes_iv);
                users_xml->getNode(i)->get("username", &keys[id].m_name);
                users_xml->getNode(i)->get("country-code",
                    &keys[id].m_country_code);
                keys[id].m_tried = false;
                if (ServerConfig::m_firewalled_server)
                {
                    SocketAddress peer_addr(addr, port);
                    if (!ipv6.empty())
                        peer_addr.init(ipv6, port);
                    peer_addr.convertForIPv6Socket(isIPv6Socket());
                    std::string peer_addr_str = peer_addr.toString();
                    if (sl->m_pending_peer_connection.find(peer_addr_str) !=
                        sl->m_pending_peer_connection.end())
                    {
                        continue;
                    }
                    auto ctp = std::make_shared<ConnectToPeer>(peer_addr);
                    if (auto pm = m_protocol_manager.lock())
                        pm->requestStart(ctp);
                    sl->addPeerConnection(peer_addr_str);
                }
            }
            sl->replaceKeys(keys);
        }
    public:
        PollServerRequest(std::shared_ptr<ServerLobby> sl,
                          std::shared_ptr<ProtocolManager> pm)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl), m_protocol_manager(pm)
        {
            m_disable_sending_log = true;
        }
    };   // PollServerRequest
    // ========================================================================

    auto request = std::make_shared<PollServerRequest>(
        std::dynamic_pointer_cast<ServerLobby>(shared_from_this()),
        ProtocolManager::lock());
    NetworkConfig::get()->setServerDetails(request,
        "poll-connection-requests");
    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address", addr.getIP()  );
    request->addParameter("port",    addr.getPort());
    request->addParameter("current-players", getLobbyPlayers());
    request->addParameter("current-ai", m_current_ai_count.load());
    request->addParameter("game-started",
        m_state.load() == WAITING_FOR_START_GAME ? 0 : 1);
    std::string current_track = getPlayingTrackIdent();
    if (!current_track.empty())
        request->addParameter("current-track", getPlayingTrackIdent());
    request->queue();

}   // checkIncomingConnectionRequests

//-----------------------------------------------------------------------------
/** Checks if the race is finished, and if so informs the clients and switches
 *  to state RESULT_DISPLAY, during which the race result gui is shown and all
 *  clients can click on 'continue'.
 */
void ServerLobby::checkRaceFinished(bool endnow)
{
    assert(RaceEventManager::get()->isRunning());
    assert(World::getWorld());
    if(!endnow)
        if (!RaceEventManager::get()->isRaceOver()) return;

    Log::info("ServerLobby", "The game is considered finished.");
    // notify the network world that it is stopped
    RaceEventManager::get()->stop();

    set_powerup_multiplier(1); // turn off powerupper mode

    // stop race protocols before going back to lobby (end race)
    RaceEventManager::get()->getProtocol()->requestTerminate();
    GameProtocol::lock()->requestTerminate();

    // Save race result before delete the world
    m_result_ns->clear();
    m_result_ns->addUInt8(LE_RACE_FINISHED);
    if (m_game_setup->isGrandPrix())
    {
        // fastest lap
        int fastest_lap =
            static_cast<LinearWorld*>(World::getWorld())->getFastestLapTicks();
        m_result_ns->addUInt32(fastest_lap);
        m_result_ns->encodeString(static_cast<LinearWorld*>(World::getWorld())
            ->getFastestLapKartName());

        // all gp tracks
        m_result_ns->addUInt8((uint8_t)m_game_setup->getTotalGrandPrixTracks())
            .addUInt8((uint8_t)m_game_setup->getAllTracks().size());
        for (const std::string& gp_track : m_game_setup->getAllTracks())
            m_result_ns->encodeString(gp_track);

        // each kart score and total time
        m_result_ns->addUInt8((uint8_t)RaceManager::get()->getNumPlayers());
        for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
        {
            int last_score = RaceManager::get()->getKartScore(i);
            int cur_score = last_score;
            float overall_time = RaceManager::get()->getOverallTime(i);
            if (auto player =
                RaceManager::get()->getKartInfo(i).getNetworkPlayerProfile().lock())
            {
                last_score = player->getScore();
                cur_score += last_score;
                overall_time = overall_time + player->getOverallTime();
                player->setScore(cur_score);
                player->setOverallTime(overall_time);
            }
            m_result_ns->addUInt32(last_score).addUInt32(cur_score)
                .addFloat(overall_time);            
        }
    }
    else if (RaceManager::get()->modeHasLaps())
    {
        int fastest_lap =
            static_cast<LinearWorld*>(World::getWorld())->getFastestLapTicks();
        m_result_ns->addUInt32(fastest_lap);
        m_result_ns->encodeString(static_cast<LinearWorld*>(World::getWorld())
            ->getFastestLapKartName());
    }

    uint8_t ranking_changes_indication = 0;
    if (ServerConfig::m_ranked && RaceManager::get()->modeHasLaps())
        ranking_changes_indication = 1;
    m_result_ns->addUInt8(ranking_changes_indication);

    if (ServerConfig::m_ranked)
    {
        computeNewRankings();
        submitRankingsToAddons();
    }
    m_state.store(WAIT_FOR_RACE_STOPPED);

}   // checkRaceFinished

//-----------------------------------------------------------------------------
/** Compute the new player's rankings used in ranked servers
 */
void ServerLobby::computeNewRankings()
{
    
    // No ranking for battle mode
    if (!RaceManager::get()->modeHasLaps())
        return;

    
    World* w = World::getWorld();
    assert(w);

    unsigned player_count = RaceManager::get()->getNumPlayers();
    

    // If all players quitted the race, we assume something went wrong
    // and skip entirely rating and statistics updates.
    for (unsigned i = 0; i < player_count; i++)
    {
        if (!w->getKart(i)->isEliminated())
            break;
        if ((i + 1) == player_count)
            return;
    }

        // Fill the results for the rankings to process
    std::vector<RaceResultData> data;
    for (unsigned i = 0; i < player_count; i++)
    {
                RaceResultData entry;
        entry.online_id = RaceManager::get()->getKartInfo(i).getOnlineId();
        entry.is_eliminated = w->getKart(i)->isEliminated();
        entry.time = RaceManager::get()->getKartRaceTime(i);
        entry.handicap = w->getKart(i)->getHandicap();
        data.push_back(entry);
    }


    for (int i = 0; i < 64; ++i) {
        m_ranking->computeNewRankings(data, RaceManager::get()->isTimeTrialMode());
    }

    // Used to display rating change at the end of a race
    m_result_ns->addUInt8((uint8_t)player_count);
    for (unsigned i = 0; i < player_count; i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        double change =m_ranking->getDelta(id);
        m_result_ns->addFloat((float)change);
    }
}   // computeNewRankings

//-----------------------------------------------------------------------------
/** Called when a client disconnects.
 *  \param event The disconnect event.
 */
void ServerLobby::clientDisconnected(Event* event)
{
    auto players_on_peer = event->getPeer()->getPlayerProfiles();
    if (players_on_peer.empty())
        return;

    NetworkString* msg = getNetworkString(2);
    const bool waiting_peer_disconnected =
        event->getPeer()->isWaitingForGame();
    msg->setSynchronous(true);
    msg->addUInt8(LE_PLAYER_DISCONNECTED);
    msg->addUInt8((uint8_t)players_on_peer.size())
        .addUInt32(event->getPeer()->getHostId());
    for (auto p : players_on_peer)
    {
        std::string name = StringUtils::wideToUtf8(p->getName());
        msg->encodeString(name);
        Log::info("ServerLobby", "%s disconnected", name.c_str());

        if ((m_state.load() == RACING) && (powerup_multiplier_value() == 3) && (!losing_team_weaker()))
        {
            set_powerup_multiplier(1);
            send_message("Powerupper is OFF (automatically)");
        }
    else if ((m_state.load() == RACING) && (powerup_multiplier_value() == 1) && (losing_team_weaker()) && (abs(getSoccerScoreDifference()) > 3))
        {
            set_powerup_multiplier(3);
            send_message("Powerupper is ON (automatically)");
        }


    }

    // Don't show waiting peer disconnect message to in game player
    STKHost::get()->sendPacketToAllPeersWith([waiting_peer_disconnected]
        (STKPeer* p)
        {
            if (!p->isValidated())
                return false;
            if (!p->isWaitingForGame() && waiting_peer_disconnected)
                return false;
            return true;
        }, msg);
    updatePlayerList();
    delete msg;

#ifdef ENABLE_SQLITE3
    m_db_connector->writeDisconnectInfoTable(event->getPeer());
#endif
}   // clientDisconnected

//-----------------------------------------------------------------------------
void ServerLobby::kickPlayerWithReason(STKPeer* peer, const char* reason) const
{
    NetworkString *message = getNetworkString(2);
    message->setSynchronous(true);
    message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_BANNED);
    message->encodeString(std::string(reason));
    peer->cleanPlayerProfiles();
    peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
    peer->reset();
    delete message;
}   // kickPlayerWithReason

//-----------------------------------------------------------------------------
void ServerLobby::saveIPBanTable(const SocketAddress& addr)
{
#ifdef ENABLE_SQLITE3
    m_db_connector->saveAddressToIpBanTable(addr);
#endif
}   // saveIPBanTable

//-----------------------------------------------------------------------------
bool ServerLobby::handleAssets(const NetworkString& ns, STKPeer* peer)
{
    std::set<std::string> client_karts, client_tracks;
    const unsigned kart_num = ns.getUInt16();
    const unsigned track_num = ns.getUInt16();
    for (unsigned i = 0; i < kart_num; i++)
    {
        std::string kart;
        ns.decodeString(&kart);
        client_karts.insert(kart);
    }
    for (unsigned i = 0; i < track_num; i++)
    {
        std::string track;
        ns.decodeString(&track);
        client_tracks.insert(track);
    }

    // Drop this player if he doesn't have at least 1 kart / track the same
    // as server
    float okt = 0.0f;
    float ott = 0.0f;
    for (auto& client_kart : client_karts)
    {
        if (m_official_kts.first.find(client_kart) !=
            m_official_kts.first.end())
            okt += 1.0f;
    }
    okt = okt / (float)m_official_kts.first.size();
    for (auto& client_track : client_tracks)
    {
        if (m_official_kts.second.find(client_track) !=
            m_official_kts.second.end())
            ott += 1.0f;
    }
    ott = ott / (float)m_official_kts.second.size();

    std::set<std::string> karts_erase, tracks_erase;
    for (const std::string& server_kart : m_available_kts.first)
    {
        if (client_karts.find(server_kart) == client_karts.end())
        {
            karts_erase.insert(server_kart);
        }
    }
    for (const std::string& server_track : m_available_kts.second)
    {
        if (client_tracks.find(server_track) == client_tracks.end())
        {
            tracks_erase.insert(server_track);
        }
    }

    if (karts_erase.size() == m_available_kts.first.size() ||
        tracks_erase.size() == m_available_kts.second.size() ||
        okt < ServerConfig::m_official_karts_threshold ||
        ott < ServerConfig::m_official_tracks_threshold)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
            .addUInt8(RR_INCOMPATIBLE_DATA);
        peer->cleanPlayerProfiles();
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player has incompatible karts / tracks.");
        return false;
    }

    std::array<int, AS_TOTAL> addons_scores = {{ -1, -1, -1, -1 }};
    size_t addon_kart = 0;
    size_t addon_track = 0;
    size_t addon_arena = 0;
    size_t addon_soccer = 0;

    for (auto& kart : m_addon_kts.first)
    {
        if (client_karts.find(kart) != client_karts.end())
            addon_kart++;
    }
    for (auto& track : m_addon_kts.second)
    {
        if (client_tracks.find(track) != client_tracks.end())
            addon_track++;
    }
    for (auto& arena : m_addon_arenas)
    {
        if (client_tracks.find(arena) != client_tracks.end())
            addon_arena++;
    }
    for (auto& soccer : m_addon_soccers)
    {
        if (client_tracks.find(soccer) != client_tracks.end())
            addon_soccer++;
    }

    if (!m_addon_kts.first.empty())
    {
        addons_scores[AS_KART] = int
            ((float)addon_kart / (float)m_addon_kts.first.size() * 100.0);
    }
    if (!m_addon_kts.second.empty())
    {
        addons_scores[AS_TRACK] = int
            ((float)addon_track / (float)m_addon_kts.second.size() * 100.0);
    }
    if (!m_addon_arenas.empty())
    {
        addons_scores[AS_ARENA] = int
            ((float)addon_arena / (float)m_addon_arenas.size() * 100.0);
    }
    if (!m_addon_soccers.empty())
    {
        addons_scores[AS_SOCCER] = int
            ((float)addon_soccer / (float)m_addon_soccers.size() * 100.0);
    }

    // Save available karts and tracks from clients in STKPeer so if this peer
    // disconnects later in lobby it won't affect current players
    peer->setAvailableKartsTracks(client_karts, client_tracks);
    peer->setAddonsScores(addons_scores);

    if (m_process_type == PT_CHILD &&
        peer->getHostId() == m_client_server_host_id.load())
    {
        // Update child process addons list too so player can choose later
        updateAddons();
        updateTracksForMode();
    }
    return true;
}   // handleAssets

//-----------------------------------------------------------------------------
void ServerLobby::connectionRequested(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    NetworkString& data = event->data();
    if (!checkDataSize(event, 14)) return;

    peer->cleanPlayerProfiles();

    // can we add the player ?
    if (!allowJoinedPlayersWaiting() &&
        (m_state.load() != WAITING_FOR_START_GAME ||
        m_game_setup->isGrandPrixStarted()))
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_BUSY);
        // send only to the peer that made the request and disconnect it now
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: selection started");
        return;
    }

    // Check server version
    int version = data.getUInt32();
    if (version < stk_config->m_min_server_version ||
        version > stk_config->m_max_server_version)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCOMPATIBLE_DATA);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: wrong server version");
        return;
    }
    std::string user_version;
    data.decodeString(&user_version);
    event->getPeer()->setUserVersion(user_version);

    unsigned list_caps = data.getUInt16();
    std::set<std::string> caps;
    for (unsigned i = 0; i < list_caps; i++)
    {
        std::string cap;
        data.decodeString(&cap);
        caps.insert(cap);
    }
    event->getPeer()->setClientCapabilities(caps);
    if (!handleAssets(data, event->getPeer()))
        return;

    unsigned player_count = data.getUInt8();
    uint32_t online_id = 0;
    uint32_t encrypted_size = 0;
    online_id = data.getUInt32();
    encrypted_size = data.getUInt32();

    // Will be disconnected if banned by IP
    testBannedForIP(peer.get());
    if (peer->isDisconnected())
        return;

    testBannedForIPv6(peer.get());
    if (peer->isDisconnected())
        return;

    if (online_id != 0)
        testBannedForOnlineId(peer.get(), online_id);
    // Will be disconnected if banned by online id
    if (peer->isDisconnected())
        return;

    unsigned total_players = 0;
    STKHost::get()->updatePlayers(NULL, NULL, &total_players);
    if (total_players + player_count + m_ai_profiles.size() >
        (unsigned)ServerConfig::m_server_max_players)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_TOO_MANY_PLAYERS);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: too many players");
        return;
    }

    // Reject non-valiated player joinning if WAN server and not disabled
    // encforement of validation, unless it's player from localhost or lan
    // And no duplicated online id or split screen players in ranked server
    // AIPeer only from lan and only 1 if ai handling
    std::set<uint32_t> all_online_ids =
        STKHost::get()->getAllPlayerOnlineIds();
    bool duplicated_ranked_player =
        all_online_ids.find(online_id) != all_online_ids.end();

    if (((encrypted_size == 0 || online_id == 0) &&
        !(peer->getAddress().isPublicAddressLocalhost() ||
        peer->getAddress().isLAN()) &&
        NetworkConfig::get()->isWAN() &&
        ServerConfig::m_validating_player) ||
        (ServerConfig::m_strict_players &&
        (player_count != 1 || online_id == 0 || duplicated_ranked_player)) ||
        (peer->isAIPeer() && !peer->getAddress().isLAN() &&!ServerConfig::m_ai_anywhere) ||
        (peer->isAIPeer() &&
        ServerConfig::m_ai_handling && !m_ai_peer.expired()))
    {
        NetworkString* message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_INVALID_PLAYER);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: invalid player");
        return;
    }

    if (ServerConfig::m_ai_handling && peer->isAIPeer())
        m_ai_peer = peer;

    if (encrypted_size != 0)
    {
        m_pending_connection[peer] = std::make_pair(online_id,
            BareNetworkString(data.getCurrentData(), encrypted_size));
    }
    else
    {
        core::stringw online_name;
        if (online_id > 0)
            data.decodeStringW(&online_name);
        handleUnencryptedConnection(peer, data, online_id, online_name,
            false/*is_pending_connection*/);
    }
}   // connectionRequested

//-----------------------------------------------------------------------------
void ServerLobby::handleUnencryptedConnection(std::shared_ptr<STKPeer> peer,
    BareNetworkString& data, uint32_t online_id,
    const core::stringw& online_name, bool is_pending_connection,
    std::string country_code)
{
    if (data.size() < 2) return;

    // Check for password
    std::string password;
    data.decodeString(&password);
    const std::string& server_pw = ServerConfig::m_private_server_password;
    if (password != server_pw)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCORRECT_PASSWORD);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: incorrect password");
        return;
    }

    // Check again max players and duplicated player in ranked server,
    // if this is a pending connection
    unsigned total_players = 0;
    unsigned player_count = data.getUInt8();

    if (is_pending_connection)
    {
        STKHost::get()->updatePlayers(NULL, NULL, &total_players);
        if (total_players + player_count >
            (unsigned)ServerConfig::m_server_max_players)
        {
            NetworkString *message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_TOO_MANY_PLAYERS);
            peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby", "Player refused: too many players");
            return;
        }

        std::set<uint32_t> all_online_ids =
            STKHost::get()->getAllPlayerOnlineIds();
        bool duplicated_ranked_player =
            all_online_ids.find(online_id) != all_online_ids.end();
        if (ServerConfig::m_ranked && duplicated_ranked_player)
        {
            NetworkString* message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INVALID_PLAYER);
            peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby", "Player refused: invalid player");
            return;
        }
    }

#ifdef ENABLE_SQLITE3
    if (country_code.empty() && !peer->getAddress().isIPv6())
        country_code = m_db_connector->ip2Country(peer->getAddress());
    if (country_code.empty() && peer->getAddress().isIPv6())
        country_code = m_db_connector->ipv62Country(peer->getAddress());
#endif

int prosLimit = ServerConfig::m_pros_limit;
  if (prosLimit !=0)
{
    // Convert irr::core::stringw to std::wstring
    std::wstring wideString(online_name.c_str());

    // Convert std::wstring to std::string
    std::string stdName(wideString.begin(), wideString.end());

    // Now you can use stdName as a std::string
    if (getPlayerInfo(stdName).first > prosLimit || getPlayerInfo(stdName).first == -1)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "Sorry mate, but you are not a pro yet!";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;

        peer->reset();
        return;

    }

}

  std::vector<std::wstring> banned_players;
        std::wifstream file("banned_players.txt");
        if (file.is_open()) {
            std::wstring word;
            while (std::getline(file, word)) {
                banned_players.push_back(word);
            }
            file.close();
        }
        else {
            // create the file if it doesn't exist with some default values
            std::wofstream new_file("banned_players.txt");
            new_file << L"player$\nto$\nban$";
            new_file.close();
            // read from the newly created file
            std::wifstream new_file_read("banned_players.txt");
            std::wstring word;
            while (std::getline(new_file_read, word)) {
                banned_players.push_back(word);
            }
            new_file_read.close();
        }
       // Example of converting irr::core::stringw to std::wstring
        std::wstring name_wstring(online_name.c_str());
        bool is_banned_name = false;

        // Remove symbols '-', '_', and '.'
        name_wstring.erase(std::remove_if(name_wstring.begin(), name_wstring.end(),
                                        [](wchar_t c) { return c == L'-' || c == L'_' || c == L'.'; }),
                        name_wstring.end());

        // Convert name to lowercase
        std::transform(name_wstring.begin(), name_wstring.end(), name_wstring.begin(), [](wchar_t c) { return std::towlower(c); });

        for (const auto& banned_name : banned_players) {
            std::wstring lowercase_banned_name;
            // Convert the banned name to lowercase
            std::transform(banned_name.begin(), banned_name.end(), std::back_inserter(lowercase_banned_name), [](wchar_t c) { return std::towlower(c); });

            // Check if the lowercase banned name is contained in the lowercase name_wstring
            if (name_wstring.find(lowercase_banned_name) != std::wstring::npos) {
                is_banned_name = true;
                break; // Stop the loop if a banned name is found
            }
        }
        if (is_banned_name) {
            peer->reset();
            Log::verbose("ServerLobby", "Player refused: banned player");

            // Writing to a file named "join_attempts.txt"
            std::wofstream file("join_attempts.txt", std::ios::app); // Open in append mode
            if (file.is_open()) {
                file << online_name.c_str() << L" attempted to join\n";
                file.close();
            }

            return;
        }

    auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
    for (unsigned i = 0; i < player_count; i++)
    {
        core::stringw name;
        data.decodeStringW(&name);
        // 30 to make it consistent with stk-addons max user name length
        if (name.empty())
            name = L"unnamed";
        else if (name.size() > 19)
            name = name.subString(0, 19);
        // Check if player's name is "Snoker" and set country code to "LB"
        if (online_name == L"Snoker")
            country_code = "LB";
        float default_kart_color = data.getFloat();
        HandicapLevel handicap = (HandicapLevel)data.getUInt8();
        auto player = std::make_shared<NetworkPlayerProfile>
            (peer, i == 0 && !online_name.empty() && !peer->isAIPeer() ?
            online_name.subString(0, 19) : name,
            peer->getHostId(), default_kart_color, i == 0 ? online_id : 0,
            handicap, (uint8_t)i, KART_TEAM_NONE,
            country_code);
        if (ServerConfig::m_team_choosing)
        {
            KartTeam cur_team = KART_TEAM_NONE;


                if (red_blue.first > red_blue.second)
                {
                    cur_team = KART_TEAM_BLUE;
                    red_blue.second++;
                }
                else if (red_blue.first < red_blue.second)
                {
                    cur_team = KART_TEAM_RED;
                    red_blue.first++;
                }
                else // red_blue.first == red_blue.second
                {
                    RandomGenerator rg;
                    cur_team = rg.get(2) == 0 ? KART_TEAM_RED : KART_TEAM_BLUE;
                    if (cur_team == KART_TEAM_RED)
                        red_blue.first++;
                    else
                        red_blue.second++;
                }
                player->setTeam(cur_team);
        }
        peer->addPlayer(player);
    }

    peer->setValidated(true);

    // send a message to the one that asked to connect
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info);
    delete server_info;

    const bool game_started = m_state.load() != WAITING_FOR_START_GAME;
    NetworkString* message_ack = getNetworkString(4);
    message_ack->setSynchronous(true);
    // connection success -- return the host id of peer
    float auto_start_timer = 0.0f;
    if (m_timeout.load() == std::numeric_limits<int64_t>::max())
        auto_start_timer = std::numeric_limits<float>::max();
    else
    {
        auto_start_timer =
            (m_timeout.load() - (int64_t)StkTime::getMonoTimeMs()) / 1000.0f;
    }
    message_ack->addUInt8(LE_CONNECTION_ACCEPTED).addUInt32(peer->getHostId())
        .addUInt32(ServerConfig::m_server_version);

    message_ack->addUInt16(
        (uint16_t)stk_config->m_network_capabilities.size());
    for (const std::string& cap : stk_config->m_network_capabilities)
        message_ack->encodeString(cap);

    message_ack->addFloat(auto_start_timer)
        .addUInt32(ServerConfig::m_state_frequency)
        .addUInt8(ServerConfig::m_chat ? 1 : 0)
        .addUInt8(playerReportsTableExists() ? 1 : 0);

    peer->setSpectator(false);

    // The 127.* or ::1/128 will be in charged for controlling AI
    if (m_ai_profiles.empty() && peer->getAddress().isLoopback())
    {
        unsigned ai_add = NetworkConfig::get()->getNumFixedAI();
        unsigned max_players = ServerConfig::m_server_max_players;
        // We need to reserve at least 1 slot for new player
        if (player_count + ai_add + 1 > max_players)
        {
            if (max_players >= player_count + 1)
                ai_add = max_players - player_count - 1;
            else
                ai_add = 0;
        }
        for (unsigned i = 0; i < ai_add; i++)
        {
#ifdef SERVER_ONLY
            core::stringw name = L"Bot";
#else
            core::stringw name = _("Bot");
#endif
            name += core::stringw(" ") + StringUtils::toWString(i + 1);
            
            m_ai_profiles.push_back(std::make_shared<NetworkPlayerProfile>
                (peer, name, peer->getHostId(), 0.0f, 0, HANDICAP_NONE,
                player_count + i, KART_TEAM_NONE, ""));
        }
    }

    if (game_started)
    {
        peer->setWaitingForGame(true);
        updatePlayerList();
        peer->sendPacket(message_ack);
        delete message_ack;
    }
    else
    {
        peer->setWaitingForGame(false);
        m_peers_ready[peer] = false;
        if (!ServerConfig::m_sql_management)
        {
            for (std::shared_ptr<NetworkPlayerProfile>& npp :
                peer->getPlayerProfiles())
            {
                Log::info("ServerLobby",
                    "New player %s with online id %u from %s with %s.",
                    StringUtils::wideToUtf8(npp->getName()).c_str(),
                    npp->getOnlineId(), peer->getAddress().toString().c_str(),
                    peer->getUserVersion().c_str());
            }
        }
        updatePlayerList();
        peer->sendPacket(message_ack);
        delete message_ack;

        if (ServerConfig::m_ranked)
        {
            getRankingForPlayer(peer->getPlayerProfiles()[0]);
        }
    }

#ifdef ENABLE_SQLITE3
    m_db_connector->onPlayerJoinQueries(peer, online_id, player_count, country_code);
#endif
}   // handleUnencryptedConnection

//-----------------------------------------------------------------------------
/** Called when any players change their setting (team for example), or
 *  connection / disconnection, it will use the game_started parameter to
 *  determine if this should be send to all peers in server or just in game.
 *  \param update_when_reset_server If true, this message will be sent to
 *  all peers.
 */
void ServerLobby::updatePlayerList(bool update_when_reset_server)
{
    const bool game_started = m_state.load() != WAITING_FOR_START_GAME &&
        !update_when_reset_server;

    auto all_profiles = STKHost::get()->getAllPlayerProfiles();
    size_t all_profiles_size = all_profiles.size();
    for (auto& profile : all_profiles)
    {
        if (profile->getPeer()->alwaysSpectate())
            all_profiles_size--;
    }

    auto spectators_by_limit = getSpectatorsByLimit();

    // N - 1 AI
    auto ai_instance = m_ai_peer.lock();
    if (supportsAI())
    {
        if (ai_instance)
        {
            auto ai_profiles = ai_instance->getPlayerProfiles();
            if (m_state.load() == WAITING_FOR_START_GAME ||
                update_when_reset_server)
            {
                if (all_profiles_size > ai_profiles.size())
                    ai_profiles.clear();
                else if (all_profiles_size != 0)
                {
                    ai_profiles.resize(
                        ai_profiles.size() - all_profiles_size + 1);
                }
            }
            else
            {
                // Use fixed number of AI calculated when started game
                ai_profiles.resize(m_ai_count);
            }
            all_profiles.insert(all_profiles.end(), ai_profiles.begin(),
                ai_profiles.end());
            m_current_ai_count.store((int)ai_profiles.size());
        }
        else if (!m_ai_profiles.empty())
        {
            all_profiles.insert(all_profiles.end(), m_ai_profiles.begin(),
                m_ai_profiles.end());
            m_current_ai_count.store((int)m_ai_profiles.size());
        }
    }
    else
        m_current_ai_count.store(0);

    m_lobby_players.store((int)all_profiles.size());

    // No need to update player list (for started grand prix currently)
    if (!allowJoinedPlayersWaiting() &&
        m_state.load() > WAITING_FOR_START_GAME && !update_when_reset_server)
        return;

    NetworkString* pl = getNetworkString();
    pl->setSynchronous(true);
    pl->addUInt8(LE_UPDATE_PLAYER_LIST)
        .addUInt8((uint8_t)(game_started ? 1 : 0))
        .addUInt8((uint8_t)all_profiles.size());
    for (auto profile : all_profiles)
    {
        auto profile_name = profile->getName();

        const wchar_t* profile_name_wide_cstr = profile_name.c_str();

        // Allocate a buffer for the std::string.
        std::size_t buffer_size = std::wcsrtombs(nullptr, &profile_name_wide_cstr, 0, nullptr);
        if (buffer_size == static_cast<std::size_t>(-1)) {
            // Handle the error.
            return;
        }
        std::string profile_name_for_rank(buffer_size, '\0');

        // Convert the const wchar_t* to a std::string.
        std::wcsrtombs(&profile_name_for_rank[0], &profile_name_wide_cstr, buffer_size, nullptr);

        // Add medal ranking
        if(getPlayerInfoCurrentRanking(profile_name_for_rank).first == 1)
            profile_name = StringUtils::utf32ToWide({ 0x1F947 }) + profile_name;
        else if (getPlayerInfoCurrentRanking(profile_name_for_rank).first == 2)
            profile_name = StringUtils::utf32ToWide({ 0x1F948 }) + profile_name;
        else if (getPlayerInfoCurrentRanking(profile_name_for_rank).first == 3)
            profile_name = StringUtils::utf32ToWide({ 0x1F949 }) + profile_name;

         // Add a Cedar tree emoji if the player's name is "Cedar"
        if (profile_name == L"Cedar")
            profile_name += irr::core::stringw(L" ") + StringUtils::utf32ToWide({ 0x0001F1F1, 0x0001F1E7 });

        // get OS information
        auto version_os = StringUtils::extractVersionOS(profile->getPeer()->getUserVersion());
        std::string os_type_str = version_os.second;
        // if mobile OS
        if (os_type_str == "iOS" || os_type_str == "Android")
            // Add a Mobile emoji for mobile OS
            profile_name = StringUtils::utf32ToWide({ 0x1F4F1 }) + profile_name;

        // Add an hourglass emoji for players waiting because of the player limit
        if (spectators_by_limit.find(profile->getPeer()) != spectators_by_limit.end()) 
            profile_name = StringUtils::utf32ToWide({ 0x231B }) + profile_name;

        pl->addUInt32(profile->getHostId()).addUInt32(profile->getOnlineId())
            .addUInt8(profile->getLocalPlayerId())
            .encodeString(profile_name);

        std::shared_ptr<STKPeer> p = profile->getPeer();
        uint8_t boolean_combine = 0;
        if (p && p->isWaitingForGame())
            boolean_combine |= 1;
        if (p && (p->isSpectator() ||
            ((m_state.load() == WAITING_FOR_START_GAME ||
            update_when_reset_server) && p->alwaysSpectate())))
            boolean_combine |= (1 << 1);
        if (p && m_server_owner_id.load() == p->getHostId())
            boolean_combine |= (1 << 2);
        if (ServerConfig::m_owner_less && !game_started &&
            m_peers_ready.find(p) != m_peers_ready.end() &&
            m_peers_ready.at(p))
            boolean_combine |= (1 << 3);
        if ((p && p->isAIPeer()) || isAIProfile(profile))
            boolean_combine |= (1 << 4);
        pl->addUInt8(boolean_combine);
        pl->addUInt8(profile->getHandicap());
        if (ServerConfig::m_team_choosing &&
            RaceManager::get()->teamEnabled())
            pl->addUInt8(profile->getTeam());
        else
            pl->addUInt8(KART_TEAM_NONE);
        pl->encodeString(profile->getCountryCode());
    }

    // Don't send this message to in-game players
    STKHost::get()->sendPacketToAllPeersWith([game_started]
        (STKPeer* p)
        {
            if (!p->isValidated())
                return false;
            if (!p->isWaitingForGame() && game_started)
                return false;
            return true;
        }, pl);
    delete pl;
}   // updatePlayerList

//-----------------------------------------------------------------------------
void ServerLobby::updateServerOwner()
{
    if (m_state.load() < WAITING_FOR_START_GAME ||
        m_state.load() > RESULT_DISPLAY ||
        ServerConfig::m_owner_less)
        return;
    if (!m_server_owner.expired())
        return;
    auto peers = STKHost::get()->getPeers();
    if (peers.empty())
        return;
    std::sort(peers.begin(), peers.end(), [](const std::shared_ptr<STKPeer> a,
        const std::shared_ptr<STKPeer> b)->bool
        {
            return a->getHostId() < b->getHostId();
        });

    std::shared_ptr<STKPeer> owner;
    for (auto peer: peers)
    {
        // Only matching host id can be server owner in case of
        // graphics-client-server
        if (peer->isValidated() && !peer->isAIPeer() &&
            (m_process_type == PT_MAIN ||
            peer->getHostId() == m_client_server_host_id.load()))
        {
            owner = peer;
            break;
        }
    }
    if (owner)
    {
        NetworkString* ns = getNetworkString();
        ns->setSynchronous(true);
        ns->addUInt8(LE_SERVER_OWNERSHIP);
        owner->sendPacket(ns);
        delete ns;
        m_server_owner = owner;
        m_server_owner_id.store(owner->getHostId());
        updatePlayerList();
    }
}   // updateServerOwner

//-----------------------------------------------------------------------------
/*! \brief Called when a player asks to select karts.
 *  \param event : Event providing the information.
 */
void ServerLobby::kartSelectionRequested(Event* event)
{
    if (m_state != SELECTING || m_game_setup->isGrandPrixStarted())
    {
        Log::warn("ServerLobby", "Received kart selection while in state %d.",
                  m_state.load());
        return;
    }

    if (!checkDataSize(event, 1) ||
        event->getPeer()->getPlayerProfiles().empty())
        return;

    const NetworkString& data = event->data();
    STKPeer* peer = event->getPeer();
    setPlayerKarts(data, peer);
}   // kartSelectionRequested

//-----------------------------------------------------------------------------
/*! \brief Called when a player votes for track(s), it will auto correct client
 *         data if it sends some invalid data.
 *  \param event : Event providing the information.
 */
void ServerLobby::handlePlayerVote(Event* event)
{
    if (m_state != SELECTING || !ServerConfig::m_track_voting)
    {
        Log::warn("ServerLobby", "Received track vote while in state %d.",
                  m_state.load());
        return;
    }

    if (!checkDataSize(event, 4) ||
        event->getPeer()->getPlayerProfiles().empty() ||
        event->getPeer()->isWaitingForGame())
        return;

    if (isVotingOver())  return;

    NetworkString& data = event->data();
    PeerVote vote(data);
    Log::debug("ServerLobby",
        "Vote from client: host %d, track %s, laps %d, reverse %d.",
        event->getPeer()->getHostId(), vote.m_track_name.c_str(),
        vote.m_num_laps, vote.m_reverse);

    Track* t = track_manager->getTrack(vote.m_track_name);
    if (!t)
    {
        vote.m_track_name = *m_available_kts.second.begin();
        t = track_manager->getTrack(vote.m_track_name);
        assert(t);
    }

    // Remove / adjust any invalid settings
    if (RaceManager::get()->modeHasLaps())
    {
        if (ServerConfig::m_auto_game_time_ratio > 0.0f)
        {
            vote.m_num_laps =
                (uint8_t)(fmaxf(1.0f, (float)t->getDefaultNumberOfLaps() *
                ServerConfig::m_auto_game_time_ratio));
        }
        else if (m_fixed_laps != -1)
            vote.m_num_laps = m_fixed_laps;
        else if (vote.m_num_laps == 0 || vote.m_num_laps > 20)
            vote.m_num_laps = (uint8_t)3;
        if (!t->reverseAvailable() && vote.m_reverse)
            vote.m_reverse = false;
    }
    else if (RaceManager::get()->isSoccerMode())
    {
        if (m_game_setup->isSoccerGoalTarget())
        {
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                vote.m_num_laps = (uint8_t)(ServerConfig::m_auto_game_time_ratio *
                                            UserConfigParams::m_num_goals);
            }
            else if (vote.m_num_laps > 10)
                vote.m_num_laps = (uint8_t)5;
        }
        else
        {
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                vote.m_num_laps = (uint8_t)(ServerConfig::m_auto_game_time_ratio *
                                            UserConfigParams::m_soccer_time_limit);
            }
            else if (vote.m_num_laps > 15)
                vote.m_num_laps = (uint8_t)7;
        }
    }
    else if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        vote.m_num_laps = 0;
    }
    else if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
    {
        vote.m_num_laps = 0;
        vote.m_reverse = false;
    }

    // Store vote:
    vote.m_player_name = event->getPeer()->getPlayerProfiles()[0]->getName();
    addVote(event->getPeer()->getHostId(), vote);

    // Now inform all clients about the vote
    NetworkString other = NetworkString(PROTOCOL_LOBBY_ROOM);
    other.setSynchronous(true);
    other.addUInt8(LE_VOTE);
    other.addUInt32(event->getPeer()->getHostId());
    vote.encode(&other);
    sendMessageToPeers(&other);

}   // handlePlayerVote

// ----------------------------------------------------------------------------
/** Select the track to be used based on all votes being received.
 * \param winner_vote The PeerVote that was picked.
 * \param winner_peer_id The host id of winner (unchanged if no vote).
 *  \return True if race can go on, otherwise wait.
 */
bool ServerLobby::handleAllVotes(PeerVote* winner_vote,
                                 uint32_t* winner_peer_id)
{
    // Determine majority agreement when 35% of voting time remains,
    // reserve some time for kart selection so it's not 50%
    if (getRemainingVotingTime() / getMaxVotingTime() > 0.35f)
    {
        return false;
    }

    // First remove all votes from disconnected hosts
    auto it = m_peers_votes.begin();
    while (it != m_peers_votes.end())
    {
        auto peer = STKHost::get()->findPeerByHostId(it->first);
        if (peer == nullptr)
        {
            it = m_peers_votes.erase(it);
        }
        else
            it++;
    }

    if (m_peers_votes.empty())
    {
        if (isVotingOver())
        {
            *winner_vote = *m_default_vote;
            return true;
        }
        return false;
    }

    // Count number of players 
    float cur_players = 0.0f;
    auto peers = STKHost::get()->getPeers();
    for (auto peer : peers)
    {
        if (peer->isAIPeer())
            continue;
        if (peer->hasPlayerProfiles() && !peer->isWaitingForGame())
            cur_players += 1.0f;
    }

    std::string top_track = m_default_vote->m_track_name;
    unsigned top_laps = m_default_vote->m_num_laps;
    bool top_reverse = m_default_vote->m_reverse;

    std::map<std::string, unsigned> tracks;
    std::map<unsigned, unsigned> laps;
    std::map<bool, unsigned> reverses;

    // Ratio to determine majority agreement
    float tracks_rate = 0.0f;
    float laps_rate = 0.0f;
    float reverses_rate = 0.0f;

    for (auto& p : m_peers_votes)
    {
        auto track_vote = tracks.find(p.second.m_track_name);
        if (track_vote == tracks.end())
            tracks[p.second.m_track_name] = 1;
        else
            track_vote->second++;
        auto lap_vote = laps.find(p.second.m_num_laps);
        if (lap_vote == laps.end())
            laps[p.second.m_num_laps] = 1;
        else
            lap_vote->second++;
        auto reverse_vote = reverses.find(p.second.m_reverse);
        if (reverse_vote == reverses.end())
            reverses[p.second.m_reverse] = 1;
        else
            reverse_vote->second++;
    }

    findMajorityValue<std::string>(tracks, cur_players, &top_track, &tracks_rate);
    findMajorityValue<unsigned>(laps, cur_players, &top_laps, &laps_rate);
    findMajorityValue<bool>(reverses, cur_players, &top_reverse, &reverses_rate);

    // End early if there is majority agreement which is all entries rate > 0.5
    it = m_peers_votes.begin();
    if (tracks_rate > 0.5f && laps_rate > 0.5f && reverses_rate > 0.5f)
    {
        while (it != m_peers_votes.end())
        {
            if (it->second.m_track_name == top_track &&
                it->second.m_num_laps == top_laps &&
                it->second.m_reverse == top_reverse)
                break;
            else
                it++;
        }
        if (it == m_peers_votes.end())
        {
            // Don't end if no vote matches all majority choices
            Log::warn("ServerLobby",
                "Missing track %s from majority.", top_track.c_str());
            it = m_peers_votes.begin();
            if (!isVotingOver())
                return false;
        }
        *winner_peer_id = it->first;
        *winner_vote = it->second;
        return true;
    }
    if (isVotingOver())
    {
        // Pick the best lap (or soccer goal / time) from only the top track
        // if no majority agreement from all
        int diff = std::numeric_limits<int>::max();
        auto closest_lap = m_peers_votes.begin();
        while (it != m_peers_votes.end())
        {
            if (it->second.m_track_name == top_track &&
                std::abs((int)it->second.m_num_laps - (int)top_laps) < diff)
            {
                closest_lap = it;
                diff = std::abs((int)it->second.m_num_laps - (int)top_laps);
            }
            else
                it++;
        }
        *winner_peer_id = closest_lap->first;
        *winner_vote = closest_lap->second;
        return true;
    }
    return false;
}   // handleAllVotes

// ----------------------------------------------------------------------------
template<typename T>
void ServerLobby::findMajorityValue(const std::map<T, unsigned>& choices, unsigned cur_players,
                       T* best_choice, float* rate)
{
    RandomGenerator rg;
    unsigned max_votes = 0;
    auto best_iter = choices.begin();
    unsigned best_iters_count = 1;
    // Among choices with max votes, we need to pick one uniformly,
    // thus we have to keep track of their number
    for (auto iter = choices.begin(); iter != choices.end(); iter++)
    {
        if (iter->second > max_votes)
        {
            max_votes = iter->second;
            best_iter = iter;
            best_iters_count = 1;
        }
        else if (iter->second == max_votes)
        {
            best_iters_count++;
            if (rg.get(best_iters_count) == 0)
            {
                max_votes = iter->second;
                best_iter = iter;
            }
        }
    }
    if (best_iter != choices.end())
    {
        *best_choice = best_iter->first;
        *rate = float(best_iter->second) / cur_players;
    }
}   // findMajorityValue

// ----------------------------------------------------------------------------
void ServerLobby::getHitCaptureLimit()
{
    int hit_capture_limit = std::numeric_limits<int>::max();
    float time_limit = 0.0f;
    if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
    {
        if (ServerConfig::m_capture_limit > 0)
            hit_capture_limit = ServerConfig::m_capture_limit;
        if (ServerConfig::m_time_limit_ctf > 0)
            time_limit = (float)ServerConfig::m_time_limit_ctf;
    }
    else
    {
        if (ServerConfig::m_hit_limit > 0)
            hit_capture_limit = ServerConfig::m_hit_limit;
        if (ServerConfig::m_time_limit_ffa > 0.0f)
            time_limit = (float)ServerConfig::m_time_limit_ffa;
    }
    m_battle_hit_capture_limit = hit_capture_limit;
    m_battle_time_limit = time_limit;
}   // getHitCaptureLimit

// ----------------------------------------------------------------------------
/** Called from the RaceManager of the server when the world is loaded. Marks
 *  the server to be ready to start the race.
 */
void ServerLobby::finishedLoadingWorld()
{
    for (auto p : m_peers_ready)
    {
        if (auto peer = p.first.lock())
            peer->updateLastActivity();
    }
    m_server_has_loaded_world.store(true);
}   // finishedLoadingWorld;

//-----------------------------------------------------------------------------
/** Called when a client notifies the server that it has loaded the world.
 *  When all clients and the server are ready, the race can be started.
 */
void ServerLobby::finishedLoadingWorldClient(Event *event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    peer->updateLastActivity();
    m_peers_ready.at(peer) = true;
    Log::info("ServerLobby", "Peer %d has finished loading world at %lf",
        peer->getHostId(), StkTime::getRealTime());

    // Send a random tip when the peer finishes loading
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    std::string msg = random_tip;
    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true /* reliable */);
    delete chat;

}   // finishedLoadingWorldClient

//-----------------------------------------------------------------------------
/** Called when a client clicks on 'ok' on the race result screen.
 *  If all players have clicked on 'ok', go back to the lobby.
 */
void ServerLobby::playerFinishedResult(Event *event)
{
    if (m_rs_state.load() == RS_ASYNC_RESET ||
        m_state.load() != RESULT_DISPLAY)
        return;
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    m_peers_ready.at(peer) = true;
}   // playerFinishedResult

//-----------------------------------------------------------------------------
bool ServerLobby::waitingForPlayers() const
{
    if (m_game_setup->isGrandPrix() && m_game_setup->isGrandPrixStarted())
        return false;
    return m_state.load() >= WAITING_FOR_START_GAME;
}   // waitingForPlayers

//-----------------------------------------------------------------------------
void ServerLobby::handlePendingConnection()
{
    std::lock_guard<std::mutex> lock(m_keys_mutex);

    for (auto it = m_pending_connection.begin();
         it != m_pending_connection.end();)
    {
        auto peer = it->first.lock();
        if (!peer)
        {
            it = m_pending_connection.erase(it);
        }
        else
        {
            const uint32_t online_id = it->second.first;
            auto key = m_keys.find(online_id);
            if (key != m_keys.end() && key->second.m_tried == false)
            {
                try
                {
                    if (decryptConnectionRequest(peer, it->second.second,
                        key->second.m_aes_key, key->second.m_aes_iv, online_id,
                        key->second.m_name, key->second.m_country_code))
                    {
                        it = m_pending_connection.erase(it);
                        m_keys.erase(online_id);
                        continue;
                    }
                    else
                        key->second.m_tried = true;
                }
                catch (std::exception& e)
                {
                    Log::error("ServerLobby",
                        "handlePendingConnection error: %s", e.what());
                    key->second.m_tried = true;
                }
            }
            it++;
        }
    }
}   // handlePendingConnection

//-----------------------------------------------------------------------------
bool ServerLobby::decryptConnectionRequest(std::shared_ptr<STKPeer> peer,
    BareNetworkString& data, const std::string& key, const std::string& iv,
    uint32_t online_id, const core::stringw& online_name,
    const std::string& country_code)
{
    SoccerWorld* sw = (SoccerWorld*)World::getWorld();
    auto crypto = std::unique_ptr<Crypto>(new Crypto(
        Crypto::decode64(key), Crypto::decode64(iv)));
    if (crypto->decryptConnectionRequest(data))
    {
        peer->setCrypto(std::move(crypto));
        Log::info("ServerLobby", "%s validated",
            StringUtils::wideToUtf8(online_name).c_str());
        handleUnencryptedConnection(peer, data, online_id,
            online_name, true/*is_pending_connection*/, country_code);

        std::string peerName = StringUtils::wideToUtf8(online_name);
            if (peerName != lastJoinedName) {
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                std::string msg = "\U0001f6ec " + peerName + " "+generateRandomMessage(JOINED_MESSAGES);
                chat->encodeString16(StringUtils::utf8ToWide(msg));
                auto peers = STKHost::get()->getPeers();
                for (auto& p : peers) {
                    if (!p->isValidated()) continue;
                    p->sendPacket(chat, true /* reliable */);
                }
                delete chat;

                // Update lastJoinedName
                lastJoinedName = peerName;
            }

        if (m_state.load() == RACING){
        const int red_score = sw->getScore(KART_TEAM_RED);
        const int blue_score = sw->getScore(KART_TEAM_BLUE);
        std::string msg = "\nScore:\n\U0001f7e5 Red " + std::to_string(red_score)+ " : " + std::to_string(blue_score) + " Blue \U0001f7e6\n";
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;

    }

        return true;
    }
    return false;
}   // decryptConnectionRequest

//-----------------------------------------------------------------------------
void ServerLobby::getRankingForPlayer(std::shared_ptr<NetworkPlayerProfile> p)
{
    int priority = Online::RequestManager::HTTP_MAX_PRIORITY;
    auto request = std::make_shared<Online::XMLRequest>(priority);
    NetworkConfig::get()->setUserDetails(request, "get-ranking");

    const uint32_t id = p->getOnlineId();
    request->addParameter("id", id);
    request->executeNow();

    const XMLNode* result = request->getXMLData();
    std::string rec_success;

    bool success = false;
    if (result->get("success", &rec_success))
        if (rec_success == "yes")
                    success = true;
    if (!success)
    {
        Log::error("ServerLobby", "No ranking info found for player %s.",
            StringUtils::wideToUtf8(p->getName()).c_str());
        // Kick the player to avoid his score being reset in case
        // connection to stk addons is broken
        auto peer = p->getPeer();
        if (peer)
        {
            peer->kick();
            return;
        }
    }
    m_ranking->fill(id, result, p);
}   // getRankingForPlayer

//-----------------------------------------------------------------------------
void ServerLobby::submitRankingsToAddons()
{
    // No ranking for battle mode
    if (!RaceManager::get()->modeHasLaps())
        return;

    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        const RankingEntry& scores = m_ranking->getScores(id);
        auto request = std::make_shared<SubmitRankingRequest>
            (scores, RaceManager::get()->getKartInfo(i).getCountryCode());
        NetworkConfig::get()->setUserDetails(request, "submit-ranking");
        Log::info("ServerLobby", "Submitting ranking for %s (%d) : %lf, %lf %d",
            StringUtils::wideToUtf8(
            RaceManager::get()->getKartInfo(i).getPlayerName()).c_str(), id,
            scores.score, scores.max_score, scores.races);
        request->queue();
    }
}   // submitRankingsToAddons

//-----------------------------------------------------------------------------
/** This function is called when all clients have loaded the world and
 *  are therefore ready to start the race. It determine the start time in
 *  network timer for client and server based on pings and then switches state
 *  to WAIT_FOR_RACE_STARTED.
 */
void ServerLobby::configPeersStartTime()
{
    uint32_t max_ping = 0;
    const unsigned max_ping_from_peers = ServerConfig::m_max_ping;
    bool peer_exceeded_max_ping = false;
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
        // Spectators don't send input so we don't need to delay for them
        if (!peer || peer->alwaysSpectate())
            continue;
        if (peer->getAveragePing() > max_ping_from_peers)
        {
            Log::warn("ServerLobby",
                "Peer %s cannot catch up with max ping %d.",
                peer->getAddress().toString().c_str(), max_ping);
            peer_exceeded_max_ping = true;
            continue;
        }
        max_ping = std::max(peer->getAveragePing(), max_ping);
    }
    if ((ServerConfig::m_high_ping_workaround && peer_exceeded_max_ping) ||
        (ServerConfig::m_live_players && RaceManager::get()->supportsLiveJoining()))
    {
        Log::info("ServerLobby", "Max ping to ServerConfig::m_max_ping for "
            "live joining or high ping workaround.");
        max_ping = ServerConfig::m_max_ping;
    }
    // Start up time will be after 2500ms, so even if this packet is sent late
    // (due to packet loss), the start time will still ahead of current time
    uint64_t start_time = STKHost::get()->getNetworkTimer() + (uint64_t)2500;
    powerup_manager->setRandomSeed(start_time);
    NetworkString* ns = getNetworkString(10);
    ns->setSynchronous(true);
    ns->addUInt8(LE_START_RACE).addUInt64(start_time);
    const uint8_t cc = (uint8_t)Track::getCurrentTrack()->getCheckManager()->getCheckStructureCount();
    ns->addUInt8(cc);
    *ns += *m_items_complete_state;
    m_client_starting_time = start_time;
    sendMessageToPeers(ns, /*reliable*/true);

    const unsigned jitter_tolerance = ServerConfig::m_jitter_tolerance;
    Log::info("ServerLobby", "Max ping from peers: %d, jitter tolerance: %d",
        max_ping, jitter_tolerance);
    // Delay server for max ping / 2 from peers and jitter tolerance.
    m_server_delay = (uint64_t)(max_ping / 2) + (uint64_t)jitter_tolerance;
    start_time += m_server_delay;
    m_server_started_at = start_time;
    delete ns;
    m_state = WAIT_FOR_RACE_STARTED;

    World::getWorld()->setPhase(WorldStatus::SERVER_READY_PHASE);
    // Different stk process thread may have different stk host
    STKHost* stk_host = STKHost::get();
    joinStartGameThread();
    m_start_game_thread = std::thread([start_time, stk_host, this]()
        {
            const uint64_t cur_time = stk_host->getNetworkTimer();
            assert(start_time > cur_time);
            int sleep_time = (int)(start_time - cur_time);
            //Log::info("ServerLobby", "Start game after %dms", sleep_time);
            StkTime::sleep(sleep_time);
            //Log::info("ServerLobby", "Started at %lf", StkTime::getRealTime());
            m_state.store(RACING);
        });
}   // configPeersStartTime

//-----------------------------------------------------------------------------
bool ServerLobby::allowJoinedPlayersWaiting() const
{
    return !m_game_setup->isGrandPrix();
}   // allowJoinedPlayersWaiting

//-----------------------------------------------------------------------------
void ServerLobby::addWaitingPlayersToGame()
{
    auto all_profiles = STKHost::get()->getAllPlayerProfiles();
    for (auto& profile : all_profiles)
    {
        auto peer = profile->getPeer();
        if (!peer || !peer->isValidated())
            continue;

        peer->resetAlwaysSpectateFull();
        peer->setWaitingForGame(false);
        peer->setSpectator(false);
        if (m_peers_ready.find(peer) == m_peers_ready.end())
        {
            m_peers_ready[peer] = false;
            if (!ServerConfig::m_sql_management)
            {
                Log::info("ServerLobby",
                    "New player %s with online id %u from %s with %s.",
                    StringUtils::wideToUtf8(profile->getName()).c_str(),
                    profile->getOnlineId(),
                    peer->getAddress().toString().c_str(),
                    peer->getUserVersion().c_str());
            }
        }
        uint32_t online_id = profile->getOnlineId();
        if (ServerConfig::m_ranked && !m_ranking->has(online_id))
        {
            getRankingForPlayer(peer->getPlayerProfiles()[0]);
        }
    }
    // Re-activiate the ai
    if (auto ai = m_ai_peer.lock())
        ai->setValidated(true);
}   // addWaitingPlayersToGame

//-----------------------------------------------------------------------------
void ServerLobby::resetServer()
{
    addWaitingPlayersToGame();
    resetPeersReady();
    updatePlayerList(true/*update_when_reset_server*/);
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    sendMessageToPeersInServer(server_info);
    delete server_info;
    setup();
    m_state = NetworkConfig::get()->isLAN() ?
        WAITING_FOR_START_GAME : REGISTER_SELF_ADDRESS;
    updatePlayerList();
}   // resetServer

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForIP(STKPeer* peer) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db_connector->hasDatabase() || !m_db_connector->hasIpBanTable())
        return;

    // Test for IPv4
    if (peer->getAddress().isIPv6())
        return;

    bool is_banned = false;
    uint32_t ip_start = 0;
    uint32_t ip_end = 0;

    std::vector<DatabaseConnector::IpBanTableData> ip_ban_list =
            m_db_connector->getIpBanTableData(peer->getAddress().getIP());
    if (!ip_ban_list.empty())
    {
        is_banned = true;
        ip_start = ip_ban_list[0].ip_start;
        ip_end = ip_ban_list[0].ip_end;
        int row_id = ip_ban_list[0].row_id;
        std::string reason = ip_ban_list[0].reason;
        std::string description = ip_ban_list[0].description;
        Log::info("ServerLobby", "%s banned by IP: %s "
                "(rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason.c_str(), row_id, description.c_str());
        kickPlayerWithReason(peer, reason.c_str());
    }
    if (is_banned)
        m_db_connector->increaseIpBanTriggerCount(ip_start, ip_end);
#endif
}   // testBannedForIP

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForIPv6(STKPeer* peer) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db_connector->hasDatabase() || !m_db_connector->hasIpv6BanTable())
        return;

    // Test for IPv6
    if (!peer->getAddress().isIPv6())
        return;

    bool is_banned = false;
    std::string ipv6_cidr = "";

    std::vector<DatabaseConnector::Ipv6BanTableData> ipv6_ban_list =
            m_db_connector->getIpv6BanTableData(peer->getAddress().toString(false));

    if (!ipv6_ban_list.empty())
    {
        is_banned = true;
        ipv6_cidr = ipv6_ban_list[0].ipv6_cidr;
        int row_id = ipv6_ban_list[0].row_id;
        std::string reason = ipv6_ban_list[0].reason;
        std::string description = ipv6_ban_list[0].description;
        Log::info("ServerLobby", "%s banned by IPv6: %s "
                "(rowid: %d, description: %s).",
                peer->getAddress().toString(false).c_str(), reason.c_str(), row_id, description.c_str());
        kickPlayerWithReason(peer, reason.c_str());
    }
    if (is_banned)
        m_db_connector->increaseIpv6BanTriggerCount(ipv6_cidr);
#endif
}   // testBannedForIPv6

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForOnlineId(STKPeer* peer,
                                        uint32_t online_id) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db_connector->hasDatabase() || !m_db_connector->hasOnlineIdBanTable())
        return;

    bool is_banned = false;
    std::vector<DatabaseConnector::OnlineIdBanTableData> online_id_ban_list =
            m_db_connector->getOnlineIdBanTableData(online_id);

    if (!online_id_ban_list.empty())
    {
        is_banned = true;
        int row_id = online_id_ban_list[0].row_id;
        std::string reason = online_id_ban_list[0].reason;
        std::string description = online_id_ban_list[0].description;
        Log::info("ServerLobby", "%s banned by online id: %s "
                "(online id: %u, rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason.c_str(), online_id, row_id, description.c_str());
        kickPlayerWithReason(peer, reason.c_str());
    }
    if (is_banned)
        m_db_connector->increaseOnlineIdBanTriggerCount(online_id);
#endif
}   // testBannedForOnlineId

//-----------------------------------------------------------------------------
void ServerLobby::listBanTable()
{
#ifdef ENABLE_SQLITE3
    m_db_connector->listBanTable();
#endif
}   // listBanTable

//-----------------------------------------------------------------------------
float ServerLobby::getStartupBoostOrPenaltyForKart(uint32_t ping,
                                                   unsigned kart_id)
{
    AbstractKart* k = World::getWorld()->getKart(kart_id);
    if (k->getStartupBoost() != 0.0f)
        return k->getStartupBoost();
    uint64_t now = STKHost::get()->getNetworkTimer();
    uint64_t client_time = now - ping / 2;
    uint64_t server_time = client_time + m_server_delay;
    int ticks = stk_config->time2Ticks(
        (float)(server_time - m_server_started_at) / 1000.0f);
    if (ticks < stk_config->time2Ticks(1.0f))
    {
        PlayerController* pc =
            dynamic_cast<PlayerController*>(k->getController());
        pc->displayPenaltyWarning();
        return -1.0f;
    }
    float f = k->getStartupBoostFromStartTicks(ticks);
    k->setStartupBoost(f);
    return f;
}   // getStartupBoostOrPenaltyForKart

//-----------------------------------------------------------------------------
/*! \brief Called when the server owner request to change game mode or
 *         difficulty.
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0            1            2
 *       -----------------------------------------------
 *  Size |     1      |     1     |         1          |
 *  Data | difficulty | game mode | soccer goal target |
 *       -----------------------------------------------
 */
void ServerLobby::handleServerConfiguration(Event* event)
{
    if (m_state != WAITING_FOR_START_GAME)
    {
        Log::warn("ServerLobby",
            "Received handleServerConfiguration while being in state %d.",
            m_state.load());
        return;
    }
    if (!ServerConfig::m_server_configurable)
    {
        Log::warn("ServerLobby", "server-configurable is not enabled.");
        return;
    }
    if (event->getPeerSP() != m_server_owner.lock())
    {
        Log::warn("ServerLobby",
            "Client %d is not authorised to config server.",
            event->getPeer()->getHostId());
        return;
    }
    NetworkString& data = event->data();
    int new_difficulty = data.getUInt8();
    int new_game_mode = data.getUInt8();
    bool new_soccer_goal_target = data.getUInt8() == 1;
    auto modes = ServerConfig::getLocalGameMode(new_game_mode);
    if (modes.second == RaceManager::MAJOR_MODE_GRAND_PRIX)
    {
        Log::warn("ServerLobby", "Grand prix is used for new mode.");
        return;
    }

    RaceManager::get()->setMinorMode(modes.first);
    RaceManager::get()->setMajorMode(modes.second);
    RaceManager::get()->setDifficulty(RaceManager::Difficulty(new_difficulty));
    m_game_setup->resetExtraServerInfo();
    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_SOCCER)
        m_game_setup->setSoccerGoalTarget(new_soccer_goal_target);

    if (NetworkConfig::get()->isWAN() &&
        (m_difficulty.load() != new_difficulty ||
        m_game_mode.load() != new_game_mode))
    {
        Log::info("ServerLobby", "Updating server info with new "
            "difficulty: %d, game mode: %d to stk-addons.", new_difficulty,
            new_game_mode);
        int priority = Online::RequestManager::HTTP_MAX_PRIORITY;
        auto request = std::make_shared<Online::XMLRequest>(priority);
        NetworkConfig::get()->setServerDetails(request, "update-config");
        const SocketAddress& addr = STKHost::get()->getPublicAddress();
        request->addParameter("address", addr.getIP());
        request->addParameter("port", addr.getPort());
        request->addParameter("new-difficulty", new_difficulty);
        request->addParameter("new-game-mode", new_game_mode);
        request->queue();
    }
    m_difficulty.store(new_difficulty);
    m_game_mode.store(new_game_mode);
    updateTracksForMode();

    auto peers = STKHost::get()->getPeers();
    for (auto& peer : peers)
    {
        auto assets = peer->getClientAssets();
        if (!peer->isValidated() || assets.second.empty())
            continue;
        std::set<std::string> tracks_erase;
        for (const std::string& server_track : m_available_kts.second)
        {
            if (assets.second.find(server_track) == assets.second.end())
            {
                tracks_erase.insert(server_track);
            }
        }
        if (tracks_erase.size() == m_available_kts.second.size())
        {
            NetworkString *message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCOMPATIBLE_DATA);
            peer->cleanPlayerProfiles();
            peer->sendPacket(message, true/*reliable*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby",
                "Player has incompatible tracks for new game mode.");
        }
    }
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    sendMessageToPeers(server_info);
    delete server_info;
    updatePlayerList();
}   // handleServerConfiguration

//-----------------------------------------------------------------------------
/*! \brief Called when a player want to change his handicap
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0                 1
 *       ----------------------------------
 *  Size |       1         |       1      |
 *  Data | local player id | new handicap |
 *       ----------------------------------
 */
void ServerLobby::changeHandicap(Event* event)
{
    NetworkString& data = event->data();
    if (m_state.load() != WAITING_FOR_START_GAME &&
        !event->getPeer()->isWaitingForGame())
    {
        Log::warn("ServerLobby", "Set handicap at wrong time.");
        return;
    }
    uint8_t local_id = data.getUInt8();
    auto& player = event->getPeer()->getPlayerProfiles().at(local_id);
    uint8_t handicap_id = data.getUInt8();
    if (handicap_id >= HANDICAP_COUNT)
    {
        Log::warn("ServerLobby", "Wrong handicap %d.", handicap_id);
        return;
    }
    HandicapLevel h = (HandicapLevel)handicap_id;
    player->setHandicap(h);
    updatePlayerList();
}   // changeHandicap

//-----------------------------------------------------------------------------
/** Update and see if any player disconnects, if so eliminate the kart in
 *  world, so this function must be called in main thread.
 */
void ServerLobby::handlePlayerDisconnection() const
{
    if (!World::getWorld() ||
        World::getWorld()->getPhase() < WorldStatus::MUSIC_PHASE)
    {
        return;
    }

    int red_count = 0;
    int blue_count = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        if (rki.isReserved())
            continue;
        bool disconnected = rki.disconnected();
        if (RaceManager::get()->getKartInfo(i).getKartTeam() == KART_TEAM_RED &&
            !disconnected)
            red_count++;
        else if (RaceManager::get()->getKartInfo(i).getKartTeam() ==
            KART_TEAM_BLUE && !disconnected)
            blue_count++;

        if (!disconnected)
        {
            total++;
            continue;
        }
        else
            rki.makeReserved();

        AbstractKart* k = World::getWorld()->getKart(i);
        if (!k->isEliminated() && !k->hasFinishedRace())
        {
            CaptureTheFlag* ctf = dynamic_cast<CaptureTheFlag*>
                (World::getWorld());
            if (ctf)
                ctf->loseFlagForKart(k->getWorldKartId());

            World::getWorld()->eliminateKart(i,
                false/*notify_of_elimination*/);
            if (ServerConfig::m_ranked)
            {
                // Handle disconnection earlier to prevent cheating by joining
                // another ranked server
                // Real score will be submitted later in computeNewRankings
                const uint32_t id =
                    RaceManager::get()->getKartInfo(i).getOnlineId();
                RankingEntry penalized = m_ranking->getTemporaryPenalizedScores(id);
                auto request = std::make_shared<SubmitRankingRequest>
                    (penalized,
                    RaceManager::get()->getKartInfo(i).getCountryCode());
                NetworkConfig::get()->setUserDetails(request,
                    "submit-ranking");
                request->queue();
            }
            k->setPosition(
                World::getWorld()->getCurrentNumKarts() + 1);
            k->finishedRace(World::getWorld()->getTime(), true/*from_server*/);
        }
    }

    // If live players is enabled, don't end the game if unfair team
    if (!ServerConfig::m_live_players &&
        total != 1 && World::getWorld()->hasTeam() &&
        (red_count == 0 || blue_count == 0))
        World::getWorld()->setUnfairTeam(true);

}   // handlePlayerDisconnection

//-----------------------------------------------------------------------------
/** Add reserved players for live join later if required.
 */
void ServerLobby::addLiveJoinPlaceholder(
    std::vector<std::shared_ptr<NetworkPlayerProfile> >& players) const
{
    if (!ServerConfig::m_live_players || !RaceManager::get()->supportsLiveJoining())
        return;
    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        Track* t = track_manager->getTrack(m_game_setup->getCurrentTrack());
        assert(t);
        int max_players = std::min((int)ServerConfig::m_server_max_players,
            (int)t->getMaxArenaPlayers());
        int add_size = max_players - (int)players.size();
        assert(add_size >= 0);
        for (int i = 0; i < add_size; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_NONE));
        }
    }
    else
    {
        // CTF or soccer, reserve at most 7 players on each team
        int red_count = 0;
        int blue_count = 0;
        for (unsigned i = 0; i < players.size(); i++)
        {
            if (players[i]->getTeam() == KART_TEAM_RED)
                red_count++;
            else
                blue_count++;
        }
        red_count = red_count >= 7 ? 0 : 7 - red_count;
        blue_count = blue_count >= 7 ? 0 : 7 - blue_count;
        for (int i = 0; i < red_count; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_RED));
        }
        for (int i = 0; i < blue_count; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_BLUE));
        }
    }
}   // addLiveJoinPlaceholder

//-----------------------------------------------------------------------------
void ServerLobby::setPlayerKarts(const NetworkString& ns, STKPeer* peer) const
{
    unsigned player_count = ns.getUInt8();
    for (unsigned i = 0; i < player_count; i++)
    {
        std::string kart;
        ns.decodeString(&kart);
        if (kart.find("randomkart") != std::string::npos ||
            (kart.find("addon_") == std::string::npos &&
            m_available_kts.first.find(kart) == m_available_kts.first.end()))
        {
            RandomGenerator rg;
            std::set<std::string>::iterator it =
                m_available_kts.first.begin();
            std::advance(it,
                rg.get((int)m_available_kts.first.size()));
            peer->getPlayerProfiles()[i]->setKartName(*it);
        }
        else
        {
            peer->getPlayerProfiles()[i]->setKartName(kart);
        }
    }
    if (peer->getClientCapabilities().find("real_addon_karts") ==
        peer->getClientCapabilities().end() || ns.size() == 0)
        return;
    for (unsigned i = 0; i < player_count; i++)
    {
        KartData kart_data(ns);
        std::string type = kart_data.m_kart_type;
        auto& player = peer->getPlayerProfiles()[i];
        const std::string& kart_id = player->getKartName();
        if (NetworkConfig::get()->useTuxHitboxAddon() &&
            StringUtils::startsWith(kart_id, "addon_") &&
            kart_properties_manager->hasKartTypeCharacteristic(type))
        {
            const KartProperties* real_addon =
                kart_properties_manager->getKart(kart_id);
            if (ServerConfig::m_real_addon_karts && real_addon)
            {
                kart_data = KartData(real_addon);
            }
            else
            {
                const KartProperties* tux_kp =
                    kart_properties_manager->getKart("tux");
                kart_data = KartData(tux_kp);
                kart_data.m_kart_type = type;
            }
            player->setKartData(kart_data);
        }
    }
}   // setPlayerKarts

//-----------------------------------------------------------------------------
/** Tell the client \ref RemoteKartInfo of a player when some player joining
 *  live.
 */
void ServerLobby::handleKartInfo(Event* event)
{
    World* w = World::getWorld();
    if (!w)
        return;

    STKPeer* peer = event->getPeer();
    const NetworkString& data = event->data();
    uint8_t kart_id = data.getUInt8();
    if (kart_id > RaceManager::get()->getNumPlayers())
        return;

    AbstractKart* k = w->getKart(kart_id);
    int live_join_util_ticks = k->getLiveJoinUntilTicks();

    const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(kart_id);

    NetworkString* ns = getNetworkString(1);
    ns->setSynchronous(true);
    ns->addUInt8(LE_KART_INFO).addUInt32(live_join_util_ticks)
        .addUInt8(kart_id) .encodeString(rki.getPlayerName())
        .addUInt32(rki.getHostId()).addFloat(rki.getDefaultKartColor())
        .addUInt32(rki.getOnlineId()).addUInt8(rki.getHandicap())
        .addUInt8((uint8_t)rki.getLocalPlayerId())
        .encodeString(rki.getKartName()).encodeString(rki.getCountryCode());
    if (peer->getClientCapabilities().find("real_addon_karts") !=
        peer->getClientCapabilities().end())
        rki.getKartData().encode(ns);
    peer->sendPacket(ns, true/*reliable*/);
    delete ns;
}   // handleKartInfo

//-----------------------------------------------------------------------------
/** Client if currently in-game (including spectator) wants to go back to
 *  lobby.
 */
void ServerLobby::clientInGameWantsToBackLobby(Event* event)
{
    World* w = World::getWorld();
    std::shared_ptr<STKPeer> peer = event->getPeerSP();

    if (!w || !worldIsActive() || peer->isWaitingForGame())
    {
        Log::warn("ServerLobby", "%s try to leave the game at wrong time.",
            peer->getAddress().toString().c_str());
        return;
    }

    if (m_process_type == PT_CHILD &&
        event->getPeer()->getHostId() == m_client_server_host_id.load())
    {
        // For child server the remaining client cannot go on player when the
        // server owner quited the game (because the world will be deleted), so
        // we reset all players
        auto pm = ProtocolManager::lock();
        if (RaceEventManager::get())
        {
            RaceEventManager::get()->stop();
            pm->findAndTerminate(PROTOCOL_GAME_EVENTS);
        }
        auto gp = GameProtocol::lock();
        if (gp)
        {
            auto lock = gp->acquireWorldDeletingMutex();
            pm->findAndTerminate(PROTOCOL_CONTROLLER_EVENTS);
            exitGameState();
        }
        else
            exitGameState();
        NetworkString* back_to_lobby = getNetworkString(2);
        back_to_lobby->setSynchronous(true);
        back_to_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_SERVER_ONWER_QUITED_THE_GAME);
        sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
        delete back_to_lobby;
        m_rs_state.store(RS_ASYNC_RESET);
        return;
    }

    for (const int id : peer->getAvailableKartIDs())
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        if (rki.getHostId() == peer->getHostId())
        {
            Log::info("ServerLobby", "%s left the game with kart id %d.",
                peer->getAddress().toString().c_str(), id);
            rki.setNetworkPlayerProfile(
                std::shared_ptr<NetworkPlayerProfile>());
        }
        else
        {
            Log::error("ServerLobby", "%s doesn't exist anymore in server.",
                peer->getAddress().toString().c_str());
        }
    }
    NetworkItemManager* nim = dynamic_cast<NetworkItemManager*>
        (Track::getCurrentTrack()->getItemManager());
    assert(nim);
    nim->erasePeerInGame(peer);
    m_peers_ready.erase(peer);
    peer->setWaitingForGame(true);
    peer->setSpectator(false);

    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;

    if ((m_state.load() == RACING) && (powerup_multiplier_value() == 3) && (!losing_team_weaker()))
        {
            set_powerup_multiplier(1);
            send_message("Powerupper is OFF (automatically)");
        }
    else if ((m_state.load() == RACING) && (powerup_multiplier_value() == 1) && (losing_team_weaker()) && (abs(getSoccerScoreDifference()) > 3))
        {
            set_powerup_multiplier(3);
            send_message("Powerupper is ON (automatically)");
        }

}   // clientInGameWantsToBackLobby

//-----------------------------------------------------------------------------
/** Client if currently select assets wants to go back to lobby.
 */
void ServerLobby::clientSelectingAssetsWantsToBackLobby(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();

    if (m_state.load() != SELECTING || peer->isWaitingForGame())
    {
        Log::warn("ServerLobby",
            "%s try to leave selecting assets at wrong time.",
            peer->getAddress().toString().c_str());
        return;
    }

    if (m_process_type == PT_CHILD &&
        event->getPeer()->getHostId() == m_client_server_host_id.load())
    {
        NetworkString* back_to_lobby = getNetworkString(2);
        back_to_lobby->setSynchronous(true);
        back_to_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_SERVER_ONWER_QUITED_THE_GAME);
        sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
        delete back_to_lobby;
        resetVotingTime();
        resetServer();
        m_rs_state.store(RS_NONE);
        return;
    }

    m_peers_ready.erase(peer);
    peer->setWaitingForGame(true);
    peer->setSpectator(false);

    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;
}   // clientSelectingAssetsWantsToBackLobby

std::set<std::shared_ptr<STKPeer>> ServerLobby::getSpectatorsByLimit()
{
    std::set<std::shared_ptr<STKPeer>> spectators_by_limit;

    auto peers = STKHost::get()->getPeers();
    std::set<std::shared_ptr<STKPeer>> always_spectate_peers;

    unsigned player_limit = ServerConfig::m_server_max_players;
    // If the server has an in-game player limit lower than the lobby limit, apply it,
    // A value of 0 for this parameter means no limit.
    if (ServerConfig::m_max_players_in_game > 0)
        player_limit = std::min(player_limit, (unsigned)ServerConfig::m_max_players_in_game);

    // only 10 players allowed for battle or soccer
    if (RaceManager::get()->isBattleMode() || RaceManager::get()->isSoccerMode())
        player_limit = std::min(player_limit, playerlimit);

    unsigned ingame_players = 0, waiting_players = 0, total_players = 0;
    STKHost::get()->updatePlayers(&ingame_players, &waiting_players, &total_players);
    if (total_players <= player_limit)
        return spectators_by_limit;

    std::sort(peers.begin(), peers.end(),
        [](const std::shared_ptr<STKPeer>& a,
            const std::shared_ptr<STKPeer>& b)
        { return a->getHostId() < b->getHostId(); });

    if (m_state.load() >= RACING)
    {
        for (auto &peer : peers)
            if (peer->isSpectator())
                ingame_players -= (int)peer->getPlayerProfiles().size();
    }

    unsigned player_count = 0;
    for (unsigned i = 0; i < peers.size(); i++)
    {
        auto& peer = peers[i];
        if (!peer->isValidated())
            continue;
        if (m_state.load() < RACING)
        {
            if (peer->alwaysSpectate() || peer->isWaitingForGame())
                continue;
            player_count += (unsigned)peer->getPlayerProfiles().size();
            if (player_count > player_limit)
                spectators_by_limit.insert(peer);
        }
        else
        {
            if (peer->isSpectator())
                continue;
            player_count += (unsigned)peer->getPlayerProfiles().size();
            if (peer->isWaitingForGame() && (player_count > player_limit || ingame_players >= player_limit))
                spectators_by_limit.insert(peer);
        }
    }
    return spectators_by_limit;
}

//-----------------------------------------------------------------------------
void ServerLobby::saveInitialItems(std::shared_ptr<NetworkItemManager> nim)
{
    m_items_complete_state->getBuffer().clear();
    m_items_complete_state->reset();
    nim->saveCompleteState(m_items_complete_state);
}   // saveInitialItems

//-----------------------------------------------------------------------------
bool ServerLobby::supportsAI()
{
    return getGameMode() == 3 || getGameMode() == 4;
}   // supportsAI

//-----------------------------------------------------------------------------
bool ServerLobby::checkPeersReady(bool ignore_ai_peer) const
{
    bool all_ready = true;
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
        if (!peer)
            continue;
        // Add check for always spectating
        if (peer->alwaysSpectate())
            continue;
        if (ignore_ai_peer && peer->isAIPeer())
            continue;
        all_ready = all_ready && p.second;
        if (!all_ready)
            return false;
    }
    return true;
}   // checkPeersReady

//-----------------------------------------------------------------------------
void ServerLobby::handleServerCommand(Event* event,
                                      std::shared_ptr<STKPeer> peer)
{
    SoccerWorld* sw = (SoccerWorld*)World::getWorld();

    NetworkString& data = event->data();
    std::string language;
    data.decodeString(&language);
    std::string cmd;
    data.decodeString(&cmd);
    auto argv = StringUtils::split(cmd, ' ');
    if (argv.size() == 0)
        return;
    if (argv[0] == "spectate" || argv[0] == "s")
    {
        if (m_game_setup->isGrandPrix() || !ServerConfig::m_live_players)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Server doesn't support spectate";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

        if (argv.size() != 2 || (argv[1] != "0" && argv[1] != "1") ||
            m_state.load() != WAITING_FOR_START_GAME)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Usage: spectate [0 or 1], before game started";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

        if (argv[1] == "1")
        {
            if (m_process_type == PT_CHILD &&
                peer->getHostId() == m_client_server_host_id.load())
            {
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                std::string msg = "Graphical client server cannot spectate";
                chat->encodeString16(StringUtils::utf8ToWide(msg));
                peer->sendPacket(chat, true/*reliable*/);
                delete chat;
                return;
            }
            peer->setAlwaysSpectate(ASM_COMMAND);
        }
        else
            peer->setAlwaysSpectate(ASM_NONE);
        updatePlayerList();
    }
    else if (argv[0] == "powerupper")
{
    if ((argv.size() < 3) || (argv.size() >4))
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "Usage: /powerupper [on|off] [password]";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }
    else {
        // Read the admin password from a file
        std::ifstream infile("admin_password.txt");
        std::string password;
        if (infile.good()) {
            infile >> password;
            infile.close(); // Close the file after reading
        }
        else {
            // Generate a random password and save it to the file
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(1000, 9999);
            password = std::to_string(distrib(gen));
            std::ofstream outfile("admin_password.txt");
            outfile << password;
            outfile.close();
        }

        // Check if the password matches
        if ((!argv[2].empty()) && (password == argv[2])) {
            if ((argv[1] == "on") && (powerup_multiplier_value() == 1)) {
                set_powerup_multiplier(3);
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                std::string msg = "Powerupper mode enabled \U0001f7e2";
                chat->encodeString16(StringUtils::utf8ToWide(msg));
                auto peers = STKHost::get()->getPeers();
            for (auto& p : peers) {
                p->sendPacket(chat, true /* reliable */);
            }
                delete chat;
            }
            else if (argv[1] == "off") {
                set_powerup_multiplier(1);
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                std::string msg = "Powerupper mode disabled \U0001f534";
                chat->encodeString16(StringUtils::utf8ToWide(msg));
                auto peers = STKHost::get()->getPeers();
            for (auto& p : peers) {
                p->sendPacket(chat, true /* reliable */);
            }
                delete chat;
            }
            else {
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                std::string msg = "Invalid state parameter. Usage: /powerupper [on|off]";
                chat->encodeString16(StringUtils::utf8ToWide(msg));
                peer->sendPacket(chat, true/*reliable*/);
                delete chat;
                return;
            }
        }

                else if ((argv.size() > 2) && (password != argv[2]))
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Incorrect password";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

}
}
    else if (argv[0] == "endrace")
{
    if (argv.size() != 2)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "Usage: /endrace [password]";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }
    else {
        // Read the admin password from a file
        std::ifstream infile("admin_password.txt");
        std::string password;
        if (infile.good()) {
            infile >> password;
            infile.close();
        }
        else {
            // Generate a random password and save it to the file
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(1000, 9999);
            password = std::to_string(distrib(gen));
            std::ofstream outfile("admin_password.txt");
            outfile << password;
            outfile.close();
        }

        // Check if the password matches
        if (password == argv[1]) {

        if (m_state.load() != RACING)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Use during the game, not before starting";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }


            checkRaceFinished(1);
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Game ended by an admin";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            auto peers = STKHost::get()->getPeers();
            for (auto& p : peers) {
                p->sendPacket(chat, true /* reliable */);
            }
            delete chat;
        }
        else {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Incorrect password";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
    }
}
       else if (argv[0] == "setlimit")
{
    if (argv.size() != 3)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "Usage: /setlimit [max nb of players] [password]";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }
    else {
        // Read the admin password from a file
        std::ifstream infile("admin_password.txt");
        std::string password;
        if (infile.good()) {
            infile >> password;
            infile.close();
        }
        else {
            // Generate a random password and save it to the file
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(1000, 9999);
            password = std::to_string(distrib(gen));
            std::ofstream outfile("admin_password.txt");
            outfile << password;
            outfile.close();
        }

        // Check if the password matches
        if (password == argv[2]) {
            playerlimit = std::stoi(argv[1]) > 14 ? 14 : std::stoi(argv[1]) < 0 ? 14 : std::stoi(argv[1]);
            updatePlayerList();
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Playable slots updated to " + std::to_string(playerlimit);
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            auto peers = STKHost::get()->getPeers();
            for (auto& p : peers) {
                p->sendPacket(chat, true /* reliable */);
            }
            delete chat;
        }
        else {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Incorrect password";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
    }
}
else if (argv[0] == "tip")
{
// Send a random tip when the peer finishes loading
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    std::string msg = generateRandomMessage(tips);
    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true /* reliable */);
    delete chat;
}

else if (argv[0] == "teams")
{
    int red_team_score = 0;
    int blue_team_score = 0;

    // Loop through all the peers (players)
    auto peers = STKHost::get()->getPeers();
    for (auto& player_peer : peers)
    {
        // Access the first player profile and get the player's name and team
        auto profiles = player_peer->getPlayerProfiles();
        if (!profiles.empty())
        {
            // Skip spectators
            if (player_peer->alwaysSpectate() || player_peer->isSpectator()) continue;

            std::string player_name = StringUtils::wideToUtf8(profiles[0]->getName());
            KartTeam current_team = profiles[0]->getTeam();

            // Get the player's score
            std::pair<int, int> player_info = getPlayerInfo(player_name);
            int player_score;
            // If the player_info.second (score) is negative (e.g., -1), assign 20 points
            if (player_info.second < 0)
            {
                player_score = 20; // Default score
            }
            else
            {
                player_score = player_info.second;
            }

            // Add the player's score to the appropriate team
            if (current_team == KartTeam::KART_TEAM_RED)
            {
                red_team_score += player_score;
            }
            else if (current_team == KartTeam::KART_TEAM_BLUE)
            {
                blue_team_score += player_score;
            }
        }
    }

    // Calculate the total score
    int total_score = red_team_score + blue_team_score;

    if (total_score > 0) // Avoid division by zero
    {
        // Calculate the percentage of each team
        int red_percentage = (red_team_score * 100) / total_score;
        int blue_percentage = 100 - red_percentage;

        // Calculate the number of squares for each team (rounded)
        int red_squares = (red_percentage + 5) / 10;
        int blue_squares = 10 - red_squares;

        // Create the visualization using square emojis
        std::wstring visualization;
        visualization.append(red_squares, L'\U0001F7E5');  // Red squares
        visualization.append(blue_squares, L'\U0001F7E6'); // Blue squares

        // Create the result message
        std::wstring result_msg = L"   Red " + std::to_wstring(red_percentage) + L"% - " +
                                  std::to_wstring(blue_percentage) + L"% Blue\n" + visualization;

        // Convert std::wstring to irr::core::stringw
        irr::core::stringw irr_result_msg = result_msg.c_str();

        // Send the message to the peer
        NetworkString* result = getNetworkString();
        result->addUInt8(LE_CHAT);
        result->setSynchronous(true);
        result->encodeString16(irr_result_msg);
        peer->sendPacket(result, true /* reliable */);
        delete result;
    }
    else
    {
        // Handle case where both teams have 0 score
        std::wstring result_msg = L"No scores available for either team.";
        irr::core::stringw irr_result_msg = result_msg.c_str();

        NetworkString* result = getNetworkString();
        result->addUInt8(LE_CHAT);
        result->setSynchronous(true);
        result->encodeString16(irr_result_msg);
        peer->sendPacket(result, true /* reliable */);
        delete result;
    }
}

else if ((argv[0] == "mix") || (argv[0] == "autoteams") || (argv[0] == "randomteams"))
{
    // 1) Only allow /mix before the game starts
    if (m_state.load() != WAITING_FOR_START_GAME)
    {
        send_private_message("Cannot use /mix once the game has started.", peer);
        return;
    }

    // 2) Get this player's name
    auto profiles = peer->getPlayerProfiles();
    if (profiles.empty())
    {
        send_private_message("Cannot retrieve your profile.", peer);
        return;
    }
    std::string player_name = StringUtils::wideToUtf8(profiles[0]->getName());

    // 3) Check if this player already voted
    auto it = m_mix_voters.find(player_name);
    if (it != m_mix_voters.end() && it->second)
    {
        send_private_message("You have already voted /mix.", peer);
        return;
    }

    // 4) Record the vote
    m_mix_voters[player_name] = true;

    // 5) Count how many active (non-spectator) players we have
    auto all_peers = STKHost::get()->getPeers();
    int total_active_players = 0;
    for (auto& p : all_peers)
    {
        // Skip spectators
        if (!p->alwaysSpectate() && !p->isSpectator())
            total_active_players++;
    }

    // 6) Count current votes
    int current_votes = 0;
    for (const auto &kv : m_mix_voters)
    {
        if (kv.second) current_votes++;
    }

    // 7) Broadcast the updated vote count to all
    {
        std::stringstream ss;
        ss <<"\U0001f5f3\uFE0F "<< player_name << " voted /mix. There are "
           << current_votes << " total votes.";
        send_message(ss.str());
    }

    // 8) Check if votes exceed 50% of the active players
    // For example, if total_active_players=6, we need 4 votes (4*2=8 > 6)
    if (current_votes * 2 >= total_active_players)
{
    // Enough votes -> Assign teams

    // A) Gather active players (skip spectators) + their rank & points
    struct RankedPlayer
    {
        std::shared_ptr<STKPeer> peer;
        int rank;
        int points;
    };
    std::vector<RankedPlayer> ranked_players;
    ranked_players.reserve(total_active_players);

    for (auto& p : all_peers)
    {
        if (p->alwaysSpectate() || p->isSpectator())
            continue;

        auto p_profiles = p->getPlayerProfiles();
        if (p_profiles.empty())
            continue;

        std::string p_name = StringUtils::wideToUtf8(p_profiles[0]->getName());
        auto info          = getPlayerInfo(p_name); // returns {rank, score}
        int rank_val       = (info.first  < 0) ? 0 : info.first;
        int points_val     = (info.second < 0) ? 20 : info.second;

        ranked_players.push_back({ p, rank_val, points_val });
    }

    // (Optional) sort by descending rank
    std::sort(ranked_players.begin(), ranked_players.end(),
              [](const RankedPlayer& a, const RankedPlayer& b)
              {
                  return a.rank > b.rank;
              });

    // Compute the total sum of points
    int total_sum = 0;
    for (auto &rp : ranked_players)
    {
        total_sum += rp.points;
    }

    // Prepare a vector of just the players' scores
    std::vector<int> scores;
    scores.reserve(ranked_players.size());
    for (auto &rp : ranked_players)
    {
        scores.push_back(rp.points);
    }

    // We will backtrack to find the subset that yields the closest sum to total_sum/2.
    // This uses simple recursion to generate combinations of size n/2 (and n/2+1 if odd).
    typedef std::vector<bool> BoolVec;

    // backtrack parameters:
    //  current_index: index of next player to consider
    //  selected_count: number chosen so far
    //  required_count: how many we want in the subset
    //  best_diff: track the best difference found so far
    //  scores: players' points
    //  total_sum: total sum of scores
    //  current_mask: which players are currently chosen
    //  best_mask: which players led to best_diff
    //  current_sum: sum of chosen subset so far

    std::function<void(int,int,int,int&,const std::vector<int>&,int,BoolVec&,BoolVec&,int&)> backtrack;
    backtrack = [&](int current_index,
                    int selected_count,
                    int required_count,
                    int &best_diff,
                    const std::vector<int> &scores,
                    int total_sum,
                    BoolVec &current_mask,
                    BoolVec &best_mask,
                    int &current_sum)
    {
        // If we've considered all players
        if (current_index == (int)scores.size())
        {
            // Check if we formed a valid subset
            if (selected_count == required_count)
            {
                int other_sum = total_sum - current_sum;
                int diff = std::abs(current_sum - other_sum);
                if (diff < best_diff)
                {
                    best_diff = diff;
                    best_mask = current_mask;
                }
            }
            return;
        }

        // Prune if we can't possibly fill or if we've already taken too many
        int remaining = (int)scores.size() - current_index;
        int to_fill   = required_count - selected_count;
        if (to_fill > remaining) return;             // not enough left
        if (selected_count > required_count) return; // took too many

        // Option 1: select this player
        if (selected_count < required_count)
        {
            current_mask[current_index] = true;
            current_sum += scores[current_index];

            backtrack(current_index + 1,
                      selected_count + 1,
                      required_count,
                      best_diff,
                      scores,
                      total_sum,
                      current_mask,
                      best_mask,
                      current_sum);

            // revert
            current_sum -= scores[current_index];
            current_mask[current_index] = false;
        }

        // Option 2: skip this player
        backtrack(current_index + 1,
                  selected_count,
                  required_count,
                  best_diff,
                  scores,
                  total_sum,
                  current_mask,
                  best_mask,
                  current_sum);
    };

    int n = (int)ranked_players.size();
    int half1 = n / 2;
    int half2 = (n % 2 == 1) ? (half1 + 1) : half1;

    int best_diff = INT_MAX;
    BoolVec best_mask(n, false);

    // Try subsets of size half1 and half2 (covers even/odd cases)
    for (int required_count : { half1, half2 })
    {
        int cur_sum = 0;
        BoolVec cur_mask(n, false);

        backtrack(/*current_index*/    0,
                  /*selected_count*/   0,
                  /*required_count*/   required_count,
                  /*best_diff*/        best_diff,
                  /*scores*/           scores,
                  /*total_sum*/        total_sum,
                  /*current_mask*/     cur_mask,
                  /*best_mask*/        best_mask,
                  /*current_sum*/      cur_sum);
    }

    // Decide randomly whether best_mask indicates RED or BLUE
    bool start_red = (std::rand() % 2 == 0);

    // Calculate final sums and apply teams
    int sum_points_red  = 0;
    int sum_points_blue = 0;

    for (int i = 0; i < n; i++)
    {
        // If best_mask[i] == true => place on subset team
        bool use_red = best_mask[i] ? start_red : !start_red;
        KartTeam new_team = use_red ? KART_TEAM_RED : KART_TEAM_BLUE;

        auto &profiles = ranked_players[i].peer->getPlayerProfiles();
        if (!profiles.empty())
        {
            profiles[0]->setTeam(new_team);
        }

        // Update sums
        if (new_team == KART_TEAM_RED)
        {
            sum_points_red += scores[i];
        }
        else
        {
            sum_points_blue += scores[i];
        }
    }



    // Send a message with final team distribution and difference
    send_message("Teams have been mixed by closest total scores!");

    // --------------------------------------------------
    // Report Red/Blue distribution using squares & %:
    // --------------------------------------------------
    int total_score = sum_points_red + sum_points_blue;
    Log::warn("ServerLobby", "total_score ="+ sum_points_red);
    if (total_score > 0) // Avoid division by zero
    {
        int red_percentage  = (sum_points_red  * 100) / total_score;
        int blue_percentage = 100 - red_percentage;

        int red_squares  = (red_percentage  + 5) / 10;  // rounding
        int blue_squares = 10 - red_squares;            // total squares: 10

        // Build a wide string visualization
        std::wstring visualization;
        // Red squares (U+1F7E5), then blue squares (U+1F7E6)
       // Create the visualization using square emojis
        visualization.append(red_squares, L'\U0001F7E5');  // Red squares
        visualization.append(blue_squares, L'\U0001F7E6'); // Blue squares

        // Create the result message
        std::wstring result_msg = L"   Red " + std::to_wstring(red_percentage) + L"% - " +
                                  std::to_wstring(blue_percentage) + L"% Blue\n" + visualization;
        std::string result_msg_str( result_msg.begin(), result_msg.end() );

        // Convert std::wstring to irr::core::stringw
        irr::core::stringw irr_result_msg = result_msg.c_str();

        // Send the message to the peer
        NetworkString* result = getNetworkString();
        result->addUInt8(LE_CHAT);
        result->setSynchronous(true);
        result->encodeString16(irr_result_msg);
        auto peers = STKHost::get()->getPeers();
        for (auto& p : peers)
            p->sendPacket(result, true /* reliable */);
        delete result;

    }
    // Update final assignments
    updatePlayerList();
    // Clear votes so we can't /mix again immediately
    m_mix_voters.clear();
}
}



else if (argv[0] == "rank")
    {
        if (argv.size() < 2)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Usage: /rank [Partial or full player name]";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true /* reliable */);
            delete chat;
            return;
        }
        else
        {
            // Attempt to retrieve player info (with partial name matching)
            PlayerStats stats;
            bool found = getPlayerFromFile("soccer_ranking_detailed.txt", argv[1], stats);

            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);

            if (!found)
            {
                // No ranking found
                std::string msg = "No ranking found for this Player!\nPlayer needs to play at least 30 minutes and 8 matches with others to get a rank";
                chat->encodeString16(StringUtils::utf8ToWide(msg));
            }
            else
            {
                // Calculate the desired stats
                double goalsPerMatch = 0.0;
                double savesPerMatch = 0.0;
                double winPercentage = 0.0;
                int attackPct = 0;
                int defensePct = 0;

                if (stats.minutes_played_count > 0)
                {
                    goalsPerMatch = static_cast<double>(stats.scoringPoints);//*stats.minutes_played_count/minutes_dur) / stats.matchesPlayed;
                    savesPerMatch = static_cast<double>(stats.defendingPoints);//*stats.minutes_played_count/minutes_dur) / stats.matchesPlayed;
                    winPercentage = (static_cast<double>(stats.matchesWon) / stats.matches_participated) * 100.0;
                }

                // Compute integer attack / defense percentages
                float totalAttackDefense = stats.attackingPoints + stats.defendingPoints;
                if (totalAttackDefense > 0.0f)
                {
                    attackPct = (stats.attackingPoints * 100) / totalAttackDefense;
                    defensePct = 100 - attackPct;
                }

                // Build the message
                std::ostringstream msgStream;
                msgStream << "-------------------------------------- " << "\n";
                msgStream << "Player: " << stats.name << "\n";
                msgStream << "Rank: " << stats.rank << "\n";
                msgStream << "Points: " << stats.total << "\n";
                msgStream << "goals/match: " << std::round(goalsPerMatch* 100.0f) / 100.0f << "\n";
                msgStream << "saves/match: " << std::round(savesPerMatch* 100.0f) / 100.0f << "\n";
                msgStream << "win percentage: " << winPercentage << "%\n";
                msgStream << "Attack " << attackPct << "% : " << defensePct << "% Defense \n";
                msgStream << "-------------------------------------- ";

                chat->encodeString16(StringUtils::utf8ToWide(msgStream.str()));
            }

            peer->sendPacket(chat, true /* reliable */);
            delete chat;
        }
    }



else if (argv[0] == "top")
{
    // Parse the count 'n' from the command - set to 8 if not provided
    int count = (argv.size() > 1) ? std::stoi(argv[1]) : 8;

    // Create a vector of tuples
    std::vector<std::tuple<int, std::string, float>> players;

    // Read from the soccer_ranking.txt file
    std::ifstream file("soccer_ranking.txt");
    std::string line;
    int i =0;
    while(std::getline(file, line) && i<count){
        std::istringstream iss(line);
        int rank;
        std::string name;
        float score;
        if(!(iss >> rank >> name >> score)) { continue; }
        players.push_back(std::make_tuple(rank, name, score));
        i++;
    }
    file.close();

    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    std::string msg = "Rank        Player        Points\n";

    // Loop through the top 'n' scores
    for (const auto& player : players) {
        msg += "  #" + std::to_string(std::get<0>(player)) + "             ";
        msg += std::get<1>(player) + "            ";

        // Format score to 2 decimal places
        std::ostringstream score_stream;
        score_stream << std::fixed << std::setprecision(2) << std::get<2>(player);
        msg += score_stream.str() + "\n";
    }


    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true /* reliable */);
    delete chat;
}

else if (argv[0] == "score")
{
        if (m_state.load() != RACING)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "No on-going game!";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }


        const int red_score = sw->getScore(KART_TEAM_RED);
        const int blue_score = sw->getScore(KART_TEAM_BLUE);

        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "\U0001f7e5 Red " + std::to_string(red_score)+ " : " + std::to_string(blue_score) + " Blue \U0001f7e6";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;

}
else if (argv[0] == "poss")
{
        if (m_state.load() != RACING)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "No on-going game!";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

        auto poss = sw->getBallPossession();
        int red_possession  = poss.first;
        int blue_possession = poss.second;

        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "\U0001f7e5 Red " + std::to_string(red_possession)+ "% : " + std::to_string(blue_possession) + "% Blue \U0001f7e6";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;

}

else if (argv[0] == "bye")
{
        if (m_state.load() == RACING || m_state.load() == SELECTING)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "use in lobby only when no on-going match!";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
        std::string name = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = name +":\n╔═╗   ║   ║╔═ ║\n╠═╝╗╚═║╠═ ║\n╚══╝   ╚╝╚═ O";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        auto peers = STKHost::get()->getPeers();
        for (auto& p : peers) {
            p->sendPacket(chat, true /* reliable */);
        }
        delete chat;

}
else if ((argv[0] == "changeteam") || (argv[0] == "ct"))
{
    if (argv.size() != 3)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "Usage: /changeteam [player name] [password]";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true /* reliable */);
        delete chat;
        return;
    }
    else
    {
        // Read the admin password from a file
        std::ifstream infile("admin_password.txt");
        std::string password;
        if (infile.good()) {
            infile >> password;
            infile.close();
        }
        else {
            // Generate a random password and save it to the file
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(1000, 9999);
            password = std::to_string(distrib(gen));
            std::ofstream outfile("admin_password.txt");
            outfile << password;
            outfile.close();
        }

        // Check if the password matches
        if (password == argv[2])
        {
            std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(argv[1]));

            if (!player_peer || player_peer->isAIPeer())
            {
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                chat->encodeString16(
                    L"Player not found or cannot change team");
                peer->sendPacket(chat, true /* reliable */);
                delete chat;
            }
            else
            {
                KartTeam current_team = player_peer->getPlayerProfiles()[0]->getTeam();

                if (current_team == KART_TEAM_RED)
                {
                    player_peer->getPlayerProfiles()[0]->setTeam(KART_TEAM_BLUE);
                }
                else if (current_team == KART_TEAM_BLUE)
                {
                    player_peer->getPlayerProfiles()[0]->setTeam(KART_TEAM_RED);
                }

                updatePlayerList();
            }
        }
        else
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                L"Incorrect password");
            peer->sendPacket(chat, true /* reliable */);
            delete chat;
        }
    }
}

else if ((argv[0] == "statall") || (argv[0] == "stata"))
{
    // By default, display 9 lines. If the user types '/statsall 10',
    // we display 11 lines (the user-specified number + 1).
    int linesToRead = 9; // default
    if (argv.size() > 1) // user provided a second argument
    {
        int requested = std::stoi(argv[1]);
        linesToRead = requested + 1;
    }

    // We will read from "colallstats.txt" line by line
    std::ifstream file("colallstats.txt");
    if (!file.is_open())
    {
        // If the file can’t be opened or doesn’t exist, send an error message
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);

        std::string err = "Error: Cannot open 'colallstats.txt' for reading.";
        chat->encodeString16(StringUtils::utf8ToWide(err));
        peer->sendPacket(chat, true /* reliable */);

        delete chat;
        return;
    }

    // Collect lines and build a single message string
    std::string msg;
    std::string line;
    int count = 0;
    while (std::getline(file, line) && count < linesToRead)
    {
        msg += line + "\n";
        count++;
    }
    file.close();

    // Send the collected lines to chat
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);

    // Put the message in chat
    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true /* reliable */);

    delete chat;
}

else if (argv[0] == "statg")
{
    // By default, display 10 lines. If the user types '/statsall 10',
    // we display 11 lines (the user-specified number + 1).
    int linesToRead = 10; // default
    if (argv.size() > 1) // user provided a second argument
    {
        int requested = std::stoi(argv[1]);
        linesToRead = requested + 1;
    }

    // We will read from "collastmatchstats.txt" line by line
    std::ifstream file("collastmatchstats.txt");
    if (!file.is_open())
    {
        // If the file can’t be opened or doesn’t exist, send an error message
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);

        std::string err = "Error: Cannot open 'collastmatchstats.txt' for reading.";
        chat->encodeString16(StringUtils::utf8ToWide(err));
        peer->sendPacket(chat, true /* reliable */);

        delete chat;
        return;
    }

    // Collect lines and build a single message string
    std::string msg;
    std::string line;
    int count = 0;
    while (std::getline(file, line) && count < linesToRead)
    {
        msg += line + "\n";
        count++;
    }
    file.close();

    // Send the collected lines to chat
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);

    // Put the message in chat
    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true /* reliable */);

    delete chat;
}
       else if (argv[0] == "help")
    {
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);

    std::ifstream file("help.txt");
    std::stringstream buffer;

    if (!file.is_open()) // check if file is not found
    {
        std::ofstream new_file("help.txt"); // create new file
        new_file << "Help content is currently unavialable. Ask the owner to add it."; // write content to file
        new_file.close(); // close file
        file.open("help.txt"); // reopen file to read content
    }

    buffer << file.rdbuf();
    std::string msg = buffer.str();

    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true/*reliable*/);

    file.close(); // close the file
    delete chat;
    return;
    }

else if (argv[0] == "alldata")
    {
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);

    std::ifstream file("db_table_copy.txt");
    std::stringstream buffer;

    if (!file.is_open()) // check if file is not found
    {
        std::ofstream new_file("db_table_copy.txt"); // create new file
        new_file << "No data avialable"; // write content to file
        new_file.close(); // close file
        file.open("db_table_copy.txt"); // reopen file to read content
    }

    buffer << file.rdbuf();
    std::string msg = buffer.str();

    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true/*reliable*/);

    file.close(); // close the file
    delete chat;
    return;
    }

else if (argv[0] == "stats")
    {
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);

    std::ifstream file("last_match_stats.txt");
    std::stringstream buffer;

    if (!file.is_open()) // check if file is not found
    {
        std::ofstream new_file("last_match_stats.txt"); // create new file
        new_file << "No stats avialable"; // write content to file
        new_file.close(); // close file
        file.open("last_match_stats.txt"); // reopen file to read content
    }

    buffer << file.rdbuf();
    std::string msg = buffer.str();

    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true/*reliable*/);

    file.close(); // close the file
    delete chat;
    return;
    }

    else if (argv[0] == "new")
    {
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);

    std::ifstream file("new.txt");
    std::stringstream buffer;

    if (!file.is_open()) // check if file is not found
    {
        std::ofstream new_file("new.txt"); // create new file
        new_file << "new content is currently unavialable. Ask the owner to add it."; // write content to file
        new_file.close(); // close file
        file.open("new.txt"); // reopen file to read content
    }

    buffer << file.rdbuf();
    std::string msg = buffer.str();

    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true/*reliable*/);

    file.close(); // close the file
    delete chat;
    return;
    }

    else if (argv[0] == "about")
    {
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);

    std::ifstream file("about.txt");
    std::stringstream buffer;

    if (!file.is_open()) // check if file is not found
    {
        std::ofstream new_file("about.txt"); // create new file
        new_file << "about content is currently unavialable. Ask the owner to add it."; // write content to file
        new_file.close(); // close file
        file.open("about.txt"); // reopen file to read content
    }

    buffer << file.rdbuf();
    std::string msg = buffer.str();

    chat->encodeString16(StringUtils::utf8ToWide(msg));
    peer->sendPacket(chat, true/*reliable*/);

    file.close(); // close the file
    delete chat;
    return;
    }

    else if (argv[0] == "listserveraddon")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        bool has_options = argv.size() > 1 &&
            (argv[1].compare("-track") == 0 ||
            argv[1].compare("-arena") == 0 ||
            argv[1].compare("-kart") == 0 ||
            argv[1].compare("-soccer") == 0);
        if (argv.size() == 1 || argv.size() > 3 || argv[1].size() < 3 ||
            (argv.size() == 2 && (argv[1].size() < 3 || has_options)) ||
            (argv.size() == 3 && (!has_options || argv[2].size() < 3)))
        {
            chat->encodeString16(
                L"Usage: /listserveraddon [option][addon string to find "
                "(at least 3 characters)]. Available options: "
                "-track, -arena, -kart, -soccer.");
        }
        else
        {
            std::string type = "";
            std::string text = "";
            if(argv.size() > 1)
            {
                if(argv[1].compare("-track") == 0 ||
                   argv[1].compare("-arena") == 0 ||
                   argv[1].compare("-kart" ) == 0 ||
                   argv[1].compare("-soccer" ) == 0)
                    type = argv[1].substr(1);
                if((argv.size() == 2 && type.empty()) || argv.size() == 3)
                    text = argv[argv.size()-1];
            }

            std::set<std::string> total_addons;
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("kart") == 0)) // list kart addon
            {
                total_addons.insert(m_addon_kts.first.begin(), m_addon_kts.first.end());
            }
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("track") == 0))
            {
                total_addons.insert(m_addon_kts.second.begin(), m_addon_kts.second.end());
            }
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("arena") == 0))
            {
                total_addons.insert(m_addon_arenas.begin(), m_addon_arenas.end());
            }
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("soccer") == 0))
            {
                total_addons.insert(m_addon_soccers.begin(), m_addon_soccers.end());
            }
            std::string msg = "";
            for (auto& addon : total_addons)
            {
                // addon_ (6 letters)
                if (!text.empty() && addon.find(text, 6) == std::string::npos)
                    continue;

                msg += addon.substr(6);
                msg += ", ";
            }
            if (msg.empty())
                chat->encodeString16(L"Addon not found");
            else
            {
                msg = msg.substr(0, msg.size() - 2);
                chat->encodeString16(StringUtils::utf8ToWide(
                    std::string("Server addon: ") + msg));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (StringUtils::startsWith(cmd, "playerhasaddon"))
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string part;
        if (cmd.length() > 15)
            part = cmd.substr(15);
        std::string addon_id = part.substr(0, part.find(' '));
        std::string player_name;
        if (part.length() > addon_id.length() + 1)
            player_name = part.substr(addon_id.length() + 1);
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name));
        if (player_name.empty() || !player_peer || addon_id.empty())
        {
            chat->encodeString16(
                L"Usage: /playerhasaddon [addon_identity] [player name]");
        }
        else
        {
            std::string addon_id_test = Addon::createAddonId(addon_id);
            bool found = false;
            const auto& kt = player_peer->getClientAssets();
            for (auto& kart : kt.first)
            {
                if (kart == addon_id_test)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                for (auto& track : kt.second)
                {
                    if (track == addon_id_test)
                    {
                        found = true;
                        break;
                    }
                }
            }
            if (found)
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has addon " + addon_id));
            }
            else
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has no addon " + addon_id));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (StringUtils::startsWith(cmd, "kick"))
{
    std::vector<std::string> argv = StringUtils::split(cmd, ' ');
    if (argv.size() != 3)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "Usage: /kick [player name] [password]";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true /* reliable */);
        delete chat;
        return;
    }
    else
    {
        // Read the admin password from a file
        std::ifstream infile("admin_password.txt");
        std::string password;
        if (infile.good()) {
            infile >> password;
            infile.close();
        }
        else {
            // Generate a random password and save it to the file
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(1000, 9999);
            password = std::to_string(distrib(gen));
            std::ofstream outfile("admin_password.txt");
            outfile << password;
            outfile.close();
        }

        // Check if the password matches
        if (password == argv[2])
        {
            std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(argv[1]));
            if (!player_peer || player_peer->isAIPeer())
            {
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                chat->encodeString16(
                    L"Player not found or cannot be kicked");
                peer->sendPacket(chat, true /* reliable */);
                delete chat;
            }
            else
            {
                player_peer->kick();
            }
        }
        else
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                L"Incorrect password");
            peer->sendPacket(chat, true /* reliable */);
            delete chat;
        }
    }
}
    else if (StringUtils::startsWith(cmd, "playeraddonscore"))
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string player_name;
        if (cmd.length() > 17)
            player_name = cmd.substr(17);
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name));
        if (player_name.empty() || !player_peer)
        {
            chat->encodeString16(
                L"Usage: /playeraddonscore [player name] (return 0-100)");
        }
        else
        {
            auto& scores = player_peer->getAddonsScores();
            if (scores[AS_KART] == -1 && scores[AS_TRACK] == -1 &&
                scores[AS_ARENA] == -1 && scores[AS_SOCCER] == -1)
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has no addon"));
            }
            else
            {
                std::string msg = player_name;
                msg += " addon:";
                if (scores[AS_KART] != -1)
                    msg += " kart: " + StringUtils::toString(scores[AS_KART]) + ",";
                if (scores[AS_TRACK] != -1)
                    msg += " track: " + StringUtils::toString(scores[AS_TRACK]) + ",";
                if (scores[AS_ARENA] != -1)
                    msg += " arena: " + StringUtils::toString(scores[AS_ARENA]) + ",";
                if (scores[AS_SOCCER] != -1)
                    msg += " soccer: " + StringUtils::toString(scores[AS_SOCCER]) + ",";
                msg = msg.substr(0, msg.size() - 1);
                chat->encodeString16(StringUtils::utf8ToWide(msg));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (argv[0] == "serverhasaddon")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        if (argv.size() != 2)
        {
            chat->encodeString16(
                L"Usage: /serverhasaddon [addon_identity]");
        }
        else
        {
            std::set<std::string> total_addons;
            total_addons.insert(m_addon_kts.first.begin(), m_addon_kts.first.end());
            total_addons.insert(m_addon_kts.second.begin(), m_addon_kts.second.end());
            total_addons.insert(m_addon_arenas.begin(), m_addon_arenas.end());
            total_addons.insert(m_addon_soccers.begin(), m_addon_soccers.end());
            std::string addon_id_test = Addon::createAddonId(argv[1]);
            bool found = total_addons.find(addon_id_test) != total_addons.end();
            if (found)
            {
                chat->encodeString16(StringUtils::utf8ToWide(std::string
                    ("Server has addon ") + argv[1]));
            }
            else
            {
                chat->encodeString16(StringUtils::utf8ToWide(std::string
                    ("Server has no addon ") + argv[1]));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (argv[0] == "mute")
    {
        std::shared_ptr<STKPeer> player_peer;
        std::string result_msg;
        core::stringw player_name;
        NetworkString* result = NULL;

        if (argv.size() != 2 || argv[1].empty())
            goto mute_error;

        player_name = StringUtils::utf8ToWide(argv[1]);
        player_peer = STKHost::get()->findPeerByName(player_name);

        if (!player_peer || player_peer == peer)
            goto mute_error;

        m_peers_muted_players[peer].insert(player_name);
        result = getNetworkString();
        result->addUInt8(LE_CHAT);
        result->setSynchronous(true);
        result_msg = "Muted player ";
        result_msg += argv[1];
        result->encodeString16(StringUtils::utf8ToWide(result_msg));
        peer->sendPacket(result, true/*reliable*/);
        delete result;
        return;

mute_error:
        NetworkString* error = getNetworkString();
        error->addUInt8(LE_CHAT);
        error->setSynchronous(true);
        std::string msg = "Usage: /mute player_name (not including yourself)";
        error->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(error, true/*reliable*/);
        delete error;
    }
    else if (argv[0] == "unmute")
    {
        std::shared_ptr<STKPeer> player_peer;
        std::string result_msg;
        core::stringw player_name;
        NetworkString* result = NULL;

        if (argv.size() != 2 || argv[1].empty())
            goto unmute_error;

        player_name = StringUtils::utf8ToWide(argv[1]);
        for (auto it = m_peers_muted_players[peer].begin();
            it != m_peers_muted_players[peer].end();)
        {
            if (*it == player_name)
            {
                it = m_peers_muted_players[peer].erase(it);
                goto unmute_found;
            }
            else
            {
                it++;
            }
        }
        goto unmute_error;

unmute_found:
        result = getNetworkString();
        result->addUInt8(LE_CHAT);
        result->setSynchronous(true);
        result_msg = "Unmuted player ";
        result_msg += argv[1];
        result->encodeString16(StringUtils::utf8ToWide(result_msg));
        peer->sendPacket(result, true/*reliable*/);
        delete result;
        return;

unmute_error:
        NetworkString* error = getNetworkString();
        error->addUInt8(LE_CHAT);
        error->setSynchronous(true);
        std::string msg = "Usage: /unmute player_name";
        error->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(error, true/*reliable*/);
        delete error;
    }
    else if (argv[0] == "listmute")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        core::stringw total;
        for (auto& name : m_peers_muted_players[peer])
        {
            total += name;
            total += " ";
        }
        if (total.empty())
            chat->encodeString16("No player has been muted by you");
        else
        {
            total += "muted";
            chat->encodeString16(total);
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "Unknown command: ";
        msg += cmd;
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
}   // handleServerCommand

//-----------------------------------------------------------------------------
bool ServerLobby::playerReportsTableExists() const
{
#ifdef ENABLE_SQLITE3
    return m_db_connector->hasPlayerReportsTable();
#else
    return false;
#endif
}

void ServerLobby::send_message(std::string msg)
{

NetworkString* chat = getNetworkString();
chat->addUInt8(LE_CHAT);
chat->setSynchronous(true);
chat->encodeString16(StringUtils::utf8ToWide(msg));
auto peers = STKHost::get()->getPeers();
for (auto& p : peers)
    p->sendPacket(chat, true /* reliable */);
delete chat;

}

void ServerLobby::send_private_message(const std::string &msg, std::shared_ptr<STKPeer> target_peer)
{
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    chat->encodeString16(StringUtils::utf8ToWide(msg));

    // Send only to the specified peer
    target_peer->sendPacket(chat, true /* reliable */);

    delete chat;
}

bool ServerLobby::losing_team_weaker()
{
    // 1) Must be racing
    if (m_state.load() != RACING)
    {
        return false;
    }

    // 2) Sum the ranking points of each team,
    //    ignoring spectators or players still waiting for the game.
    int red_team_points  = 0;
    int blue_team_points = 0;

    // Gather all peers
    auto peers = STKHost::get()->getPeers();
    for (auto& player_peer : peers)
    {
        // Skip spectators and players waiting for game
        if (player_peer->alwaysSpectate() || player_peer->isSpectator())  continue;
        if (player_peer->isWaitingForGame())                               continue;

        // Get the slot's first profile
        auto profiles = player_peer->getPlayerProfiles();
        if (!profiles.empty())
        {
            std::string player_name = StringUtils::wideToUtf8(profiles[0]->getName());
            KartTeam    current_team = profiles[0]->getTeam();

            // Retrieve player's ranking info
            std::pair<int, int> player_info = getPlayerInfo(player_name);
            int player_score = (player_info.second < 0) ? 20 : player_info.second;

            // Tally scores by team
            if (current_team == KART_TEAM_RED)
                red_team_points += player_score;
            else if (current_team == KART_TEAM_BLUE)
                blue_team_points += player_score;
        }
    }

    // 3) Check current SoccerWorld scores to see which team is losing
    SoccerWorld* sw = (SoccerWorld*)World::getWorld();
    if (!sw)  // Safety check
    {
        return false;
    }
    const int red_score  = sw->getScore(KART_TEAM_RED);
    const int blue_score = sw->getScore(KART_TEAM_BLUE);

    // If both teams are tied, there's no "losing" team
    if (red_score == blue_score) return false;

    // Identify losing team based on SoccerWorld's scoreboard
    bool red_is_losing = (red_score < blue_score);
    // If red is losing but also has fewer total ranking points => true
    if (red_is_losing && red_team_points < blue_team_points)
    {
        return true;
    }
    // If blue is losing but also has fewer total ranking points => true
    bool blue_is_losing = (blue_score < red_score);
    if (blue_is_losing && blue_team_points < red_team_points)
    {
        return true;
    }

    // Otherwise, losing team's total ranking points are not strictly less
    return false;
}

int ServerLobby::getSoccerScoreDifference() const
{
    // Ensure the game is in RACING state
    if (m_state.load() != RACING)
    {
        return 0;
    }

    // Retrieve the SoccerWorld
    SoccerWorld* sw = dynamic_cast<SoccerWorld*>(World::getWorld());
    if (!sw)
    {
        return 0;
    }

    // Get each team's current score
    int red_score  = sw->getScore(KART_TEAM_RED);
    int blue_score = sw->getScore(KART_TEAM_BLUE);

    // Return the difference (red minus blue)
    return red_score - blue_score;
}
