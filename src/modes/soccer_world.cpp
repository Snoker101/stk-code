//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2006-2015 SuperTuxKart-Team
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

#include "modes/soccer_world.hpp"

#include "main_loop.hpp"
#include "audio/music_manager.hpp"
#include "audio/sfx_base.hpp"
#include "config/user_config.hpp"
#include "io/file_manager.hpp"
#include "items/powerup_manager.hpp"
#include "graphics/irr_driver.hpp"
#include "karts/abstract_kart_animation.hpp"
#include "karts/kart_model.hpp"
#include "karts/kart_properties.hpp"
#include "karts/controller/local_player_controller.hpp"
#include "karts/controller/network_player_controller.hpp"
#include "network/network_config.hpp"
#include "network/network_string.hpp"
#include "network/protocols/game_events_protocol.hpp"
#include "network/protocols/server_lobby.hpp"
#include "network/rewind_manager.hpp"
#include "network/stk_host.hpp"
#include "network/stk_peer.hpp"
#include "physics/physics.hpp"
#include "states_screens/race_gui_base.hpp"
#include "tracks/check_goal.hpp"
#include "tracks/check_manager.hpp"
#include "tracks/graph.hpp"
#include "tracks/quad.hpp"
#include "tracks/track.hpp"
#include "tracks/track_object_manager.hpp"
#include "tracks/track_sector.hpp"
#include "utils/constants.hpp"
#include "utils/translation.hpp"
#include "utils/string_utils.hpp"

#include <IMeshSceneNode.h>
#include <numeric>
#include <string>
#include <fstream>
#include <algorithm> // For std::sort
#include <sstream>
#include <map>


#include <cassert>
#include <iostream>
#include <vector>
#include <ctime>
#include <cstdio>       // for rename()
#include <dirent.h>     // for opendir(), readdir(), closedir()

#include "sqlite3.h" //  You will have to install the package libsqlite3-dev. For ubuntu: "sudo apt install libsqlite3-dev"
                    // and build game with sqlite3 on: "cmake .. -DNO_SHADERC=on ENABLE_SQLITE3"

//=============================================================================
namespace
{
    // A typed function-pointer approach for retrieving a particular integer field.
    // Example usage: findTopInField(m_kart_scores, &getTotalPts)

    typedef int(*FieldGetter)(const KartScore&);

    // These "getter" functions return whichever field we want.
    int getTotalPts   (const KartScore &k) { return k.total_pts;    }
    int getAttacking  (const KartScore &k) { return k.attacking_pts;}
    int getDefending  (const KartScore &k) { return k.defending_pts + k.inddefending_pts;}
    int getScoring    (const KartScore &k) { return k.scoring_pts;  }

    // This function scans all `KartScore` objects, finds the maximum value
    // for a given field (determined by `getter`), and returns a pair:
    // (max_value, vector_of_names_that_share_max). If the `max_value` ≤ 0,
    // "none" is interpreted.
    std::pair<int, std::vector<std::string>>
    findTopInField(const std::vector<KartScore>& scores, FieldGetter getter)
    {
        int max_val = std::numeric_limits<int>::min();

        // 1) Find the maximum value
        for (size_t i = 0; i < scores.size(); i++)
        {
            if (scores[i].m_name.empty()) continue;
            int val = getter(scores[i]);
            if (val > max_val) max_val = val;
        }

        // 2) Gather names if `max_val > 0`
        std::vector<std::string> top_names;
        if (max_val > 0)
        {
            for (size_t i = 0; i < scores.size(); i++)
            {
                if (scores[i].m_name.empty()) continue;
                if (getter(scores[i]) == max_val)
                    top_names.push_back(scores[i].m_name);
            }
        }

        return std::make_pair(max_val, top_names);
    }
} // end anonymous namespace

class BallGoalData
{
// These data are used by AI to determine ball aiming angle
private:
    // Radius of the ball
    float m_radius;

    // Slope of the line from ball to the center point of goals
    float m_red_goal_slope;
    float m_blue_goal_slope;

    // The transform only takes the ball heading into account,
    // ie no hpr of ball which allowing setting aim point easier
    btTransform m_trans;

    // Two goals
    CheckGoal* m_blue_check_goal;
    CheckGoal* m_red_check_goal;

    // Location to red/blue goal points from the ball heading point of view
    Vec3 m_red_goal_1;
    Vec3 m_red_goal_2;
    Vec3 m_red_goal_3;
    Vec3 m_blue_goal_1;
    Vec3 m_blue_goal_2;
    Vec3 m_blue_goal_3;
public:
    void reset()
    {
        m_red_goal_1 = Vec3(0, 0, 0);
        m_red_goal_2 = Vec3(0, 0, 0);
        m_red_goal_3 = Vec3(0, 0, 0);
        m_blue_goal_1 = Vec3(0, 0, 0);
        m_blue_goal_2 = Vec3(0, 0, 0);
        m_blue_goal_3 = Vec3(0, 0, 0);
        m_red_goal_slope = 1.0f;
        m_blue_goal_slope = 1.0f;
        m_trans = btTransform(btQuaternion(0, 0, 0, 1), Vec3(0, 0, 0));
    }   // reset

    float getDiameter() const
    {
        return m_radius * 2;
    }   // getDiameter

    void init(float ball_radius)
    {
        m_radius = ball_radius;
        assert(m_radius > 0.0f);

        // Save two goals
        CheckManager* cm = Track::getCurrentTrack()->getCheckManager();
        unsigned int n = cm->getCheckStructureCount();
        for (unsigned int i = 0; i < n; i++)
        {
            CheckGoal* goal = dynamic_cast<CheckGoal*>
                (cm->getCheckStructure(i));
            if (goal)
            {
                if (goal->getTeam())
                    m_blue_check_goal = goal;
                else
                    m_red_check_goal = goal;
            }
        }
        if (m_blue_check_goal == NULL || m_red_check_goal == NULL)
        {
            Log::error("SoccerWorld", "Goal(s) is missing!");
        }
    }   // init

    void updateBallAndGoal(const Vec3& ball_pos, float heading)
    {
        btQuaternion quat(Vec3(0, 1, 0), -heading);
        m_trans = btTransform(btQuaternion(Vec3(0, 1, 0), heading),
            ball_pos);

        // Red goal
        m_red_goal_1 = quatRotate(quat, m_red_check_goal
            ->getPoint(CheckGoal::POINT_FIRST) - ball_pos);
        m_red_goal_2 = quatRotate(quat, m_red_check_goal
            ->getPoint(CheckGoal::POINT_CENTER) - ball_pos);
        m_red_goal_3 = quatRotate(quat, m_red_check_goal
            ->getPoint(CheckGoal::POINT_LAST) - ball_pos);

        // Blue goal
        m_blue_goal_1 = quatRotate(quat, m_blue_check_goal
            ->getPoint(CheckGoal::POINT_FIRST) - ball_pos);
        m_blue_goal_2 = quatRotate(quat, m_blue_check_goal
            ->getPoint(CheckGoal::POINT_CENTER) - ball_pos);
        m_blue_goal_3 = quatRotate(quat, m_blue_check_goal
            ->getPoint(CheckGoal::POINT_LAST) - ball_pos);

        // Update the slope:
        // Use y = mx + c as an equation from goal center to ball
        // As the line always intercept in (0,0) which is the ball location,
        // so y(z)/x is the slope , it is used for determine aiming position
        // of ball later
        m_red_goal_slope = m_red_goal_2.z() / m_red_goal_2.x();
        m_blue_goal_slope = m_blue_goal_2.z() / m_blue_goal_2.x();
    }   // updateBallAndGoal

    bool isApproachingGoal(KartTeam team) const
    {
        // If the ball lies between the first and last pos, and faces
        // in front of either of them, (inside angular size of goal)
        // than it's likely to goal
        if (team == KART_TEAM_BLUE)
        {
            if ((m_blue_goal_1.z() > 0.0f || m_blue_goal_3.z() > 0.0f) &&
                ((m_blue_goal_1.x() < 0.0f && m_blue_goal_3.x() > 0.0f) ||
                (m_blue_goal_3.x() < 0.0f && m_blue_goal_1.x() > 0.0f)))
                return true;
        }
        else
        {
            if ((m_red_goal_1.z() > 0.0f || m_red_goal_3.z() > 0.0f) &&
                ((m_red_goal_1.x() < 0.0f && m_red_goal_3.x() > 0.0f) ||
                (m_red_goal_3.x() < 0.0f && m_red_goal_1.x() > 0.0f)))
                return true;
        }
        return false;
    }   // isApproachingGoal

    Vec3 getAimPosition(KartTeam team, bool reverse) const
    {
        // If it's likely to goal already, aim the ball straight behind
        // should do the job
        if (isApproachingGoal(team))
            return m_trans(Vec3(0, 0, reverse ? m_radius*2 : -m_radius*2));

        // Otherwise do the below:
        // This is done by using Pythagorean Theorem and solving the
        // equation from ball to goal center (y = (m_***_goal_slope) x)

        // We aim behind the ball from the center of the ball to its
        // diameter, so 2*m_radius = sqrt (x2 + y2),
        // which is next x = sqrt (2*m_radius - y2)
        // And than we have x = y / m(m_***_goal_slope)
        // After put that in the slope equation, we have
        // y = sqrt(2*m_radius*m2 / (1+m2))
        float x = 0.0f;
        float y = 0.0f;
        if (team == KART_TEAM_BLUE)
        {
            y = sqrt((m_blue_goal_slope * m_blue_goal_slope * m_radius*2) /
                (1 + (m_blue_goal_slope * m_blue_goal_slope)));
            if (m_blue_goal_2.x() == 0.0f ||
                (m_blue_goal_2.x() > 0.0f && m_blue_goal_2.z() > 0.0f) ||
                (m_blue_goal_2.x() < 0.0f && m_blue_goal_2.z() > 0.0f))
            {
                // Determine when y should be negative
                y = -y;
            }
            x = y / m_blue_goal_slope;
        }
        else
        {
            y = sqrt((m_red_goal_slope * m_red_goal_slope * m_radius*2) /
                (1 + (m_red_goal_slope * m_red_goal_slope)));
            if (m_red_goal_2.x() == 0.0f ||
                (m_red_goal_2.x() > 0.0f && m_red_goal_2.z() > 0.0f) ||
                (m_red_goal_2.x() < 0.0f && m_red_goal_2.z() > 0.0f))
            {
                y = -y;
            }
            x = y / m_red_goal_slope;
        }
        assert (!std::isnan(x));
        assert (!std::isnan(y));
        // Return the world coordinates
        return (reverse ? m_trans(Vec3(-x, 0, -y)) :
            m_trans(Vec3(x, 0, y)));
    }   // getAimPosition
    void resetCheckGoal(const Track* t)
    {
        m_red_check_goal->reset(*t);
        m_blue_check_goal->reset(*t);
    }

    // ------------------------------------------------------------------------
    /** Returns the center point of the given team's goal.
     *  If that goal pointer is missing, returns Vec3(0,0,0).
     */
    Vec3 getGoalCenter(KartTeam team) const
    {
        if (team == KART_TEAM_BLUE)
        {
            if (!m_blue_check_goal) return Vec3(0, 0, 0);
            return m_blue_check_goal->getPoint(CheckGoal::POINT_CENTER);
        }
        else
        {
            if (!m_red_check_goal) return Vec3(0, 0, 0);
            return m_red_check_goal->getPoint(CheckGoal::POINT_CENTER);
        }
    }   // getGoalCenter

    CheckGoal* getCheckGoal(KartTeam team) const
    {
    return (team == KART_TEAM_BLUE) ? m_blue_check_goal : m_red_check_goal;
    }
};   // BallGoalData

//-----------------------------------------------------------------------------
/** Constructor. Sets up the clock mode etc.
 */
SoccerWorld::SoccerWorld() : WorldWithRank()
{
    if (RaceManager::get()->hasTimeTarget())
    {
        WorldStatus::setClockMode(WorldStatus::CLOCK_COUNTDOWN,
            RaceManager::get()->getTimeTarget());
    }
    else
    {
        WorldStatus::setClockMode(CLOCK_CHRONO);
    }

    m_frame_count = 0;
    m_use_highscores = false;
    m_red_ai = 0;
    m_blue_ai = 0;
    m_ball_track_sector = NULL;
    m_bgd.reset(new BallGoalData());
}   // SoccerWorld

//-----------------------------------------------------------------------------
/** The destructor frees all data structures.
 */
SoccerWorld::~SoccerWorld()
{
    m_goal_sound->deleteSFX();

    delete m_ball_track_sector;
    m_ball_track_sector = NULL;
}   // ~SoccerWorld

//-----------------------------------------------------------------------------
/** Initializes the soccer world. It sets up the data structure
 *  to keep track of points etc. for each kart.
 */
int once = 1;
int write_once = 1;
int once_blue_five = 1;
int once_red_five = 1;

void SoccerWorld::init()
{
    once = 1;
    once_blue_five = 1;
    once_red_five = 1;
    write_once = 1;
    m_kart_team_map.clear();
    m_kart_position_map.clear();

    m_presence_intervals_total = 0;
    m_player_presence_count.clear();
    m_player_last_position.clear();
    m_player_last_name.clear();

    WorldWithRank::init();
    m_display_rank = false;
    m_ball_hitter  = -1;
    m_ball_hitter_red = -1;
    m_ball_hitter_blue = -1;
    m_ball         = NULL;
    m_ball_body    = NULL;
    m_goal_target  = RaceManager::get()->getMaxGoal();
    m_goal_sound   = SFXManager::get()->createSoundSource("goal_scored");

    // Clear out any old data from previous matches
    m_kart_scores.clear();
    m_previous_ball_hitter          = -1;
    m_previous_approaching_hitter   = false;
    m_previous_approaching_opponent = false;

    m_previous_approaching_hitter_half   = false;
    m_previous_approaching_opponent_half = false;

    // For each kart, record its name and zero the score
    for (unsigned int i = 0; i < m_karts.size(); i++)
    {
        KartScore ks;
        ks.m_name  = StringUtils::wideToUtf8(getKart(i)->getController()->getName()).c_str();
        m_kart_scores.push_back(ks);
    }

    Track *track = Track::getCurrentTrack();
    if (track->hasNavMesh())
    {
        // Init track sector for ball if navmesh is found
        m_ball_track_sector = new TrackSector();
    }

    TrackObjectManager* tom = track->getTrackObjectManager();
    assert(tom);
    PtrVector<TrackObject>& objects = tom->getObjects();
    for (unsigned int i = 0; i < objects.size(); i++)
    {
        TrackObject* obj = objects.get(i);
        if(!obj->isSoccerBall())
            continue;
        m_ball = obj;
        m_ball_body = m_ball->getPhysicalObject()->getBody();
        // Handle one ball only
        break;
    }
    if (!m_ball)
        Log::fatal("SoccerWorld","Ball is missing in soccer field, abort.");

    float radius = m_ball->getPhysicalObject()->getRadius();
    if (radius <= 0.0f)
    {
        btVector3 min, max;
        m_ball->getPhysicalObject()->getBody()->getAabb(min, max);
        radius = (max.y() - min.y()) / 2.0f;
    }
    m_bgd->init(radius);

}   // init

//-----------------------------------------------------------------------------
/** Called when a soccer game is restarted.
 */
void SoccerWorld::reset(bool restart)
{
    WorldWithRank::reset(restart);
    if (RaceManager::get()->hasTimeTarget())
    {
        WorldStatus::setClockMode(WorldStatus::CLOCK_COUNTDOWN,
            RaceManager::get()->getTimeTarget());
    }
    else
    {
        WorldStatus::setClockMode(CLOCK_CHRONO);
    }

    m_count_down_reached_zero = false;
    m_red_scorers.clear();
    m_blue_scorers.clear();
    m_ball_hitter = -1;
    m_red_kdm.clear();
    m_blue_kdm.clear();
    m_ball_heading = 0.0f;
    m_ball_invalid_timer = 0;
    m_goal_transforms.clear();
    m_goal_transforms.resize(m_karts.size());
    if (m_goal_sound != NULL &&
        m_goal_sound->getStatus() == SFXBase::SFX_PLAYING)
    {
        m_goal_sound->stop();
    }

    if (Track::getCurrentTrack()->hasNavMesh())
    {
        m_ball_track_sector->reset();
    }

    m_reset_ball_ticks = -1;
    m_ball->reset();
    m_bgd->reset();
    m_ticks_back_to_own_goal = -1;
    m_ball->setEnabled(false);

    // Make the player kart in profiling mode up
    // ie make this kart less likely to affect gaming result
    if (UserConfigParams::m_arena_ai_stats)
        getKart(8)->flyUp();

    // Team will remain unchanged even with live join
    std::vector<int> red_id, blue_id;
    for (unsigned int i = 0; i < m_karts.size(); i++)
    {
        if (getKartTeam(i) == KART_TEAM_RED)
            red_id.push_back(i);
        else
            blue_id.push_back(i);
    }

    m_team_icon_draw_id.resize(getNumKarts());
    if (!Track::getCurrentTrack()->getMinimapInvert())
        std::swap(red_id, blue_id);
    int pos = 0;
    for (int id : red_id)
        m_team_icon_draw_id[pos++] = id;
    for (int id : blue_id)
        m_team_icon_draw_id[pos++] = id;
}   // reset

//-----------------------------------------------------------------------------
void SoccerWorld::onGo()
{
    m_ball->setEnabled(true);
    m_ball->reset();
    WorldWithRank::onGo();
}   // onGo

//-----------------------------------------------------------------------------
void SoccerWorld::terminateRace()
{
    const unsigned int kart_amount = getNumKarts();
    for (unsigned int i = 0; i < kart_amount ; i++)
    {
        // Soccer mode use goal for race result, and each goal time is
        // handled by handlePlayerGoalFromServer already
        m_karts[i]->finishedRace(0.0f, true/*from_server*/);
    }   // i<kart_amount
    WorldWithRank::terminateRace();
}   // terminateRace

//-----------------------------------------------------------------------------
/** Returns the internal identifier for this race.
 */
const std::string& SoccerWorld::getIdent() const
{
    return IDENT_SOCCER;
}   // getIdent

//-----------------------------------------------------------------------------
/** Update the world and the track.
 *  \param ticks Physics time steps - should be 1.
 */
void SoccerWorld::update(int ticks)
{
    updateBallPosition(ticks);
    if (Track::getCurrentTrack()->hasNavMesh())
    {
        updateSectorForKarts();
        if (!NetworkConfig::get()->isNetworking())
            updateAIData();
    }

    WorldWithRank::update(ticks);
    WorldWithRank::updateTrack(ticks);

    if (isGoalPhase())
    {
        m_ball_hitter  = -1;
        for (unsigned int i = 0; i < m_karts.size(); i++)
        {
            auto& kart = m_karts[i];
            if (kart->isEliminated())
                continue;
            if (kart->getKartAnimation())
            {
                AbstractKartAnimation* ka = kart->getKartAnimation();
                kart->setKartAnimation(NULL);
                delete ka;
            }
            kart->getBody()->setLinearVelocity(Vec3(0.0f));
            kart->getBody()->setAngularVelocity(Vec3(0.0f));
            kart->getBody()->proceedToTransform(m_goal_transforms[i]);
            kart->setTrans(m_goal_transforms[i]);
        }
        if (m_ticks_back_to_own_goal - getTicksSinceStart() == 1 &&
            !isRaceOver())
        {
            // Reset all karts and ball
            resetKartsToSelfGoals();
            if (UserConfigParams::m_arena_ai_stats)
                getKart(8)->flyUp();
        }
    }

    // ------------------------------------------------------------------------
    // NEW: Update the last hitter’s score based on whether the ball is
    // approaching that hitter’s gate or the opposing gate.
    // ------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    // STEP 1: Add a static (or member) counter to ensure scoring updates
    //         are performed only once every 50 ticks.
    // -------------------------------------------------------------------------
    static int s_score_update_counter = 0;
    s_score_update_counter += ticks;
    if ((s_score_update_counter >= 50) && (!isRaceOver()))
    {
        // Reset the counter so it updates again in another 50 ticks
        s_score_update_counter = 0;

        // ---------------------------------------------------------------------
        // STEP 2: Move the existing scoring logic so it’s only called
        //         when s_score_update_counter >= 5.
        // ---------------------------------------------------------------------
        if ((m_ball_hitter >= 0) && (m_ball_hitter < (int)m_karts.size()))
        {
             KartTeam hitter_team = getKartTeam(m_ball_hitter);
             KartTeam opponent_team = (hitter_team == KART_TEAM_RED) ? KART_TEAM_BLUE : KART_TEAM_RED;

            // Each frame that the ball is “owned” by a team, add 1 to that team’s counter
            if (hitter_team == KART_TEAM_RED)
            {
                m_ball_possession_red++;
            }
            else if (hitter_team == KART_TEAM_BLUE)
            {
                m_ball_possession_blue++;
            }

            bool approaching_hitter_goal   = isBallMovingTowardGoal(hitter_team);
            bool approaching_opponent_goal = isBallMovingTowardGoal(opponent_team);

            bool approaching_hitter_half  = isBallMovingTowardHalf(hitter_team);
            bool approaching_opponent_half = isBallMovingTowardHalf(opponent_team);

            bool hitter_changed = (m_ball_hitter != m_previous_ball_hitter);
            bool hitter_goal_changed  = (approaching_hitter_goal   != m_previous_approaching_hitter);
            bool opponent_goal_changed = (approaching_opponent_goal != m_previous_approaching_opponent);

            bool hitter_half_changed  = (approaching_hitter_half   != m_previous_approaching_hitter_half);
            bool opponent_half_changed = (approaching_opponent_half != m_previous_approaching_opponent_half);

            bool temp_bool = false;
            if (getKartTeam(m_ball_hitter) != getKartTeam(m_previous_ball_hitter))
            {
                temp_bool = m_previous_approaching_hitter;
                m_previous_approaching_hitter = m_previous_approaching_opponent;
                m_previous_approaching_opponent = temp_bool;

                temp_bool = m_previous_approaching_hitter_half;
                m_previous_approaching_hitter_half = m_previous_approaching_opponent_half;
                m_previous_approaching_opponent_half = temp_bool;
            }

            // If either the ball hitter or 'approaching' flags changed, update scoring
            if ((hitter_changed || hitter_goal_changed || opponent_goal_changed) && (!RewindManager::get()->isRewinding()))
            {
                if (approaching_opponent_goal)
                {
                    m_kart_scores[m_ball_hitter].attacking_pts += 1;
                    m_kart_scores[m_ball_hitter].total_pts     += 1;
                   // Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "shoot at opponent goal");
                }
                else if ((approaching_hitter_goal) && (!m_previous_approaching_hitter))
                {
                    m_kart_scores[m_ball_hitter].bad_play_pts += -1;
                    m_kart_scores[m_ball_hitter].total_pts    += -1;
                  //  Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "shoot at his goal");
                }
                else if ((m_previous_approaching_hitter) && (!approaching_hitter_goal) && isBallBetweenRedAndBlueGates())
                {
                    // Defended your own goal
                    m_kart_scores[m_ball_hitter].defending_pts += 1;
                    m_kart_scores[m_ball_hitter].total_pts     += 1;
                   // Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "Defended his goal");
                }
                else if ((m_previous_approaching_opponent) && (!approaching_opponent_goal) && (hitter_changed) && (isBallBetweenRedAndBlueGates()))
                {
                    // Defended opponent's goal
                    m_kart_scores[m_ball_hitter].bad_play_pts += -1;
                    m_kart_scores[m_ball_hitter].total_pts    += -1;
                    //Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "Defended/missed opponent goal");
                }
            }
            else if ((hitter_changed || hitter_half_changed || opponent_half_changed) && (!RewindManager::get()->isRewinding()))
            {
                if (approaching_opponent_half)
                {
                   m_kart_scores[m_ball_hitter].inddefending_pts += 1;
                   m_kart_scores[m_ball_hitter].total_pts     += 1;
                  // Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "shoot at opponent half");
                }
                else if ((approaching_hitter_half) && (!m_previous_approaching_hitter_half))
                {
                    m_kart_scores[m_ball_hitter].bad_play_pts += -1;
                    m_kart_scores[m_ball_hitter].total_pts    += -1;
                   // Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "shoot at his half");
                }
            }

            // Save state for next scoring check
            m_previous_ball_hitter          = m_ball_hitter;
            m_previous_approaching_hitter   = approaching_hitter_goal;
            m_previous_approaching_opponent = approaching_opponent_goal;

            m_previous_approaching_hitter_half   = approaching_hitter_half;
            m_previous_approaching_opponent_half = approaching_opponent_half;
        }
        else
        {
            // If no valid hitter, reset approach flags (or leave them alone if you prefer)
            m_previous_ball_hitter          = -1;
            m_previous_approaching_hitter   = false;
            m_previous_approaching_opponent = false;

            m_previous_approaching_hitter_half   = false;
            m_previous_approaching_opponent_half = false;
        }
    }
    // -------------------------------------------------------------------------
    static int s_presence_update_counter = 0;
    s_presence_update_counter += ticks;
    if ((s_presence_update_counter >= 1000) && (!isRaceOver()))
{
    // Reset the counter so it updates again in another 50 ticks
    s_presence_update_counter = 0;

    for (unsigned int i = 0; i < m_karts.size(); i++)
    {
        const std::string player_name =
            StringUtils::wideToUtf8(m_karts[i]->getController()->getName());
        if (player_name.empty()) continue;

        int kart_id = m_karts[i]->getWorldKartId();

        // ─────────────────────────────────────────────────────────────
        // NEW STEP: check if this kart’s name changed since last time
        // ─────────────────────────────────────────────────────────────
        auto it_name = m_player_last_name.find(kart_id);
        if (it_name == m_player_last_name.end())
        {
            // First time seeing this kart id, just store its name
            m_player_last_name[kart_id] = player_name;
        }
        else
        {
            if (it_name->second != player_name)
            {
                // The name for this kart id changed!
                // 1) Update the stored name
                it_name->second = player_name;

                // 2) Reset presence counter
                m_player_presence_count[kart_id] = 0;

                // 3) Reset the scoreboard for this kart.
                //    We can search for the correct index in m_kart_scores
                //    by matching the same i or by matching name.
                //    Typically, “i” is the same index for m_kart_scores,
                //    so we can do:
                m_kart_scores[i].scoring_pts   = 0;
                m_kart_scores[i].attacking_pts = 0;
                m_kart_scores[i].defending_pts = 0;
                m_kart_scores[i].bad_play_pts  = 0;
                m_kart_scores[i].total_pts     = 0;

                //    If you prefer to erase the old name entirely so the new name can
                //    be appended, you can also do (optional):
                m_kart_scores[i].m_name = player_name;
            }
        }
        // ─────────────────────────────────────────────────────
        // Name check done. Now do your presence logic as before
        // ─────────────────────────────────────────────────────
        Vec3 current_pos = m_karts[kart_id]->getXYZ();

        if (m_player_last_position.find(kart_id) == m_player_last_position.end())
        {
            m_player_presence_count[kart_id]++;
            m_player_last_position[kart_id] = current_pos;
        }
        else
        {
            Vec3 last_pos = m_player_last_position[kart_id];
            if (current_pos != last_pos)
            {
                m_player_presence_count[kart_id]++;
            }
            m_player_last_position[kart_id] = current_pos;
        }
    } // end for


    // We have hit the "50-tick" interval one more time
    m_presence_intervals_total++;
}
    if (UserConfigParams::m_arena_ai_stats)
        m_frame_count++;

    // FIXME before next release always update soccer kart position to fix
    // powerup random number
    /*beginSetKartPositions();
    int pos = 1;
    for (unsigned i = 0; i < getNumKarts(); i++)
    {
        if (getKart(i)->isEliminated())
            continue;
        setKartPosition(i, pos++);
    }
    for (unsigned i = 0; i < getNumKarts(); i++)
    {
        if (!getKart(i)->isEliminated())
            continue;
        setKartPosition(i, pos++);
    }
    endSetKartPositions();*/
}   // update

//-----------------------------------------------------------------------------
void SoccerWorld::onCheckGoalTriggered(bool first_goal)
{
    auto sl = LobbyProtocol::get<ServerLobby>();
    if (isRaceOver() || isStartPhase() ||
        (NetworkConfig::get()->isNetworking() &&
        NetworkConfig::get()->isClient()))
        return;

    if (getTicksSinceStart() < m_ticks_back_to_own_goal)
        return;
    m_ticks_back_to_own_goal = getTicksSinceStart() +
        stk_config->time2Ticks(3.0f);
    m_goal_sound->play();
    m_ball->reset();
    m_ball->setEnabled(false);
    if (m_ball_hitter != -1)
    {
        if (UserConfigParams::m_arena_ai_stats)
        {
            const int elapsed_frame = m_goal_frame.empty() ? 0 :
                std::accumulate(m_goal_frame.begin(), m_goal_frame.end(), 0);
            m_goal_frame.push_back(m_frame_count - elapsed_frame);
        }

        if (!isCorrectGoal(m_ball_hitter, first_goal))
        {
         // m_kart_scores[m_ball_hitter].bad_play_pts += -1;
         // m_kart_scores[m_ball_hitter].total_pts    += -1;
         // Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "scored a goal!");
          KartTeam team = getKartTeam(m_ball_hitter);
          if (team == KART_TEAM_RED && m_ball_hitter_blue != -1) m_ball_hitter = m_ball_hitter_blue;
          else if (team == KART_TEAM_BLUE && m_ball_hitter_red != -1) m_ball_hitter = m_ball_hitter_red;
        }

        m_kart_scores[m_ball_hitter].scoring_pts += 1;
        m_kart_scores[m_ball_hitter].total_pts   += 1;
        // Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "scored a goal!!");

        ScorerData sd = {};
        sd.m_id = m_ball_hitter;
        sd.m_correct_goal = isCorrectGoal(m_ball_hitter, first_goal);
        sd.m_kart = getKart(m_ball_hitter)->getIdent();
        sd.m_player = getKart(m_ball_hitter)->getController()
            ->getName(false/*include_handicap_string*/);
        sd.m_handicap_level = getKart(m_ball_hitter)->getHandicap();
        if (RaceManager::get()->getKartGlobalPlayerId(m_ball_hitter) > -1)
        {
            sd.m_country_code =
                RaceManager::get()->getKartInfo(m_ball_hitter).getCountryCode();
        }
        if (sd.m_correct_goal)
        {
            m_karts[m_ball_hitter]->getKartModel()
                ->setAnimation(KartModel::AF_WIN_START, true/* play_non_loop*/);
        }
        else if (!sd.m_correct_goal)
        {
            m_karts[m_ball_hitter]->getKartModel()
                ->setAnimation(KartModel::AF_LOSE_START, true/* play_non_loop*/);
        }

#ifndef SERVER_ONLY
        // show a message once a goal is made
        core::stringw msg;
        if (sd.m_correct_goal)
            msg = _("%s scored a goal!", sd.m_player);
        else
            msg = _("Oops, %s made an own goal!", sd.m_player);
        if (m_race_gui)
        {
            m_race_gui->addMessage(msg, NULL, 3.0f,
                video::SColor(255, 255, 0, 255), /*important*/true,
                /*big_font*/false, /*outline*/true);
        }
#endif

        if (first_goal)
        {
            if (RaceManager::get()->hasTimeTarget())
            {
                sd.m_time = RaceManager::get()->getTimeTarget() - getTime();
            }
            else
                sd.m_time = getTime();
            // Notice: true first_goal means it's blue goal being shoot,
            // so red team can score
            m_red_scorers.push_back(sd);
        }
        else
        {
            if (RaceManager::get()->hasTimeTarget())
            {
                sd.m_time = RaceManager::get()->getTimeTarget() - getTime();
            }
            else
                sd.m_time = getTime();
            m_blue_scorers.push_back(sd);
        }
        if (NetworkConfig::get()->isNetworking() &&
            NetworkConfig::get()->isServer())
        {
            NetworkString p(PROTOCOL_GAME_EVENTS);
            p.setSynchronous(true);
            p.addUInt8(GameEventsProtocol::GE_PLAYER_GOAL)
                .addUInt8((uint8_t)sd.m_id).addUInt8(sd.m_correct_goal)
                .addUInt8(first_goal).addFloat(sd.m_time)
                .addTime(m_ticks_back_to_own_goal)
                .encodeString(sd.m_kart).encodeString(sd.m_player);
            // Added in 1.1, add missing handicap info and country code
            NetworkString p_1_1 = p;
            p_1_1.encodeString(sd.m_country_code)
                .addUInt8(sd.m_handicap_level);
            auto peers = STKHost::get()->getPeers();
            for (auto& peer : peers)
            {
                if (peer->isValidated() && !peer->isWaitingForGame())
                {
                    if (peer->getClientCapabilities().find("soccer_fixes") !=
                        peer->getClientCapabilities().end())
                    {
                        peer->sendPacket(&p_1_1, true/*reliable*/);
                    }
                    else
                    {
                        peer->sendPacket(&p, true/*reliable*/);
                    }
                }
            }
        }
    }
    for (unsigned i = 0; i < m_karts.size(); i++)
    {
        auto& kart = m_karts[i];
        kart->getBody()->setLinearVelocity(Vec3(0.0f));
        kart->getBody()->setAngularVelocity(Vec3(0.0f));
        m_goal_transforms[i] = kart->getBody()->getWorldTransform();
    }

    if((abs(getScore(KART_TEAM_BLUE)-getScore(KART_TEAM_RED)) == 4) && (powerup_multiplier_value() == 1) && (!isRaceOver()) && (once == 1))
    {
        if (sl->losing_team_weaker())
        {
        set_powerup_multiplier(3);
            sl->send_message("Powerupper on (automatically)");
            once =2;
        }
    }

    if ((once == 2) && (powerup_multiplier_value() == 1) && (sl->losing_team_weaker()) && (!isRaceOver()))
    {
    set_powerup_multiplier(3);
        sl->send_message("Powerupper on (automatically)");
    }

    if ((abs(getScore(KART_TEAM_BLUE)-getScore(KART_TEAM_RED)) != 0) && (powerup_multiplier_value() == 3) && (!sl->losing_team_weaker()) && (!isRaceOver()))
    {
    set_powerup_multiplier(1);
        sl->send_message("Powerupper off (automatically)");
    }

    if(getScore(KART_TEAM_BLUE) == 5 && once_blue_five == 1 )
    {
        auto poss = getBallPossession();
        int red_possession  = poss.first;
        int blue_possession = poss.second;
        sl->send_message("Ball Possession:\n\U0001f7e5 Red " + std::to_string(red_possession)+ "% : " + std::to_string(blue_possession) + "% Blue \U0001f7e6");

        once_blue_five = 2;
    }
    if(getScore(KART_TEAM_RED) == 5 && once_red_five == 1)
    {
        auto poss = getBallPossession();
        int red_possession  = poss.first;
        int blue_possession = poss.second;
        sl->send_message("Ball Possession:\n\U0001f7e5 Red " + std::to_string(red_possession)+ "% : " + std::to_string(blue_possession) + "% Blue \U0001f7e6");

        once_red_five = 2;
    }
    if(isRaceOver())
    {
        auto poss = getBallPossession();
        int red_possession  = poss.first;
        int blue_possession = poss.second;
        sl->send_message("Ball Possession:\n\U0001f7e5 Red " + std::to_string(red_possession)+ "% : " + std::to_string(blue_possession) + "% Blue \U0001f7e6");
    }

    m_ball_hitter_red = -1;
    m_ball_hitter_blue = -1;
    m_ball_hitter = -1;
}   // onCheckGoalTriggered

//-----------------------------------------------------------------------------
void SoccerWorld::handleResetBallFromServer(const NetworkString& ns)
{
    int ticks_now = World::getWorld()->getTicksSinceStart();
    int ticks_back_to_own_goal = ns.getTime();
    if (ticks_now >= ticks_back_to_own_goal)
    {
        Log::warn("SoccerWorld", "Server ticks %d is too close to client ticks "
            "%d when reset player", ticks_back_to_own_goal, ticks_now);
        return;
    }
    m_reset_ball_ticks = ticks_back_to_own_goal;
}   // handleResetBallFromServer

//-----------------------------------------------------------------------------
void SoccerWorld::handlePlayerGoalFromServer(const NetworkString& ns)
{
    ScorerData sd = {};
    sd.m_id = ns.getUInt8();
    sd.m_correct_goal = ns.getUInt8() == 1;
    bool first_goal = ns.getUInt8() == 1;
    sd.m_time = ns.getFloat();
    int ticks_now = World::getWorld()->getTicksSinceStart();
    int ticks_back_to_own_goal = ns.getTime();
    ns.decodeString(&sd.m_kart);
    ns.decodeStringW(&sd.m_player);

   /*
     // Insert the new scoring lines:
    if (sd.m_correct_goal)
    {
        m_kart_scores[sd.m_id].scoring_pts += 1;
        m_kart_scores[sd.m_id].total_pts   += 1;
        Log::verbose((m_kart_scores[m_ball_hitter].m_name).c_str(), "scored a goal!");
    }
    else
    {
        m_kart_scores[sd.m_id].bad_play_pts += -1;
        m_kart_scores[sd.m_id].total_pts    += -1;
    }*/

    // Added in 1.1, add missing handicap info and country code
    if (NetworkConfig::get()->getServerCapabilities().find("soccer_fixes")
        != NetworkConfig::get()->getServerCapabilities().end())
    {
        ns.decodeString(&sd.m_country_code);
        sd.m_handicap_level = (HandicapLevel)ns.getUInt8();
    }

    if (first_goal)
    {
        m_red_scorers.push_back(sd);
    }
    else
    {
        m_blue_scorers.push_back(sd);
    }

    if (ticks_now >= ticks_back_to_own_goal && !isStartPhase())
    {
        Log::warn("SoccerWorld", "Server ticks %d is too close to client ticks "
            "%d when goal", ticks_back_to_own_goal, ticks_now);
        return;
    }

    // show a message once a goal is made
    core::stringw msg;
    if (sd.m_correct_goal)
        msg = _("%s scored a goal!", sd.m_player);
    else
        msg = _("Oops, %s made an own goal!", sd.m_player);
    float time = stk_config->ticks2Time(ticks_back_to_own_goal - ticks_now);
    // May happen if this message is added when spectate started
    if (time > 3.0f)
        time = 3.0f;
    if (m_race_gui && !isStartPhase())
    {
        m_race_gui->addMessage(msg, NULL, time,
            video::SColor(255, 255, 0, 255), /*important*/true,
            /*big_font*/false, /*outline*/true);
    }

    m_ticks_back_to_own_goal = ticks_back_to_own_goal;
    for (unsigned i = 0; i < m_karts.size(); i++)
    {
        auto& kart = m_karts[i];
        btTransform transform_now = kart->getBody()->getWorldTransform();
        kart->getBody()->setLinearVelocity(Vec3(0.0f));
        kart->getBody()->setAngularVelocity(Vec3(0.0f));
        kart->getBody()->proceedToTransform(transform_now);
        kart->setTrans(transform_now);
        m_goal_transforms[i] = transform_now;
    }
    m_ball->reset();
    m_ball->setEnabled(false);

    // Ignore the rest in live join
    if (isStartPhase())
        return;

    if (sd.m_correct_goal)
    {
        m_karts[sd.m_id]->getKartModel()
            ->setAnimation(KartModel::AF_WIN_START, true/* play_non_loop*/);
    }
    else if (!sd.m_correct_goal)
    {
        m_karts[sd.m_id]->getKartModel()
            ->setAnimation(KartModel::AF_LOSE_START, true/* play_non_loop*/);
    }
    m_goal_sound->play();

}   // handlePlayerGoalFromServer

//-----------------------------------------------------------------------------
void SoccerWorld::resetKartsToSelfGoals()
{
    m_ball->setEnabled(true);
    m_ball->reset();
    m_bgd->resetCheckGoal(Track::getCurrentTrack());
    for (unsigned i = 0; i < m_karts.size(); i++)
    {
        auto& kart = m_karts[i];
        if (kart->isEliminated())
            continue;

        if (kart->getKartAnimation())
        {
            AbstractKartAnimation* ka = kart->getKartAnimation();
            kart->setKartAnimation(NULL);
            delete ka;
        }
        kart->getBody()->setLinearVelocity(Vec3(0.0f));
        kart->getBody()->setAngularVelocity(Vec3(0.0f));
        unsigned index = m_kart_position_map.at(kart->getWorldKartId());
        btTransform t = Track::getCurrentTrack()->getStartTransform(index);
        moveKartTo(kart.get(), t);
    }
}   // resetKartsToSelfGoals

//-----------------------------------------------------------------------------
/** Sets the last kart that hit the ball, to be able to
 * identify the scorer later.
 */
void SoccerWorld::setBallHitter(unsigned int kart_id)
{
    m_ball_hitter = kart_id;
    KartTeam team = getKartTeam(kart_id);
    if (team == KART_TEAM_RED) m_ball_hitter_red = kart_id;
    else m_ball_hitter_blue = kart_id;
}   // setBallHitter

//-----------------------------------------------------------------------------
/** The soccer game is over if time up or either team wins.
 */
bool SoccerWorld::isRaceOver()
{
    if (m_unfair_team)
        return true;

    if (RaceManager::get()->hasTimeTarget())
    {
        return m_count_down_reached_zero;
    }
    // One team scored the target goals ...
    else
    {
        return (getScore(KART_TEAM_BLUE) >= m_goal_target ||
            getScore(KART_TEAM_RED) >= m_goal_target);
    }

}   // isRaceOver

//-----------------------------------------------------------------------------
/** Called when the match time ends.
 */
void SoccerWorld::countdownReachedZero()
{
    // Prevent negative time in network soccer when finishing
    m_time_ticks = 0;
    m_time = 0.0f;
    m_count_down_reached_zero = true;
}   // countdownReachedZero

//-----------------------------------------------------------------------------
bool SoccerWorld::getKartSoccerResult(unsigned int kart_id) const
{
    if (m_red_scorers.size() == m_blue_scorers.size()) return true;

    bool red_win = m_red_scorers.size() > m_blue_scorers.size();
    KartTeam team = getKartTeam(kart_id);

    if ((red_win && team == KART_TEAM_RED) ||
        (!red_win && team == KART_TEAM_BLUE))
        return true;
    else
        return false;

}   // getKartSoccerResult

//-----------------------------------------------------------------------------
/** Localize the ball on the navigation mesh.
 */
void SoccerWorld::updateBallPosition(int ticks)
{
    if (isRaceOver()) return;

    if (!ballNotMoving())
    {
        // Only update heading if the ball is moving
        m_ball_heading = atan2f(m_ball_body->getLinearVelocity().getX(),
            m_ball_body->getLinearVelocity().getZ());
    }

    if (Track::getCurrentTrack()->hasNavMesh())
    {
        m_ball_track_sector
            ->update(getBallPosition(), true/*ignore_vertical*/);

        bool is_client = NetworkConfig::get()->isNetworking() &&
            NetworkConfig::get()->isClient();
        bool is_server = NetworkConfig::get()->isNetworking() &&
            NetworkConfig::get()->isServer();

        if (!is_client && getTicksSinceStart() > m_reset_ball_ticks &&
            !m_ball_track_sector->isOnRoad())
        {
            m_ball_invalid_timer += ticks;
            // Reset the ball and karts if out of navmesh after 2 seconds
            if (m_ball_invalid_timer >= stk_config->time2Ticks(2.0f))
            {
                if (is_server)
                {
                    // Reset the ball 2 seconds in the future to make sure it's
                    // after all clients time
                    m_reset_ball_ticks = getTicksSinceStart() +
                        stk_config->time2Ticks(2.0f);

                    NetworkString p(PROTOCOL_GAME_EVENTS);
                    p.setSynchronous(true);
                    p.addUInt8(GameEventsProtocol::GE_RESET_BALL)
                        .addTime(m_reset_ball_ticks);
                    STKHost::get()->sendPacketToAllPeers(&p, true);
                }
                else if (!NetworkConfig::get()->isNetworking())
                {
                    m_ball_invalid_timer = 0;
                    resetKartsToSelfGoals();
                    if (UserConfigParams::m_arena_ai_stats)
                        getKart(8)->flyUp();
                }
            }
        }
        else
            m_ball_invalid_timer = 0;
        if (m_reset_ball_ticks == World::getWorld()->getTicksSinceStart())
        {
            resetKartsToSelfGoals();
        }
    }

}   // updateBallPosition

//-----------------------------------------------------------------------------
int SoccerWorld::getBallNode() const
{
    assert(m_ball_track_sector != NULL);
    return m_ball_track_sector->getCurrentGraphNode();
}   // getBallNode

//-----------------------------------------------------------------------------
bool SoccerWorld::isCorrectGoal(unsigned int kart_id, bool first_goal) const
{
    KartTeam team = getKartTeam(kart_id);
    if (first_goal)
    {
        if (team == KART_TEAM_RED)
            return true;
    }
    else if (!first_goal)
    {
        if (team == KART_TEAM_BLUE)
            return true;
    }
    return false;
}   // isCorrectGoal

//-----------------------------------------------------------------------------
void SoccerWorld::updateAIData()
{
    if (isRaceOver()) return;

    // Fill the kart distance map
    m_red_kdm.clear();
    m_blue_kdm.clear();

    for (unsigned int i = 0; i < m_karts.size(); ++i)
    {
        if (UserConfigParams::m_arena_ai_stats &&
            m_karts[i]->getController()->isPlayerController())
            continue;

        if (getKartTeam(m_karts[i]->getWorldKartId()) == KART_TEAM_RED)
        {
            Vec3 rd = m_karts[i]->getXYZ() - getBallPosition();
            m_red_kdm.push_back(KartDistanceMap(i, rd.length_2d()));
        }
        else
        {
            Vec3 bd = m_karts[i]->getXYZ() - getBallPosition();
            m_blue_kdm.push_back(KartDistanceMap(i, bd.length_2d()));
        }
    }
    // Sort the vectors, so first vector will have the min distance
    std::sort(m_red_kdm.begin(), m_red_kdm.end());
    std::sort(m_blue_kdm.begin(), m_blue_kdm.end());

    // Fill Ball and goals data
    m_bgd->updateBallAndGoal(getBallPosition(), getBallHeading());

}   // updateAIData

//-----------------------------------------------------------------------------
int SoccerWorld::getAttacker(KartTeam team) const
{
    if (team == KART_TEAM_BLUE && m_blue_kdm.size() > 1)
    {
        for (unsigned int i = 1; i < m_blue_kdm.size(); i++)
        {
            // Only AI will do the attack job
            if (getKart(m_blue_kdm[i].m_kart_id)
                ->getController()->isPlayerController())
                continue;
            return m_blue_kdm[i].m_kart_id;
        }
    }
    else if (team == KART_TEAM_RED && m_red_kdm.size() > 1)
    {
        for (unsigned int i = 1; i < m_red_kdm.size(); i++)
        {
            if (getKart(m_red_kdm[i].m_kart_id)
                ->getController()->isPlayerController())
                continue;
            return m_red_kdm[i].m_kart_id;
        }
    }

    // No attacker
    return -1;
}   // getAttacker

//-----------------------------------------------------------------------------
unsigned int SoccerWorld::getRescuePositionIndex(AbstractKart *kart)
{
    if (!Track::getCurrentTrack()->hasNavMesh())
        return m_kart_position_map.at(kart->getWorldKartId());

    int last_valid_node =
        getTrackSector(kart->getWorldKartId())->getLastValidGraphNode();
    if (last_valid_node >= 0)
        return last_valid_node;
    Log::warn("SoccerWorld", "Missing last valid node for rescuing");
    return 0;
}   // getRescuePositionIndex

//-----------------------------------------------------------------------------
btTransform SoccerWorld::getRescueTransform(unsigned int rescue_pos) const
{
    if (!Track::getCurrentTrack()->hasNavMesh())
        return WorldWithRank::getRescueTransform(rescue_pos);

    const Vec3 &xyz = Graph::get()->getQuad(rescue_pos)->getCenter();
    const Vec3 &normal = Graph::get()->getQuad(rescue_pos)->getNormal();
    btTransform pos;
    pos.setOrigin(xyz);
    btQuaternion q1 = shortestArcQuat(Vec3(0.0f, 1.0f, 0.0f), normal);
    pos.setRotation(q1);

    // Get the ball initial position (at the moment of hitting the rescue button)
    Vec3 ball_position_0 = getBallPosition();

    // Get the ball Linear Velocity
    Vec3 ball_velocity = m_ball_body->getLinearVelocity();

    // Calculate the new ball position after 1.1 s (after the rescue animation is over) assuming the ball to be moving in URM
    Vec3 ball_position = ball_velocity*1.1 + ball_position_0;

    // Calculate the direction vector from the kart to the ball
    Vec3 direction = (ball_position - xyz).normalized();

    // Calculate the angle between the kart's forward direction and the direction vector
    float angle = atan2f(direction.getX(), direction.getZ());

    // Create a quaternion representing the rotation around the Y axis
    btQuaternion q2(btVector3(0.0f, 1.0f, 0.0f), angle);

    // Combine the two rotations and normalize the result
    btQuaternion combined_rotation = q1 * q2;
    combined_rotation.normalize();
    pos.setRotation(combined_rotation);

    return pos;
}   // getRescueTransform

//-----------------------------------------------------------------------------
void SoccerWorld::enterRaceOverState()
{
    WorldWithRank::enterRaceOverState();

    if(write_once == 1)
    {
            // --- Reset ranking monthly Begins Here ---

    // (1) Read all file names in the current directory.
    std::vector<std::string> directoryFiles;
    DIR* dir = opendir(".");
    if (dir != NULL)
    {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL)
        {
            directoryFiles.push_back(std::string(ent->d_name));
        }
        closedir(dir);
    }
    else
    {
        Log::warn("SoccerWorld", "Could not open the current directory");
    }

    // (2) Get current month and year, then deduce the previous month (adjust year if needed).
    std::time_t t_now = std::time(nullptr);
    std::tm* tm_info = std::localtime(&t_now);
    int currMonth = tm_info->tm_mon; // tm_mon in range [0, 11]
    int currYear = tm_info->tm_year + 1900;

    int prevMonth, prevYear;
    if (currMonth == 0) // January: previous is December of the previous year
    {
        prevMonth = 11;
        prevYear = currYear - 1;
    }
    else
    {
        prevMonth = currMonth - 1;
        prevYear = currYear;
    }
    const char* month_names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    std::string monthSuffix = std::string(month_names[prevMonth]) + "_" + std::to_string(prevYear);

    // (3) List of target files to be renamed.
    std::vector<std::string> targetFiles;
    targetFiles.push_back("soccer_ranking.txt");
    targetFiles.push_back("soccer_ranking_detailed.txt");
    targetFiles.push_back("soccer_ranking_detailed.db");
    targetFiles.push_back("db_table_copy.txt");
    targetFiles.push_back("colallstats.txt");

    // (4) For each file, if it exists in the directory, construct a new name by appending "_" + monthSuffix
    // before the extension and rename it (only if the new name doesn't already exist).
    for (size_t i = 0; i < targetFiles.size(); ++i)
    {
        const std::string &oldName = targetFiles[i];
        // Check if the file exists in the current directory.
        if (std::find(directoryFiles.begin(), directoryFiles.end(), oldName) != directoryFiles.end())
        {
            // Construct new name: insert "_" + monthSuffix before the file extension.
            std::string newName;
            size_t pos = oldName.rfind('.');
            if (pos != std::string::npos)
            {
                newName = oldName.substr(0, pos) + "_" + monthSuffix + oldName.substr(pos);
            }
            else
            {
                newName = oldName + "_" + monthSuffix;
            }

            // Check if the new name already exists.
            if (std::find(directoryFiles.begin(), directoryFiles.end(), newName) == directoryFiles.end())
            {
                // Attempt to rename the file.
                if (std::rename(oldName.c_str(), newName.c_str()) == 0)
                {
                    Log::info("SoccerWorld", ("Renamed " + oldName + " to " + newName).c_str());
                    // Optionally update the directoryFiles vector.
                    directoryFiles.push_back(newName);
                }
                else
                {
                    Log::warn("SoccerWorld", ("Failed renaming " + oldName + " to " + newName).c_str());
                }
            }
            else
            {
                Log::info("SoccerWorld", (newName + " already exists, skipping rename.").c_str());
            }
        }
        else
        {
            Log::info("SoccerWorld", (oldName + " does not exist in the current directory, skipping.").c_str());
        }
    }

    // --- Reset ranking monthly Ends Here ---



        // Now figure out how many seconds the match lasted
        float final_time_secs = getTime();  // total match time in seconds
    // ─────────────────────────────────────────────
        // check if there's at least 1 red & 1 blue
        // ─────────────────────────────────────────────
        int red_count = 0;
        int blue_count = 0;
        for (unsigned int i = 0; i < m_karts.size(); i++)
        {
        // If the name is empty, skip
        const std::string &kart_name = StringUtils::wideToUtf8(m_karts[i]->getController()->getName());
        if (kart_name.empty()) continue;

        // If non-empty name, see which team
        if (getKartTeam(i) == KART_TEAM_RED)  red_count  += 1;
        if (getKartTeam(i) == KART_TEAM_BLUE) blue_count += 1;
        }

        if ((red_count == 0) || (blue_count == 0) || ((red_count+blue_count) <= 2))
        {
            // If either team is missing, skip the ranking logic or just warn:
            Log::warn("SoccerWorld",
                "Skipping DB writes: not enough players in red/blue teams.");
        }
    else
        {
    // ----------------------------------------------------------------
    // C) Now add matches_played, matches_won columns in the DB
    //    and update them the same way
    // ----------------------------------------------------------------
    sqlite3* db = nullptr;
    int rc = sqlite3_open("soccer_ranking_detailed.db", &db);
    if (rc != SQLITE_OK)
    {
        Log::warn("SoccerWorld",
            (std::string("Cannot open SQLite DB: ") + sqlite3_errmsg(db)).c_str());
        if (db) sqlite3_close(db);
    }
    else
    {
        // 1) Create table with new columns matches_played, matches_won
        //    (Rank is also in here as before)
        const char* create_table_sql =
          "CREATE TABLE IF NOT EXISTS players ("
          "  PlayerName      TEXT PRIMARY KEY,"
          "  ScoringPts      INTEGER,"
          "  AttackingPts    INTEGER,"
          "  DefendingPts    INTEGER,"
          "  IndDefendingPts    INTEGER,"
          "  BadPlayPts      INTEGER,"
          "  Total           INTEGER,"
          "  Rank            INTEGER,"
          "  matches_played  REAL,"
          "  matches_participated            INTEGER,"
          "  matches_won     INTEGER,"
          "  team_members_count     INTEGER,"
          "  minutes_played_count     REAL"
          ");";

        char* err_msg = nullptr;
        rc = sqlite3_exec(db, create_table_sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK)
        {
            Log::warn("SoccerWorld",
                (std::string("Failed to create table: ") +
                 (err_msg ? err_msg : "(no msg)")).c_str());
            sqlite3_free(err_msg);
        }
        else
        {
            // 2) We do an upsert that sums the new partial points
            //    and also increments matches_played by +1,
            //    increments matches_won by +1 if the player truly won.
            //
            //    So we pass 1 or 0 for “matches_won” in the VALUES list,
            //    then do “players.matches_won + excluded.matches_won”.
            //    Same for matches_played +1 for everyone.

            const char* upsert_sql =
              "INSERT INTO players (PlayerName, ScoringPts, AttackingPts, "
              "                     DefendingPts, IndDefendingPts, BadPlayPts, Total, "
              "                     matches_played, matches_participated, matches_won, team_members_count, minutes_played_count) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
              "ON CONFLICT(PlayerName) DO UPDATE SET "
              "  ScoringPts      = players.ScoringPts      + excluded.ScoringPts, "
              "  AttackingPts    = players.AttackingPts    + excluded.AttackingPts, "
              "  DefendingPts    = players.DefendingPts    + excluded.DefendingPts, "
              "  IndDefendingPts    = players.IndDefendingPts    + excluded.IndDefendingPts, "
              "  BadPlayPts      = players.BadPlayPts      + excluded.BadPlayPts, "
              "  Total           = players.Total           + excluded.Total, "
              "  matches_played  = players.matches_played  + excluded.matches_played, "
              "  matches_participated  = players.matches_participated  + excluded.matches_participated, "
              "  matches_won     = players.matches_won     + excluded.matches_won,"
              "  team_members_count     = players.team_members_count     + excluded.team_members_count,"
              "  minutes_played_count     = players.minutes_played_count     + excluded.minutes_played_count;";

            sqlite3_stmt* stmt = nullptr;
            rc = sqlite3_prepare_v2(db, upsert_sql, -1, &stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                Log::warn("SoccerWorld",
                    (std::string("Cannot prepare upsert: ") +
                     sqlite3_errmsg(db)).c_str());
            }
            else
            {
                // For each player who participated in the match
                for (unsigned int i=0; i<m_karts.size(); i++)
                {
                    std::string player_name = StringUtils::wideToUtf8(m_karts[i]->getController()->getName());
                    if (player_name.empty()) continue;

                    int kart_id = m_karts[i]->getWorldKartId();

                    // If this kart never got incremented, presence_count=0 => 0 time
                    int presence_count = 0;
                    if (m_player_presence_count.find(kart_id) != m_player_presence_count.end())
                    {
                        presence_count = m_player_presence_count[kart_id];
                    }

                    // Safeguard: avoid division by zero if the match ended instantly
                    float fraction = 0.0f;
                    if (m_presence_intervals_total > 0)
                    {
                        fraction = (float)presence_count / (float)m_presence_intervals_total;
                    }
                    if      (fraction < 0.0f) fraction = 0.0f;
                    else if (fraction > 1.0f) fraction = 1.0f;  // sanity clamp

                    float seconds_played = fraction * final_time_secs;
                    if (seconds_played < 0.0f) seconds_played = 0.0f;

                    // If you want minutes:
                    float minutes_played_increment = std::round(seconds_played / 60.0f* 100.0f) / 100.0f;

                    // We gather the partial increments from m_kart_scores[i]
                    // which was just used for the text file portion.
                    const auto &p = m_kart_scores[i];

                    // Everyone increments matches_played by +1
                    int matches_participated_increment = 1;
                    float match_played_increment = std::round(fraction * (static_cast<float>(m_goal_target) / 10.0f) * 100.0f) / 100.0f;

                    // If that player’s team was the match winner => matches_won +1
                    int match_won_increment = getKartSoccerResult(i) ? 1 : 0;

                    int team_members_count_increment = getKartTeam(i) == KART_TEAM_RED ? red_count : blue_count;

                    // If no points were awarded at all, set both increments to 0
                    if (p.scoring_pts == 0 &&
                        p.attacking_pts == 0 &&
                        p.defending_pts == 0 &&
                        p.bad_play_pts == 0)
                    {
                        matches_participated_increment = 0;
                        match_won_increment = 0;
                        team_members_count_increment =0;
                        match_played_increment = 0.0f;
                    }

                    // Bind columns
                    // 1) PlayerName
                    sqlite3_bind_text(stmt, 1, player_name.c_str(), -1, SQLITE_TRANSIENT);
                    // 2) ScoringPts
                    sqlite3_bind_int(stmt, 2, p.scoring_pts);
                    // 3) AttackingPts
                    sqlite3_bind_int(stmt, 3, p.attacking_pts);
                    // 4) DefendingPts
                    sqlite3_bind_int(stmt, 4, p.defending_pts);
                    // 5) IndDefendingPts
                    sqlite3_bind_int(stmt, 5, p.inddefending_pts);
                    // 6) BadPlayPts
                    sqlite3_bind_int(stmt, 6, p.bad_play_pts);
                    // 7) Total
                    sqlite3_bind_int(stmt, 7, p.total_pts);
                    // 8) matches_played
                    sqlite3_bind_double(stmt, 8, match_played_increment);
                    // 9) matches_participated
                    sqlite3_bind_int(stmt, 9, matches_participated_increment);
                    // 10) matches_won
                    sqlite3_bind_int(stmt, 10, match_won_increment);
                    // 11) team_members_count
                    sqlite3_bind_int(stmt, 11, team_members_count_increment);
                    // 12) minutes_played_count
                    sqlite3_bind_double(stmt, 12, minutes_played_increment);

                    rc = sqlite3_step(stmt);
                    if (rc != SQLITE_DONE)
                    {
                        Log::warn("SoccerWorld",
                            (std::string("Upsert failed for player ") +
                             player_name + ": " + sqlite3_errmsg(db)).c_str());
                    }
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                } // for each kart
                sqlite3_finalize(stmt);
            }

            // 3) Recompute “Rank” as before, sorting by Total DESC
            const char* select_sql = R"(
              SELECT rowid, PlayerName, Total
              FROM players
              ORDER BY Total DESC
            )";

            sqlite3_stmt* sel_stmt = nullptr;
            rc = sqlite3_prepare_v2(db, select_sql, -1, &sel_stmt, nullptr);
            if (rc == SQLITE_OK)
            {
                int rank = 1;
                int previous_total = -9999999;
                bool first_row = true;

                while ((rc = sqlite3_step(sel_stmt)) == SQLITE_ROW)
                {
                    int rowid          = sqlite3_column_int(sel_stmt, 0);
                    int total_val      = sqlite3_column_int(sel_stmt, 2);

                    if (first_row)
                    {
                        previous_total = total_val;
                        first_row = false;
                    }
                    else if (total_val != previous_total)
                    {
                        rank++;
                        previous_total = total_val;
                    }
                    // update rank
                    const char* update_sql = "UPDATE players SET Rank=? WHERE rowid=?;";
                    sqlite3_stmt* upd_stmt = nullptr;
                    if (sqlite3_prepare_v2(db, update_sql, -1, &upd_stmt, nullptr) == SQLITE_OK)
                    {
                        sqlite3_bind_int(upd_stmt, 1, rank);
                        sqlite3_bind_int(upd_stmt, 2, rowid);

                        int rc2 = sqlite3_step(upd_stmt);
                        if (rc2 != SQLITE_DONE)
                        {
                            Log::warn("SoccerWorld",
                                (std::string("Rank UPDATE failed for rowid ") +
                                 std::to_string(rowid) + ": " + sqlite3_errmsg(db)).c_str());
                        }
                        sqlite3_finalize(upd_stmt);
                    }
                }
                sqlite3_finalize(sel_stmt);
            }
        }
        sqlite3_close(db);
    }
        }
// Step D) Write last_match_stats.txt
//   - MVP: highest total_pts
//   - Top attacker: highest attacking_pts
//   - Top defender: highest defending_pts
//   - Top scorer: highest scoring_pts
//   - If a category’s top value <= 0 => print “none”
//   - If multiple players tie for top, list them all
//   - Finally, ball possession for each team
{
    // 1) Find highest totals in each category using the helper functions above
    std::pair<int, std::vector<std::string>> mvpPair = findTopInField(m_kart_scores, &getTotalPts);
    int mvp_val = mvpPair.first;
    const std::vector<std::string>& mvp_names = mvpPair.second;

    std::pair<int, std::vector<std::string>> atkPair = findTopInField(m_kart_scores, &getAttacking);
    int atk_val = atkPair.first;
    const std::vector<std::string>& atk_names = atkPair.second;

    std::pair<int, std::vector<std::string>> defPair = findTopInField(m_kart_scores, &getDefending);
    int def_val = defPair.first;
    const std::vector<std::string>& def_names = defPair.second;

    std::pair<int, std::vector<std::string>> scrPair = findTopInField(m_kart_scores, &getScoring);
    int scr_val = scrPair.first;
    const std::vector<std::string>& scr_names = scrPair.second;

    // 2) Open last_match_stats.txt
    std::ofstream lmfile("last_match_stats.txt");
    if (!lmfile.is_open())
    {
        Log::warn("SoccerWorld", "Failed to open last_match_stats.txt for writing.");
    }
    else
    {
        lmfile << "-------------------------------------\n";
        lmfile << "Last match stats:\n";

        // MVP
        if (mvp_val <= 0)
        {
            lmfile << "MVP: none\n";
        }
        else
        {
            // Join all tied MVP names with commas
            std::ostringstream oss;
            for (size_t i = 0; i < mvp_names.size(); i++)
            {
                if (i > 0) oss << ", ";
                oss << mvp_names[i];
            }
            lmfile << "MVP: " << oss.str()
                   << "\n";
        }

         // Top scorer
        if (scr_val <= 0)
        {
            lmfile << "Top scorer: none\n";
        }
        else
        {
            std::ostringstream oss;
            for (size_t i = 0; i < scr_names.size(); i++)
            {
                if (i > 0) oss << ", ";
                oss << scr_names[i];
            }
            lmfile << "Top scorer: " << oss.str()
                   << " (Goals=" << scr_val << ")\n";
        }

        // Top attacker
        if (atk_val <= 0)
        {
            lmfile << "Top attacker: none\n";
        }
        else
        {
            std::ostringstream oss;
            for (size_t i = 0; i < atk_names.size(); i++)
            {
                if (i > 0) oss << ", ";
                oss << atk_names[i];
            }
            lmfile << "Top attacker: " << oss.str()
                   << " (shots on goal=" << atk_val << ")\n";
        }

        // Top defender
        if (def_val <= 0)
        {
            lmfile << "Top defender: none\n";
        }
        else
        {
            std::ostringstream oss;
            for (size_t i = 0; i < def_names.size(); i++)
            {
                if (i > 0) oss << ", ";
                oss << def_names[i];
            }
            lmfile << "Top defender: " << oss.str()
                   << " (direct & indirect saves=" << def_val << ")\n";
        }

        // Ball possession
        std::pair<int, int> poss = getBallPossession();  // (red%, blue%)
        int red_possession  = poss.first;
        int blue_possession = poss.second;

        lmfile << "Ball possession:\n"
               << "\U0001f7e5 Red " << red_possession
               << "% : " << blue_possession << "% Blue \U0001f7e6\n";
        lmfile << "-------------------------------------\n";

        lmfile.close();
    }
} // End of Step D

// Step E) Write soccer_ranking_detailed_sw.txt from local m_kart_scores
{
    // 1) Make a copy of the kart scores to sort them locally:
    std::vector<KartScore> sorted_scores = m_kart_scores;
    // Sort by total_pts descending
    std::sort(sorted_scores.begin(), sorted_scores.end(),
              [](const KartScore &a, const KartScore &b)
              {
                  return a.total_pts > b.total_pts;
              });

    // 2) Open the file
    std::ofstream srdfile("soccer_ranking_detailed_sw.txt");
    if (!srdfile.is_open())
    {
        Log::warn("SoccerWorld", "Failed to open soccer_ranking_detailed_sw.txt for writing.");
    }
    else
    {
        // 3) Print a header (match it to your needs)
        srdfile << "Rank  Player              Scoring_Pts  Attacking_Pts  "
                << "Defending_Pts  Bad_Play_Pts  Total\n";

        // 4) Assign ranks based on “total_pts”, handling ties
        int rank           = 1;
        int previous_total = (sorted_scores.empty()? 0 : sorted_scores[0].total_pts);
        srdfile << std::fixed; // or std::setw for alignment if you prefer

        for (size_t i = 0; i < sorted_scores.size(); i++)
        {
            const KartScore &ks = sorted_scores[i];
            // If name is empty (maybe AI?), skip
            if (ks.m_name.empty()) continue;

            // If total_pts changed, update the rank (this is “dense ranking”)
            if (i > 0 && ks.total_pts < previous_total)
            {
                rank = i + 1;
                previous_total = ks.total_pts;
            }

            //  Output one line
            srdfile << rank << " "
                    << ks.m_name << " "
                    << ks.scoring_pts << " "
                    << ks.attacking_pts << " "
                    << ks.defending_pts + ks.inddefending_pts<< " "
                    << ks.bad_play_pts << " "
                    << ks.total_pts << "\n";
        }
        srdfile.close();
    }
}


write_once = 0;
}

    if (UserConfigParams::m_arena_ai_stats)
    {
        Log::verbose("Soccer AI profiling", "Total frames elapsed for a team"
            " to win with 30 goals: %d", m_frame_count);

        // Goal time statistics
        std::sort(m_goal_frame.begin(), m_goal_frame.end());

        const int mean = std::accumulate(m_goal_frame.begin(),
            m_goal_frame.end(), 0) / (int)m_goal_frame.size();

        // Prevent overflow if there is a large frame in vector
        double squared_sum = 0;
        for (const int &i : m_goal_frame)
            squared_sum = squared_sum + (double(i - mean) * double(i - mean));

        // Use sample st. deviation (n-1) as the profiling can't be run forever
        const int stdev = int(sqrt(squared_sum / (m_goal_frame.size() - 1)));

        int median = 0;
        if (m_goal_frame.size() % 2 == 0)
        {
            median = (m_goal_frame[m_goal_frame.size() / 2 - 1] +
                m_goal_frame[m_goal_frame.size() / 2]) / 2;
        }
        else
        {
            median = m_goal_frame[m_goal_frame.size() / 2];
        }

        Log::verbose("Soccer AI profiling", "Frames elapsed for each goal:"
            " min: %d max: %d mean: %d median: %d standard deviation: %d",
            m_goal_frame.front(), m_goal_frame.back(), mean, median, stdev);

        // Goal calculation
        int red_own_goal = 0;
        int blue_own_goal = 0;
        for (unsigned i = 0; i < m_red_scorers.size(); i++)
        {
            // Notice: if a team has own goal, the score will end up in the
            // opposite team
            if (!m_red_scorers[i].m_correct_goal)
                blue_own_goal++;
        }
        for (unsigned i = 0; i < m_blue_scorers.size(); i++)
        {
            if (!m_blue_scorers[i].m_correct_goal)
                red_own_goal++;
        }

        int red_goal = ((int(m_red_scorers.size()) - blue_own_goal) >= 0 ?
            (int)m_red_scorers.size() - blue_own_goal : 0);
        int blue_goal = ((int(m_blue_scorers.size()) - red_own_goal) >= 0 ?
            (int)m_blue_scorers.size() - red_own_goal : 0);

        Log::verbose("Soccer AI profiling", "Red goal: %d, Red own goal: %d,"
            "Blue goal: %d, Blue own goal: %d", red_goal, red_own_goal,
            blue_goal, blue_own_goal);

        if (getScore(KART_TEAM_BLUE) >= m_goal_target)
            Log::verbose("Soccer AI profiling", "Blue team wins");
        else
            Log::verbose("Soccer AI profiling", "Red team wins");

        delete this;
        main_loop->abort();
    }

}   // enterRaceOverState

// ----------------------------------------------------------------------------
void SoccerWorld::saveCompleteState(BareNetworkString* bns, STKPeer* peer)
{
    const unsigned red_scorers = (unsigned)m_red_scorers.size();
    bns->addUInt32(red_scorers);
    for (unsigned i = 0; i < red_scorers; i++)
    {
        bns->addUInt8((uint8_t)m_red_scorers[i].m_id)
            .addUInt8(m_red_scorers[i].m_correct_goal)
            .addFloat(m_red_scorers[i].m_time)
            .encodeString(m_red_scorers[i].m_kart)
            .encodeString(m_red_scorers[i].m_player);
        if (peer->getClientCapabilities().find("soccer_fixes") !=
            peer->getClientCapabilities().end())
        {
            bns->encodeString(m_red_scorers[i].m_country_code)
                .addUInt8(m_red_scorers[i].m_handicap_level);
        }
    }

    const unsigned blue_scorers = (unsigned)m_blue_scorers.size();
    bns->addUInt32(blue_scorers);
    for (unsigned i = 0; i < blue_scorers; i++)
    {
        bns->addUInt8((uint8_t)m_blue_scorers[i].m_id)
            .addUInt8(m_blue_scorers[i].m_correct_goal)
            .addFloat(m_blue_scorers[i].m_time)
            .encodeString(m_blue_scorers[i].m_kart)
            .encodeString(m_blue_scorers[i].m_player);
        if (peer->getClientCapabilities().find("soccer_fixes") !=
            peer->getClientCapabilities().end())
        {
            bns->encodeString(m_blue_scorers[i].m_country_code)
                .addUInt8(m_blue_scorers[i].m_handicap_level);
        }
    }
    bns->addTime(m_reset_ball_ticks).addTime(m_ticks_back_to_own_goal);
}   // saveCompleteState

// ----------------------------------------------------------------------------
void SoccerWorld::restoreCompleteState(const BareNetworkString& b)
{
    m_red_scorers.clear();
    m_blue_scorers.clear();

    const unsigned red_size = b.getUInt32();
    for (unsigned i = 0; i < red_size; i++)
    {
        ScorerData sd;
        sd.m_id = b.getUInt8();
        sd.m_correct_goal = b.getUInt8() == 1;
        sd.m_time = b.getFloat();
        b.decodeString(&sd.m_kart);
        b.decodeStringW(&sd.m_player);
        if (NetworkConfig::get()->getServerCapabilities().find("soccer_fixes")
            != NetworkConfig::get()->getServerCapabilities().end())
        {
            b.decodeString(&sd.m_country_code);
            sd.m_handicap_level = (HandicapLevel)b.getUInt8();
        }
        m_red_scorers.push_back(sd);
    }

    const unsigned blue_size = b.getUInt32();
    for (unsigned i = 0; i < blue_size; i++)
    {
        ScorerData sd;
        sd.m_id = b.getUInt8();
        sd.m_correct_goal = b.getUInt8() == 1;
        sd.m_time = b.getFloat();
        b.decodeString(&sd.m_kart);
        b.decodeStringW(&sd.m_player);
        if (NetworkConfig::get()->getServerCapabilities().find("soccer_fixes")
            != NetworkConfig::get()->getServerCapabilities().end())
        {
            b.decodeString(&sd.m_country_code);
            sd.m_handicap_level = (HandicapLevel)b.getUInt8();
        }
        m_blue_scorers.push_back(sd);
    }
    m_reset_ball_ticks = b.getTime();
    m_ticks_back_to_own_goal = b.getTime();
}   // restoreCompleteState

// ----------------------------------------------------------------------------
float SoccerWorld::getBallDiameter() const
{
    return m_bgd->getDiameter();
}   // getBallDiameter

// ----------------------------------------------------------------------------
bool SoccerWorld::ballApproachingGoal(KartTeam team) const
{
    return m_bgd->isApproachingGoal(team);
}   // ballApproachingGoal

// ----------------------------------------------------------------------------
bool SoccerWorld::isBallMovingTowardGoal(KartTeam team) const
{
    // 1) Get ball position and velocity
    Vec3 ball_pos = getBallPosition();
    Vec3 ball_vel = getBallVelocity();

    // If the ball is barely moving, we can’t say it’s “moving toward” anything
    //if (ball_vel.length_2d() < 15.0f)
        //return false;

    // 2) Get the appropriate goal’s first and last points
    CheckGoal* check_goal = m_bgd->getCheckGoal(team);
    if (!check_goal)
    {
        Log::warn("SoccerWorld", "isBallMovingTowardGoal: Missing CheckGoal pointer!");
        return false;
    }
    Vec3 goal_first = check_goal->getPoint(CheckGoal::POINT_FIRST);
    Vec3 goal_last  = check_goal->getPoint(CheckGoal::POINT_LAST);

    // 3) Form vectors from the ball to each post
    Vec3 to_first = goal_first - ball_pos;
    Vec3 to_last  = goal_last  - ball_pos;

    // 4) We can check whether ball_vel is “between” these two vectors in the XZ plane.
    //    A quick way is to compare the cross product signs:
    //    cross(to_first, ball_vel) and cross(to_last, ball_vel).
    //    If they differ (i.e., their product is negative), then ball_vel is between them.
    float cross1 = to_first.x() * ball_vel.z() - to_first.z() * ball_vel.x();
    float cross2 = to_last.x()  * ball_vel.z() - to_last.z()  * ball_vel.x();
    bool between_posts = (cross1 * cross2 < 0.0f);

    if (between_posts)
    {
        // 5) Finally, ensure the ball velocity is actually heading *forward* toward the goal,
        //    and not behind or away. A simple check is dot(midpoint, velocity) > 0.
        Vec3 midpoint = (to_first + to_last) * 0.5f;
        float dot_mid = midpoint.dot(ball_vel);

        if (dot_mid > 0.0f)
        {
            // Optionally, you can also set a distance threshold if needed:
            float distance = (midpoint).length_2d();
            if (distance/ball_vel.length_2d() < 3.5f)
            return true;
        }
    }

    return false;
}
/*
bool SoccerWorld::isBallMovingTowardHalf(KartTeam team) const
{
    // 1) Get the current ball position, velocity, and (assumed) initial position.
    Vec3 ball_pos         = getBallPosition();
    Vec3 ball_vel         = getBallVelocity();
    Vec3 ball_initial_pos = ball_pos; // Assumes this function exists

    // Optional: you may want to ignore cases where ball is barely moving.
    // if (ball_vel.length_2d() < 15.0f)
    //     return false;

    // 2) Get this team's goal.
    CheckGoal* team_goal = m_bgd->getCheckGoal(team);
    if (!team_goal)
    {
        Log::warn("SoccerWorld", "isBallMovingTowardHalf: Missing CheckGoal pointer for team!");
        return false;
    }
    Vec3 team_goal_first = team_goal->getPoint(CheckGoal::POINT_FIRST);
    Vec3 team_goal_last  = team_goal->getPoint(CheckGoal::POINT_LAST);
    Vec3 team_goal_mid   = (team_goal_first + team_goal_last) * 0.5f;

    // 3) Determine the other team (assumes a two-team system).
    KartTeam other_team;
    if (team == KART_TEAM_RED)
        other_team = KART_TEAM_BLUE;
    else
        other_team = KART_TEAM_RED;

    CheckGoal* other_goal = m_bgd->getCheckGoal(other_team);
    if (!other_goal)
    {
        Log::warn("SoccerWorld", "isBallMovingTowardHalf: Missing CheckGoal pointer for other team!");
        return false;
    }
    Vec3 other_goal_first = other_goal->getPoint(CheckGoal::POINT_FIRST);
    Vec3 other_goal_last  = other_goal->getPoint(CheckGoal::POINT_LAST);
    Vec3 other_goal_mid   = (other_goal_first + other_goal_last) * 0.5f;

    // 4) Ensure ball's initial position is closer to the other gate than to its own.
    float distInitialToOther = (ball_initial_pos - other_goal_mid).length_2d();
    float distInitialToTeam  = (ball_initial_pos - team_goal_mid).length_2d();
    if (distInitialToOther >= distInitialToTeam)
        return false;

    // 5) Compute the half-field (midfield) point.
    //     Here we use the midpoint between both teams' goal centers.
    Vec3 half_point = (team_goal_mid + other_goal_mid) * 0.5f;

    // 6) Check if the ball is heading towards the half.
    //     We form a vector from the ball to the half point and take its dot product with the ball's velocity.
    Vec3 to_half = team_goal_mid - ball_pos;
    float dot_half = to_half.dot(ball_vel);
    if (dot_half > 0.0f)
    {
        // Optional: you may apply a threshold based on estimated arrival time.
        float remainDist = to_half.length_2d();
        if (ball_vel.length_2d() > remainDist / 0.01f)
            return true;

        // Or simply return true when heading toward half.
        return true;
    }

    return false;
}
*/
bool SoccerWorld::isBallMovingTowardHalf(KartTeam team) const
{
    // 1) Get ball position and velocity
    Vec3 ball_pos = getBallPosition();
    Vec3 ball_vel = getBallVelocity();

    // If the ball is barely moving, we can’t say it’s “moving toward” anything
    //if (ball_vel.length_2d() < 15.0f)
        //return false;

    // 2) Get the appropriate goal’s first and last points
    CheckGoal* check_goal = m_bgd->getCheckGoal(team);
    if (!check_goal)
    {
        Log::warn("SoccerWorld", "isBallMovingTowardGoal: Missing CheckGoal pointer!");
        return false;
    }
    Vec3 goal_first = check_goal->getPoint(CheckGoal::POINT_FIRST);
    Vec3 goal_last  = check_goal->getPoint(CheckGoal::POINT_LAST);
    Vec3 team_goal_mid   = (goal_first + goal_last) * 0.5f;

     // 3) Determine the other team (assumes a two-team system).
    KartTeam other_team;
    if (team == KART_TEAM_RED)
        other_team = KART_TEAM_BLUE;
    else
        other_team = KART_TEAM_RED;

    CheckGoal* other_goal = m_bgd->getCheckGoal(other_team);
    if (!other_goal)
    {
        Log::warn("SoccerWorld", "isBallMovingTowardHalf: Missing CheckGoal pointer for other team!");
        return false;
    }
    Vec3 other_goal_first = other_goal->getPoint(CheckGoal::POINT_FIRST);
    Vec3 other_goal_last  = other_goal->getPoint(CheckGoal::POINT_LAST);
    Vec3 other_goal_mid   = (other_goal_first + other_goal_last) * 0.5f;

    // 3) Form vectors from the ball to each post
    Vec3 to_first = goal_first - ball_pos;
    Vec3 to_last  = goal_last  - ball_pos;

    // 4) Ensure ball's initial position is closer to the other gate than to its own.
    float distInitialToOther = (ball_pos - other_goal_mid).length_2d();
    float distInitialToTeam  = (ball_pos - team_goal_mid).length_2d();
    if (distInitialToOther >= distInitialToTeam)
        return false;

        // 5) Finally, ensure the ball velocity is actually heading *forward* toward the goal,
        //    and not behind or away. A simple check is dot(midpoint, velocity) > 0.
        Vec3 midpoint = (to_first + to_last) * 0.5f;
        float dot_mid = midpoint.dot(ball_vel);

        if (dot_mid > 0.0f)
        {
            // Optionally, you can also set a distance threshold if needed:
            float distance = (midpoint).length_2d();
            if (distance/ball_vel.length_2d() < 4.0f)
            return true;
        }


    return false;
}

bool SoccerWorld::isBallBetweenRedAndBlueGates() const
{
    // 1) Get positions of the two goals’ center points
    Vec3 red_center  = m_bgd->getGoalCenter(KART_TEAM_RED);
    Vec3 blue_center = m_bgd->getGoalCenter(KART_TEAM_BLUE);

    // 2) Get ball position
    Vec3 ball_pos = getBallPosition();

    // 3) Vector from red_gate_center to blue_gate_center
    Vec3 red2blue = blue_center - red_center;
    // 4) Vector from red_gate_center to the ball
    Vec3 red2ball = ball_pos - red_center;

    // 5) Dot products
    //    dot_rb   tells us how far along red→blue the ball is
    //    dotmax   is the squared length of the entire red→blue segment
    float dot_rb  = red2ball.dot(red2blue);
    float dotmax  = red2blue.dot(red2blue);

    // 6) If 0 <= dot_rb <= dotmax, the ball is “between” red_center and blue_center
    //    on that line segment.  (Negative => behind red goal, bigger than dotmax => past blue goal)
    return (dot_rb >= 0.0f && dot_rb <= dotmax);
}

// ----------------------------------------------------------------------------
Vec3 SoccerWorld::getBallAimPosition(KartTeam team, bool reverse) const
{
    return m_bgd->getAimPosition(team, reverse);
}   // getBallAimPosition

// ----------------------------------------------------------------------------
/** Returns the data to display in the race gui.
 */
void SoccerWorld::getKartsDisplayInfo(
                           std::vector<RaceGUIBase::KartIconDisplayInfo> *info)
{
    if (!UserConfigParams::m_soccer_player_list)
        return;
    const unsigned int kart_amount = getNumKarts();
    for (unsigned int i = 0; i < kart_amount ; i++)
    {
        RaceGUIBase::KartIconDisplayInfo& rank_info = (*info)[i];
        rank_info.lap = -1;
        rank_info.m_outlined_font = true;
        rank_info.m_color = getKartTeam(i) == KART_TEAM_RED ?
            video::SColor(255, 255, 0, 0) : video::SColor(255, 0, 0, 255);
        rank_info.m_text = getKart(i)->getController()->getName();
        if (RaceManager::get()->getKartGlobalPlayerId(i) > -1)
        {
            const core::stringw& flag = StringUtils::getCountryFlag(
                RaceManager::get()->getKartInfo(i).getCountryCode());
            if (!flag.empty())
            {
                rank_info.m_text += L" ";
                rank_info.m_text += flag;
            }
        }
    }
}   // getKartsDisplayInfo

std::pair<int, int> SoccerWorld::getBallPossession() const
{
    int total = m_ball_possession_red + m_ball_possession_blue;
    if (total <= 0)
    {
        // No one ever hit the ball, or no valid data
        return std::make_pair(0, 0);
    }

    int red_percent  = 100 * m_ball_possession_red  / total;
    int blue_percent = 100 - red_percent;

    return std::make_pair(red_percent, blue_percent);
} // getBallPossession
