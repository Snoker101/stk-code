//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2016 SuperTuxKart-Team
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

#include "karts/controller/soccer_ai.hpp"

#include "items/attachment.hpp"
#include "items/flyable.hpp"
#include "items/powerup.hpp"
#include "items/projectile_manager.hpp"
#include "karts/abstract_kart.hpp"
#include "karts/controller/kart_control.hpp"
#include "karts/kart_properties.hpp"
#include "karts/rescue_animation.hpp"
#include "karts/controller/player_controller.hpp"
#include "modes/soccer_world.hpp"
#include "network/network_config.hpp"
#include "tracks/arena_graph.hpp"
#include "tracks/arena_node.hpp"
#include "tracks/track.hpp"

#include <algorithm>
#include <cmath>

#ifdef AI_DEBUG
#include "irrlicht.h"
#endif

#ifdef BALL_AIM_DEBUG
#include "graphics/camera/camera.hpp"
#endif

bool SoccerAI::m_baseline_red = false;
bool SoccerAI::m_baseline_blue = false;

namespace
{
    /** How much rolling slows down the ball over time, measured in matches.
     *  It doesn't slow down in the air. */
    const float BALL_DRAG = 0.157f;
    /** Radius of a bowling ball. */
    const float BOWLING_RADIUS = 0.4f;
    /** The speed of a bowling ball is at least this when fired forward, and
     *  this with a kart standing still when fired backward. */
    const float BOWLING_MIN_SPEED = 32.0f;
    const float BOWLING_BACK_SPEED = 16.0f;
    /** Bowling ball speed above the speed of the kart. */
    const float BOWLING_ADD_SPEED = 8.0f;
    /** A bowling ball is not fired at anything further than this. */
    const float BOWLING_MAX_RANGE = 45.0f;
    /** Time between bowling balls at the soccer ball and at karts. */
    const float BALL_SHOT_INTERVAL = 0.5f;
    const float KART_SHOT_INTERVAL = 4.0f;
    /** If nobody touched the ball after this time, everyone may go for it. */
    const float KICKOFF_MAX_WAIT = 6.0f;
    /** The others keep waiting this long after the first touch, for the ball
     *  to come out of the fight between the strikers. */
    const float KICKOFF_TOUCH_WAIT = 1.5f;
    /** A kart that moved less than this distance in this time needs a rescue.*/
    const float STUCK_DISTANCE = 2.5f;
    const float STUCK_TIME = 3.0f;
    /** A hit from the side is good enough when the ball is this close to the
     *  goal, instead of going around the ball for a better one. */
    const float SIDE_HIT_GOAL_DISTANCE = 45.0f;
    /** A jump at the ball is tried from this distance, and the ball must be
     *  this high above the kart. The same limits as for the player jump. */
    const float JUMP_RANGE = 18.0f;
    const float JUMP_MIN_HEIGHT = 2.0f;

    // ------------------------------------------------------------------------
    /** Returns a vector without height. */
    Vec3 flat(const Vec3& v)
    {
        return Vec3(v.x(), 0.0f, v.z());
    }   // flat

    // ------------------------------------------------------------------------
    /** Returns a vector without height of length 1, or 0 if it has no length.*/
    Vec3 flatNormal(const Vec3& v)
    {
        Vec3 result = flat(v);
        const float length = result.length();
        return length > 0.001f ? Vec3(result / length) : Vec3(0, 0, 0);
    }   // flatNormal
}   // namespace

SoccerAI::SoccerAI(AbstractKart *kart)
        : ArenaAI(kart)
{

    reset();

#ifdef AI_DEBUG
    video::SColor col_debug(128, 128, 0, 0);
    video::SColor col_debug_next(128, 0, 128, 128);
    m_debug_sphere = irr_driver->addSphere(1.0f, col_debug);
    m_debug_sphere->setVisible(true);
    m_debug_sphere_next = irr_driver->addSphere(1.0f, col_debug_next);
    m_debug_sphere_next->setVisible(true);
#endif

#ifdef BALL_AIM_DEBUG
    video::SColor red(128, 128, 0, 0);
    video::SColor blue(128, 0, 0, 128);
    m_red_sphere = irr_driver->addSphere(1.0f, red);
    m_red_sphere->setVisible(true);
    m_blue_sphere = irr_driver->addSphere(1.0f, blue);
    m_blue_sphere->setVisible(true);
#endif

    m_world = dynamic_cast<SoccerWorld*>(World::getWorld());
    m_track = Track::getCurrentTrack();
    m_cur_team = m_world->getKartTeam(m_kart->getWorldKartId());
    m_opp_team = (m_cur_team == KART_TEAM_BLUE ?
        KART_TEAM_RED : KART_TEAM_BLUE);

    // Don't call our own setControllerName, since this will add a
    // billboard showing 'AIBaseController' to the kart.
    Controller::setControllerName("SoccerAI");

    setupSkill();
}   // SoccerAI

//-----------------------------------------------------------------------------
/** Set what this AI can do, depending on the difficulty.
 */
void SoccerAI::setupSkill()
{
    const RaceManager::Difficulty difficulty =
        RaceManager::get()->getDifficulty();
    const bool baseline = m_cur_team == KART_TEAM_RED ? m_baseline_red
                                                      : m_baseline_blue;

    m_full_ai = !baseline && difficulty != RaceManager::DIFFICULTY_EASY;
    m_use_nitro_skid = m_use_ball_shots = m_use_jump = false;
    m_nitro_regen = 0.0f;
    if (!m_full_ai)
        return;

    // Intermediate plays well but gets no help at all
    m_use_nitro_skid = m_use_ball_shots = true;
    if (difficulty == RaceManager::DIFFICULTY_BEST)
    {
        m_use_jump = true;
        m_nitro_regen = 0.0f;
    }
}   // setupSkill

//-----------------------------------------------------------------------------
SoccerAI::~SoccerAI()
{
#ifdef AI_DEBUG
    irr_driver->removeNode(m_debug_sphere);
    irr_driver->removeNode(m_debug_sphere_next);
#endif

#ifdef BALL_AIM_DEBUG
    irr_driver->removeNode(m_red_sphere);
    irr_driver->removeNode(m_blue_sphere);
#endif

}   //  ~SoccerAI

//-----------------------------------------------------------------------------
/** Resets the AI when a race is restarted.
 */
void SoccerAI::reset()
{
    ArenaAI::reset();

    m_overtake_ball = false;
    m_force_brake = false;
    m_chasing_ball = false;
    m_striking = false;
    m_playing_ball = false;
    m_direct = false;
    m_hold_position = false;
    m_saving = false;
    m_shot_dir = Vec3(0, 0, 0);
    m_time_since_shot = BALL_SHOT_INTERVAL;
    m_time_since_kart_shot = KART_SHOT_INTERVAL;
    m_kickoff_time = 0.0f;
    m_time_since_first_touch = -1.0f;
    m_jump_cooldown = 0.0f;
    m_blocked_time = 0.0f;
    m_debug_mode = "";
    m_still_time = 0.0f;
    m_still_position = flat(m_kart->getXYZ());
    m_around_side = 1.0f;

    m_front_transform.setOrigin(m_kart->getFrontXYZ());
    m_front_transform.setBasis(m_kart->getTrans().getBasis());

}   // reset

//-----------------------------------------------------------------------------
/** Update \ref m_front_transform for ball aiming functions, also make AI stop
 *  after goal.
 *  \param dt Time step size.
 */
void SoccerAI::update(int ticks)
{
#ifdef BALL_AIM_DEBUG
    Vec3 red = m_world->getBallAimPosition(KART_TEAM_RED);
    Vec3 blue = m_world->getBallAimPosition(KART_TEAM_BLUE);
    m_red_sphere->setPosition(red.toIrrVector());
    m_blue_sphere->setPosition(blue.toIrrVector());
#endif
    m_force_brake = false;
    m_chasing_ball = false;
    m_front_transform.setOrigin(m_kart->getFrontXYZ());
    m_front_transform.setBasis(m_kart->getTrans().getBasis());

    const float dt = stk_config->ticks2Time(ticks);
    if (m_world->isGoalPhase() || m_world->isStartPhase())
    {
        m_kickoff_time = 0.0f;
        m_time_since_first_touch = -1.0f;
    }
    else if (m_world->isBallUntouched())
    {
        m_kickoff_time += dt;
    }
    else if (m_time_since_first_touch < 0.0f)
    {
        // Normal play if the ball was hit long ago, like after a rescue of it
        m_time_since_first_touch = m_kickoff_time > 0.0f ? 0.0f :
            KICKOFF_TOUCH_WAIT;
        m_kickoff_time = 0.0f;
    }
    else
    {
        m_time_since_first_touch += dt;
    }

    if (m_world->isGoalPhase())
    {
        // The karts are put back for the kickoff, so whatever they were in
        // the middle of, like reversing to turn around, is over
        ArenaAI::reset();
        resetAfterStop();
        m_controls->setBrake(false);
        m_controls->setAccel(0.0f);
        AIBaseController::update(ticks);
        return;
    }

    if (m_full_ai && !m_world->isStartPhase() && !m_kart->getKartAnimation() &&
        !NetworkConfig::get()->isNetworking())
    {
        applyHelp(stk_config->ticks2Time(ticks));
    }
    // Pushing against a post or a wall doesn't move the kart: back up from it
    // instead of waiting for a rescue, which can put it in a bad place like
    // inside the goal. Pushing the ball isn't being blocked.
    if (m_full_ai && m_controls->getAccel() > 0.0f &&
        fabsf(m_kart->getSpeed()) < 1.0f && !m_hold_position &&
        !m_kart->getKartAnimation() && !m_world->isStartPhase() &&
        (m_world->getBallPosition() - m_kart->getXYZ()).length() > 4.0f)
        m_blocked_time += dt;
    else
        m_blocked_time = 0.0f;
    ArenaAI::update(ticks);

    if (m_full_ai && !m_world->isStartPhase() && !m_kart->getKartAnimation())
    {
        const Vec3 position = flat(m_kart->getXYZ());
        m_still_time += dt;
        if ((position - m_still_position).length() > STUCK_DISTANCE ||
            m_hold_position)
        {
            m_still_position = position;
            m_still_time = 0.0f;
        }
        if (m_still_time > STUCK_TIME)
        {
            // Backing up didn't help
            m_still_time = 0.0f;
            RescueAnimation::create(m_kart);
            return;
        }
    }

    if (m_use_jump && !m_world->isStartPhase())
        tryBallJump();
}   // update

//-----------------------------------------------------------------------------
/** True if the kart has been trying to drive without moving for a while.
 */
bool SoccerAI::isBlocked() const
{
    return m_blocked_time > 0.8f;
}   // isBlocked

//-----------------------------------------------------------------------------
/** Gives this AI the extra nitro of its difficulty.
 *  \param dt Time step size.
 */
void SoccerAI::applyHelp(float dt)
{
    m_time_since_shot += dt;
    m_time_since_kart_shot += dt;
    m_jump_cooldown -= dt;

    if (m_nitro_regen > 0.0f)
    {
        const float max_energy = m_kart->getKartProperties()->getNitroMax();
        const float energy = m_kart->getEnergy() + m_nitro_regen * dt;
        m_kart->setEnergy(std::min(energy, max_energy));
    }
}   // applyHelp

//-----------------------------------------------------------------------------
/** Predicts where the ball will be, ignoring any bounces and walls.
 *  \param time Time in seconds from now.
 */
Vec3 SoccerAI::predictBall(float time) const
{
    const Vec3& pos = m_world->getBallPosition();
    const Vec3& vel = m_world->getBallVelocity();
    const float gravity = m_track->getGravity();
    const float rest = m_kart->getXYZ().y() + m_world->getBallDiameter() / 2;

    float air_time = 0.0f;
    if (pos.y() - rest > 0.3f || vel.y() > 1.0f)
    {
        air_time = (vel.y() + sqrtf(vel.y() * vel.y() +
            2.0f * gravity * std::max(pos.y() - rest, 0.0f))) / gravity;
    }
    const float flying = std::min(time, air_time);
    const float rolling = time - flying;
    const float moved = flying + (1.0f - expf(-BALL_DRAG * rolling)) / BALL_DRAG;
    const float height = pos.y() + vel.y() * flying -
        0.5f * gravity * flying * flying;
    return Vec3(pos.x() + vel.x() * moved,
        rolling > 0.0f ? rest : std::max(height, rest),
        pos.z() + vel.z() * moved);
}   // predictBall

//-----------------------------------------------------------------------------
/** Finds the ArenaNode of a point, searching close to the ball first.
 *  \return The node, or \ref Graph::UNKNOWN_SECTOR if it's outside the arena.
 */
int SoccerAI::findNode(const Vec3& point) const
{
    int node = m_world->getBallNode();
    std::vector<int>* nearby = NULL;
    if (node != Graph::UNKNOWN_SECTOR)
    {
        nearby = m_graph->getNode(node)->getNearbyNodes();
        m_graph->findRoadSector(point, &node, nearby,
            true/*ignore_vertical*/);
    }
    if (node == Graph::UNKNOWN_SECTOR)
    {
        m_graph->findRoadSector(point, &node, NULL,
            true/*ignore_vertical*/);
    }
    return node;
}   // findNode

//-----------------------------------------------------------------------------
/** Finds where this kart can meet the ball, if the ball is in the air this is
 *  where it will come down when the kart would be too early otherwise.
 *  \param[out] node ArenaNode of the returned point.
 *  \return The point on the ball path, always inside the arena.
 */
Vec3 SoccerAI::findBallIntercept(int* node) const
{
    const float speed = std::max(m_kart->getSpeed(), 15.0f);
    const Vec3 front = m_kart->getFrontXYZ();
    float time = 0.0f;
    for (int i = 0; i < 3; i++)
    {
        time = std::min((flat(predictBall(time)) - flat(front)).length() /
            speed, 2.0f);
    }

    const Vec3& pos = m_world->getBallPosition();
    const float above = pos.y() - m_kart->getXYZ().y() -
        m_world->getBallDiameter();
    if (above > 1.5f)
    {
        const float vy = m_world->getBallVelocity().y();
        const float gravity = m_track->getGravity();
        const float land = (vy + sqrtf(vy * vy + 2.0f * gravity * above)) /
            gravity;
        time = std::max(time, std::min(land, 4.0f));
    }

    // A ball going out of the arena will bounce back at some point before
    for (float t : { time, time * 0.5f })
    {
        const Vec3 predicted = predictBall(t);
        *node = findNode(predicted);
        if (*node != Graph::UNKNOWN_SECTOR)
            return predicted;
    }
    *node = m_world->getBallNode();
    return pos;
}   // findBallIntercept

//-----------------------------------------------------------------------------
/** Sets the target and if the way there is free. It's free if the path is
 *  about as long as a straight line, as there is nothing to go around then.
 *  \param point The point to drive to.
 *  \param node ArenaNode of that point.
 */
void SoccerAI::setTarget(const Vec3& point, int node)
{
    m_target_point = point;
    m_target_node = node;

    const int current = getCurrentNode();
    if (current == Graph::UNKNOWN_SECTOR || node == Graph::UNKNOWN_SECTOR)
    {
        m_direct = false;
        return;
    }
    const float straight = (flat(point) - flat(m_kart->getXYZ())).length();
    m_direct = current == node ||
        m_graph->getDistance(current, node) < straight * 1.25f + 3.0f;

    // Slow down for a sharp turn close to the target if can't skid through it
    Vec3 lc = m_kart->getTrans().inverse()(point);
    const float angle = atan2f(fabsf(lc.x()), lc.z());
    m_force_brake = !m_use_nitro_skid && !m_hold_position && m_direct &&
        straight < 10.0f && angle > 0.9f && m_kart->getSpeed() > 14.0f;
}   // setTarget

//-----------------------------------------------------------------------------
/** Check if an opposing kart could get to the ball before this one.
 *  \param ball Position of the ball.
 *  \param my_distance Distance from this kart to the ball.
 */
bool SoccerAI::isUnderPressure(const Vec3& ball, float my_distance) const
{
    for (unsigned int i = 0; i < m_world->getNumKarts(); i++)
    {
        const AbstractKart* kart = m_world->getKart(i);
        if (m_world->getKartTeam(i) == m_cur_team || kart->getKartAnimation())
            continue;
        const float distance = (flat(ball) - flat(kart->getXYZ())).length();
        if (distance < 7.0f || distance < my_distance - 2.0f)
            return true;
    }
    return false;
}   // isUnderPressure

//-----------------------------------------------------------------------------
/** Plays the ball: hit it if that makes it go in a useful direction, otherwise
 *  get to the other side of the ball first without touching it.
 */
void SoccerAI::playBall()
{
    m_playing_ball = true;

    if (m_world->isBallMovingTowardGoal(m_cur_team))
    {
        doSave();
        return;
    }

    // A kart that can jump doesn't have to wait for a ball in the air to come
    // down: it only has to get close enough to the ball's path while the ball
    // is still high, the jump there is done by tryBallJump
    if (m_use_jump && m_jump_cooldown <= 0.0f)
    {
        const Vec3 kart = flat(m_kart->getXYZ());
        const float speed = std::max(m_kart->getSpeed(), 12.0f);
        for (int i = 1; i <= 30; i++)
        {
            const float time = i * 0.1f;
            Vec3 point = predictBall(time);
            if (point.y() - m_kart->getXYZ().y() < JUMP_MIN_HEIGHT)
                break;
            const float distance = (flat(point) - kart).length();
            if (std::max(distance - (JUMP_RANGE - 2.5f), 0.0f) / speed <= time)
            {
                point.setY(m_kart->getXYZ().y());
                const int node = findNode(point);
                if (node == Graph::UNKNOWN_SECTOR)
                    break;
                setTarget(point, node);
                m_direct = true;
                m_debug_mode = "air_chase";
                return;
            }
        }
    }

    int ball_node = Graph::UNKNOWN_SECTOR;
    const Vec3 ball = findBallIntercept(&ball_node);

    const Vec3 own_goal = m_world->getGoalCenter(m_cur_team);
    const Vec3 opp_goal = m_world->getGoalCenter(m_opp_team);
    const float field = (flat(opp_goal) - flat(own_goal)).length();
    const float own_goal_distance = (flat(ball) - flat(own_goal)).length();
    const Vec3 to_own_goal = flatNormal(own_goal - ball);

    // Towards the goal, but from too flat an angle the ball would only go into
    // the corner, cross it to the space in front of the goal instead
    const Vec3 into_field = flatNormal(own_goal - opp_goal);
    const Vec3 from_goal = flat(ball) - flat(opp_goal);
    const float depth = from_goal.dot(into_field);
    const float wide = (from_goal - into_field * depth).length() -
        m_world->getGoalHalfWidth(m_opp_team) * 0.6f;
    const float cross = std::min(std::max(wide - depth * 0.5f, 0.0f), 20.0f);
    m_shot_dir = flatNormal(opp_goal + into_field * cross - ball);

    // Close to own goal any way away from it is good
    const float danger = 1.0f - own_goal_distance / (field * 0.3f);
    if (danger > 0.0f)
    {
        m_shot_dir = flatNormal(m_shot_dir * (1.0f - danger) -
            to_own_goal * danger);
    }

    const Vec3 kart = flat(m_kart->getXYZ());
    const Vec3 to_ball = flat(ball) - kart;
    const float distance = to_ball.length();
    const Vec3 hit_dir = flatNormal(to_ball);

    // Next to a wall there may be no room to shoot the way we want, use the
    // closest direction that can be reached instead
    const float ball_radius = m_world->getBallDiameter() / 2;
    const float clearance = ball_radius + m_kart_length * 0.5f + 1.5f;
    const float standoff =
        std::min(std::max(distance * 0.5f, clearance + 3.0f), 14.0f);
    int behind_node = findNode(ball - m_shot_dir * standoff);
    if (behind_node == Graph::UNKNOWN_SECTOR)
    {
        const Vec3 wanted = m_shot_dir;
        for (float angle : { 0.5f, 1.0f, 1.5f, 2.0f })
        {
            for (float sign : { 1.0f, -1.0f })
            {
                const float c = cosf(angle * sign), s = sinf(angle * sign);
                const Vec3 dir(wanted.x() * c - wanted.z() * s, 0.0f,
                    wanted.x() * s + wanted.z() * c);
                // Prefer the side which doesn't go back to own goal
                if (dir.dot(to_own_goal) > 0.5f)
                    continue;
                behind_node = findNode(ball - dir * standoff);
                if (behind_node != Graph::UNKNOWN_SECTOR)
                {
                    m_shot_dir = dir;
                    break;
                }
            }
            if (behind_node != Graph::UNKNOWN_SECTOR)
                break;
        }
    }

    // Accept a worse hit if someone else is going to get the ball, but not one
    // that is likely to go to own goal. Close to the goal it has to be accurate
    const float opp_goal_distance = (flat(ball) - flat(opp_goal)).length();
    const bool close_to_goal = opp_goal_distance < field * 0.35f;
    const float min_alignment = isUnderPressure(ball, distance) ? 0.4f :
        close_to_goal ? 0.85f : 0.7f;
    const float own_goal_limit = own_goal_distance < field * 0.25f ? 0.2f :
        own_goal_distance < field * 0.6f ? 0.5f : 2.0f;
    m_striking = hit_dir.dot(m_shot_dir) >= min_alignment &&
        hit_dir.dot(to_own_goal) < own_goal_limit;

    // A keeper close to its goal just clears the ball straight away, any
    // direction is fine if it doesn't go in: going around the ball for a
    // better one gives an opponent time to get past
    if (m_world->getAIRole(m_kart->getWorldKartId()) ==
        SoccerWorld::AI_ROLE_KEEPER && own_goal_distance < field * 0.3f)
    {
        bool into_own_goal = false;
        const float toward = hit_dir.dot(into_field);
        if (toward > 0.05f)
        {
            const Vec3 side(into_field.z(), 0.0f, -into_field.x());
            const float to_line = (flat(own_goal) - flat(ball)).dot(into_field);
            const float at_line = (flat(ball) + hit_dir * (to_line / toward) -
                flat(own_goal)).dot(side);
            into_own_goal = fabsf(at_line) <
                m_world->getGoalHalfWidth(m_cur_team) * 1.4f;
        }
        if (!into_own_goal && hit_dir.dot(to_own_goal) < 0.6f)
        {
            m_striking = true;
            m_shot_dir = hit_dir;
            setTarget(ball, ball_node);
            m_debug_mode = "keeper_clear";
            return;
        }
    }

    // If a hit straight through the middle of the ball from where the kart is
    // goes in the goal, that's all that's needed
    const float closing = -hit_dir.dot(into_field);
    if (closing > 0.1f && depth < SIDE_HIT_GOAL_DISTANCE)
    {
        const Vec3 side(into_field.z(), 0.0f, -into_field.x());
        const float at_goal =
            (from_goal + hit_dir * (depth / closing)).dot(side);
        if (fabsf(at_goal) < m_world->getGoalHalfWidth(m_opp_team) * 0.8f)
        {
            m_striking = true;
            setTarget(ball, ball_node);
            m_debug_mode = "shoot_straight";
            return;
        }
    }

    // Near the goal hit it from the side instead of going around it: the
    // contact point below makes it go the right way
    if (depth < SIDE_HIT_GOAL_DISTANCE && hit_dir.dot(m_shot_dir) > 0.1f)
        m_striking = true;

    if (m_striking)
    {
        // A ball goes away from the center of the kart on the line through them
        // when they touch, so to send it in a direction the center of the kart
        // must get to the point behind the ball on that line. Once touching it
        // just keep pushing.
        const float contact = ball_radius + m_kart_length * 0.5f;
        if (distance < contact + 0.5f)
            setTarget(ball + m_shot_dir * ball_radius, ball_node);
        else
            setTarget(ball - m_shot_dir * contact, ball_node);
        m_debug_mode = "strike";
        return;
    }

    m_debug_mode = "go_around";
    driveBehindBall(ball, m_shot_dir, clearance, behind_node);

    // Don't arrive so fast that the kart can't turn onto the line
    if (distance < 18.0f && m_kart->getSpeed() > 20.0f)
    {
        const Vec3 lc = m_kart->getTrans().inverse()(m_target_point);
        if (atan2f(fabsf(lc.x()), lc.z()) > 0.5f)
            m_force_brake = true;
    }
}   // playBall

//-----------------------------------------------------------------------------
/** Get to the spot straight behind the ball, so that driving forward then
 *  pushes it in the wanted direction. The target is a point on a circle around
 *  the ball which is always a bit further along toward that spot, on the side
 *  the kart is on: it can't jump around and it never leads through the ball.
 *  \param ball Position of the ball.
 *  \param dir Direction the ball should go.
 *  \param clearance Distance from the ball that doesn't touch it.
 *  \param fallback_node ArenaNode to use if the point isn't in the arena.
 */
void SoccerAI::driveBehindBall(const Vec3& ball, const Vec3& dir,
                               float clearance, int fallback_node)
{
    const Vec3 side(dir.z(), 0.0f, -dir.x());
    const Vec3 from_ball = flat(m_kart->getXYZ()) - flat(ball);
    const float x = from_ball.dot(side);
    const float z = from_ball.dot(dir);
    // 0 is straight behind the ball, +-PI straight in front of it
    const float angle = atan2f(x, -z);

    // Only change side when clearly on the other one, not right in front
    if (fabsf(angle) < 2.6f)
        m_around_side = angle < 0.0f ? -1.0f : 1.0f;
    const float absolute = fabsf(angle) < 2.6f ? fabsf(angle) :
        (angle * m_around_side > 0.0f ? fabsf(angle) :
         2.0f * (float)M_PI - fabsf(angle));

    const float radius = clearance + 1.5f;
    for (float try_side : { m_around_side, -m_around_side })
    {
        const float next = (try_side == m_around_side ?
            std::max(absolute - 1.0f, 0.0f) : 1.0f) * try_side;
        Vec3 point = ball + side * (radius * sinf(next)) -
            dir * (radius * cosf(next));
        point.setY(m_kart->getXYZ().y());
        const int node = findNode(point);
        if (node != Graph::UNKNOWN_SECTOR)
        {
            setTarget(point, node);
            return;
        }
    }
    setTargetAvoidingBall(ball - dir * radius, fallback_node);
}   // driveBehindBall

//-----------------------------------------------------------------------------
/** A kart too fast to turn onto its target just goes around it in circles, so
 *  slow down enough when playing the ball. But close to a target that is nearly
 *  straight ahead even a small correction looks like a tight turn: braking for
 *  that wastes a lot of time, so only do it for real turns.
 */
bool SoccerAI::brakeForTurns() const
{
    if (!m_playing_ball || m_saving)
        return false;
    const Vec3 lc = m_kart->getTrans().inverse()(m_target_point);
    const float angle = atan2f(fabsf(lc.x()), lc.z());
    return angle >= (m_striking ? 0.5f : 0.3f);
}   // brakeForTurns

//-----------------------------------------------------------------------------
/** Getting back up to speed takes long, so don't slow down more than needed to
 *  make tight turns when playing the ball.
 */
float SoccerAI::getMinBrakeSpeed() const
{
    return m_playing_ball ? 7.0f : 5.0f;
}   // getMinBrakeSpeed

//-----------------------------------------------------------------------------
/** Don't reverse for long near the ball: it moves, so the turn will be wrong.
 */
float SoccerAI::getMaxUTurnTime() const
{
    return m_full_ai ? 0.5f : 1.5f;
}   // getMaxUTurnTime

//-----------------------------------------------------------------------------
/** Stop a ball that is going to own goal: there is no time to set up a shot.
 *  A kart already in its way just stays there. Otherwise get in its way at the
 *  first point of its path this kart can reach before the ball, and wait there
 *  if early. If it's too late to get in front of it, hit it from the inside to
 *  send it past the post.
 */
void SoccerAI::doSave()
{
    m_saving = true;

    const Vec3 ball = m_world->getBallPosition();
    const Vec3 own_goal = m_world->getGoalCenter(m_cur_team);
    const Vec3 kart = flat(m_kart->getXYZ());
    const Vec3 away = flatNormal(ball - own_goal);
    m_shot_dir = away;

    const float closing = -flat(m_world->getBallVelocity()).dot(away);
    const float goal_time = closing > 0.1f ?
        std::min((flat(ball) - flat(own_goal)).length() / closing, 3.5f) : 0.0f;
    const float speed = std::max(fabsf(m_kart->getSpeed()), 15.0f);
    const int STEPS = 20;

    // The ball is going to hit this kart: driving anywhere, even forward to
    // meet it, could only open a gap. But an opponent close to the ball can
    // still steer it around a kart that just waits, if it's not just chasing
    // a ball that is faster than it
    bool opponent_near = false;
    const Vec3 ball_velocity = flat(m_world->getBallVelocity());
    for (unsigned int i = 0; i < m_world->getNumKarts(); i++)
    {
        const AbstractKart* opponent = m_world->getKart(i);
        if (m_world->getKartTeam(i) == m_cur_team ||
            (flat(opponent->getXYZ()) - flat(ball)).length() >= 8.0f)
            continue;
        const Vec3 to_ball = flatNormal(ball - opponent->getXYZ());
        const float closing =
            (flat(opponent->getVelocity()) - ball_velocity).dot(to_ball);
        if (closing > 1.0f || ball_velocity.dot(to_ball) < 0.0f)
            opponent_near = true;
    }
    const float block = m_kart->getKartWidth() * 0.5f +
        m_world->getBallDiameter() * 0.35f;
    for (int i = 1; i <= STEPS && !opponent_near; i++)
    {
        const Vec3 point = predictBall(goal_time * i / STEPS);
        if ((flat(point) - kart).length() < block)
        {
            m_hold_position = true;
            m_force_brake = m_kart->getSpeed() > 3.0f;
            m_target_point = ball;
            m_target_node = m_world->getBallNode();
            m_direct = true;
            m_debug_mode = "save_block";
            return;
        }
    }

    for (int i = 0; i <= STEPS; i++)
    {
        const float time = goal_time * i / STEPS;
        const Vec3 point = predictBall(time);
        const float distance = (flat(point) - kart).length();
        const float spare_time = time - distance / speed;
        if (spare_time < 0.0f)
            continue;
        const int node = findNode(point);
        if (node == Graph::UNKNOWN_SECTOR)
            continue;

        // In its way with time left, and it isn't time to go and meet it yet
        const float to_ball = (flat(ball) - kart).length();
        if (distance < 3.0f && spare_time > 0.5f && to_ball > 12.0f)
        {
            driveToPosition(point, ball, m_world->getBallNode());
            m_debug_mode = "save_wait";
            return;
        }
        setTarget(point, node);
        m_direct = true;
        m_debug_mode = "save_meet";
        return;
    }

    // Too late for that. Getting closer from behind must not push it in
    const Vec3 line_end = predictBall(goal_time);
    const Vec3 to_post = flatNormal(line_end - own_goal);
    const Vec3 side_hit =
        ball - to_post * (m_world->getBallDiameter() / 2);
    setTarget(side_hit, m_world->getBallNode());
    m_direct = true;
    m_debug_mode = "save_late";
}   // doSave

//-----------------------------------------------------------------------------
/** Sets the target, going around the ball if it's on the way there: driving
 *  through it would kick it in a random direction, often to own goal.
 *  \param point The point to drive to.
 *  \param node ArenaNode of that point.
 */
void SoccerAI::setTargetAvoidingBall(const Vec3& point, int node)
{
    const Vec3 ball = flat(m_world->getBallPosition());
    const Vec3 kart = flat(m_kart->getXYZ());
    const float clearance = m_world->getBallDiameter() / 2 +
        m_kart_length * 0.5f + 1.5f;

    const Vec3 line = flat(point) - kart;
    const float line_length2 = line.length2();
    if (line_length2 > 0.0f)
    {
        const float along = (ball - kart).dot(line) / line_length2;
        const Vec3 closest = kart + line * std::min(std::max(along, 0.0f), 1.0f);
        Vec3 away = closest - ball;
        if (away.length() < clearance)
        {
            // Pass on the side the kart already is, or if the ball is exactly
            // on the line either side
            if (away.length2() < 0.01f)
                away = Vec3(line.z(), 0.0f, -line.x());
            for (float side : { 1.0f, -1.0f })
            {
                const Vec3 around = ball + flatNormal(away) * side *
                    (clearance + 1.5f);
                const int around_node = findNode(around);
                if (around_node != Graph::UNKNOWN_SECTOR)
                {
                    setTarget(around, around_node);
                    return;
                }
            }
        }
    }
    setTarget(point, node);
}   // setTargetAvoidingBall

//-----------------------------------------------------------------------------
/** A keeper leaves its position if the ball is very close or going to goal.
 *  Close is measured from the goal mouth, not its center: otherwise a ball
 *  coming along a side wall only counts once it's past the keeper. Where the
 *  ball is about to be counts, so the keeper isn't too late for a fast one.
 */
bool SoccerAI::shouldKeeperPlayBall() const
{
    const Vec3 own_goal = flat(m_world->getGoalCenter(m_cur_team));
    const Vec3 opp_goal = flat(m_world->getGoalCenter(m_opp_team));
    const float field = (opp_goal - own_goal).length();
    const Vec3 out = flatNormal(opp_goal - own_goal);
    const Vec3 side(out.z(), 0.0f, -out.x());
    const float half_width = m_world->getGoalHalfWidth(m_cur_team);

    const Vec3 ball = flat(m_world->getBallPosition()) +
        flat(m_world->getBallVelocity()) * 0.7f;
    const float across = std::max(std::min((ball - own_goal).dot(side),
        half_width), -half_width);
    const float mouth_distance = (ball - (own_goal + side * across)).length();
    return mouth_distance < field * 0.22f ||
        m_world->isBallMovingTowardGoal(m_cur_team);
}   // shouldKeeperPlayBall

//-----------------------------------------------------------------------------
/** True if the ball going from a point in a direction would go into own goal
 *  or close to it.
 */
bool SoccerAI::isShotAtOwnGoal(const Vec3& from, const Vec3& dir) const
{
    const Vec3 own_goal = flat(m_world->getGoalCenter(m_cur_team));
    const Vec3 out =
        flatNormal(flat(m_world->getGoalCenter(m_opp_team)) - own_goal);
    const Vec3 side(out.z(), 0.0f, -out.x());
    const float toward = -dir.dot(out);
    if (toward < 0.2f)
        return false;
    const float depth = (flat(from) - own_goal).dot(out);
    const float across =
        (flat(from) + dir * (depth / toward) - own_goal).dot(side);
    return fabsf(across) < m_world->getGoalHalfWidth(m_cur_team) * 1.3f;
}   // isShotAtOwnGoal

//-----------------------------------------------------------------------------
/** The point on the way of a shot at own goal that is a distance in front of
 *  the goal.
 */
Vec3 SoccerAI::getPointOnShotLine(const Vec3& from, const Vec3& dir,
                                  float from_goal) const
{
    const Vec3 own_goal = flat(m_world->getGoalCenter(m_cur_team));
    const Vec3 out =
        flatNormal(flat(m_world->getGoalCenter(m_opp_team)) - own_goal);
    const float toward = std::max(-dir.dot(out), 0.2f);
    const float depth = (flat(from) - own_goal).dot(out);
    Vec3 point = flat(from) + dir * (std::max(depth - from_goal, 0.0f) /
        toward);
    point.setY(m_kart->getXYZ().y());
    return point;
}   // getPointOnShotLine

//-----------------------------------------------------------------------------
/** A bowling ball of an opponent hitting the ball changes everything: if it's
 *  going to send the ball to own goal, get in its way right now, before the
 *  ball even moves.
 *  \return True if the keeper is doing this.
 */
bool SoccerAI::blockBowlingShot()
{
    const float hit_distance = m_world->getBallDiameter() / 2 + 0.7f;
    const std::vector<std::shared_ptr<Flyable> > bowlings =
        ProjectileManager::get()->getProjectiles(
            PowerupManager::POWERUP_BOWLING);
    for (const std::shared_ptr<Flyable>& bowling : bowlings)
    {
        const AbstractKart* owner = bowling->getOwner();
        if (!owner ||
            m_world->getKartTeam(owner->getWorldKartId()) == m_cur_team)
            continue;
        const Vec3 position = flat(bowling->getXYZ());
        const Vec3 velocity = flat(bowling->getVelocity());
        if (velocity.length() < 5.0f)
            continue;
        for (int i = 0; i <= 30; i++)
        {
            const float time = i * 0.05f;
            const Vec3 at = position + velocity * time;
            const Vec3 ball = flat(predictBall(time));
            if ((at - ball).length() > hit_distance)
                continue;
            // The ball goes away from the bowling ball where they touch
            const Vec3 dir = (ball - at).length() > 0.1f ?
                flatNormal(ball - at) : flatNormal(velocity);
            if (!isShotAtOwnGoal(ball, dir))
                break;
            const Vec3 own_goal = flat(m_world->getGoalCenter(m_cur_team));
            const Vec3 kart = flat(m_kart->getXYZ());
            const float from_goal =
                std::min(std::max((kart - own_goal).length(), 3.0f), 14.0f);
            const Vec3 point = getPointOnShotLine(ball, dir, from_goal);
            const int node = findNode(point);
            if (node == Graph::UNKNOWN_SECTOR)
                break;
            m_playing_ball = true;
            m_saving = true;
            setTarget(point, node);
            m_direct = true;
            if ((flat(point) - kart).length() < 2.0f)
            {
                m_hold_position = true;
                m_force_brake = m_kart->getSpeed() > 3.0f;
                m_target_point = m_world->getBallPosition();
            }
            m_debug_mode = "keeper_bowling_block";
            return true;
        }
    }
    return false;
}   // blockBowlingShot

//-----------------------------------------------------------------------------
/** An opponent who has a bowling ball and is behind the ball as seen from own
 *  goal can shoot it at the goal very fast at any time, so wait on that line
 *  instead of the one to the goal center.
 *  \return True if the keeper is doing this.
 */
bool SoccerAI::coverBowlingShot()
{
    const Vec3 ball = flat(m_world->getBallPosition());
    for (unsigned int i = 0; i < m_world->getNumKarts(); i++)
    {
        const AbstractKart* opponent = m_world->getKart(i);
        if (m_world->getKartTeam(i) == m_cur_team ||
            opponent->getKartAnimation() ||
            opponent->getPowerup()->getType() !=
            PowerupManager::POWERUP_BOWLING)
            continue;
        const float distance = (ball - flat(opponent->getXYZ())).length();
        if (distance > 45.0f || distance < 2.0f)
            continue;
        const Vec3 dir = flatNormal(ball - opponent->getXYZ());
        if (!isShotAtOwnGoal(ball, dir))
            continue;
        const Vec3 own_goal = flat(m_world->getGoalCenter(m_cur_team));
        const float from_goal =
            std::min(std::max((ball - own_goal).length() * 0.35f, 5.0f),
                     14.0f);
        driveToPosition(getPointOnShotLine(ball, dir, from_goal),
            m_world->getBallPosition(), m_world->getBallNode());
        m_debug_mode =
            m_hold_position ? "keeper_bowling_wait" : "keeper_bowling_cover";
        return true;
    }
    return false;
}   // coverBowlingShot

//-----------------------------------------------------------------------------
/** Stay on the line between the ball and the center of own goal.
 */
void SoccerAI::doKeeper()
{
    int ball_node = Graph::UNKNOWN_SECTOR;
    const Vec3 ball = findBallIntercept(&ball_node);

    const Vec3 own_goal = m_world->getGoalCenter(m_cur_team);
    const float ball_distance = (flat(ball) - flat(own_goal)).length();
    const float distance =
        std::min(std::max(ball_distance * 0.35f, 5.0f), 14.0f);
    Vec3 position = own_goal + flatNormal(ball - own_goal) * distance;
    position.setY(m_kart->getXYZ().y());

    driveToPosition(position, ball, ball_node);
    m_debug_mode = m_hold_position ? "keeper_wait" : "keeper_pos";
}   // doKeeper

//-----------------------------------------------------------------------------
/** Drive to a position to wait there, looking at the ball. If it's outside the
 *  arena the kart just stays where it is.
 *  \param position Where to go.
 *  \param ball Position of the ball.
 *  \param ball_node ArenaNode of the ball.
 */
void SoccerAI::driveToPosition(const Vec3& position, const Vec3& ball,
                               int ball_node)
{
    const Vec3 kart = flat(m_kart->getXYZ());
    // Close enough is fine for a kart looking at the ball already: a stopped
    // kart can't turn, so correcting that would mean looking away from it
    const Vec3 forward =
        flatNormal(m_kart->getTrans().getBasis().getColumn(2));
    const float near = forward.dot(flatNormal(ball - kart)) > 0.5f ? 8.0f : 3.0f;

    const int node = findNode(position);
    const float distance = (flat(position) - kart).length();
    if (node != Graph::UNKNOWN_SECTOR && distance >= near)
    {
        setTargetAvoidingBall(position, node);
        // Arrive slowly enough to stop there, a keeper running back to its
        // place would otherwise end up in its own goal
        if (m_kart->getSpeed() > 4.0f + 1.5f * distance)
            m_force_brake = true;
        return;
    }
    m_hold_position = true;
    m_force_brake = m_kart->getSpeed() > 3.0f;
    m_target_point = ball;
    m_target_node = ball_node;
    m_direct = true;
}   // driveToPosition

//-----------------------------------------------------------------------------
/** At a kickoff only the striker goes for the ball, as more karts would just
 *  get in each other's way: the others wait for the ball to come out.
 */
bool SoccerAI::isKickoff() const
{
    if (m_world->isBallUntouched())
        return m_kickoff_time < KICKOFF_MAX_WAIT;
    return m_time_since_first_touch >= 0.0f &&
        m_time_since_first_touch < KICKOFF_TOUCH_WAIT;
}   // isKickoff

//-----------------------------------------------------------------------------
/** Wait at a position behind the ball on the side of the field where this kart
 *  already is.
 *  \param back Distance behind the ball.
 *  \param wide Distance from the line through the ball and the goals.
 */
void SoccerAI::doKickoffPosition(float back, float wide)
{
    int ball_node = Graph::UNKNOWN_SECTOR;
    const Vec3 ball = findBallIntercept(&ball_node);
    const Vec3 to_goal =
        flatNormal(m_world->getGoalCenter(m_opp_team) - ball);

    // The ball came to this kart, that's what it was waiting for
    if ((flat(ball) - flat(m_kart->getXYZ())).length() < 12.0f)
    {
        playBall();
        return;
    }

    const Vec3 side(to_goal.z(), 0.0f, -to_goal.x());
    float offset = (flat(m_kart->getXYZ()) - flat(ball)).dot(side);
    // In the middle choose a side so that not all karts take the same one
    if (fabsf(offset) < 2.0f)
        offset = m_kart->getWorldKartId() % 2 == 0 ? 1.0f : -1.0f;
    Vec3 position = ball - to_goal * back +
        side * (offset < 0.0f ? -wide : wide);
    position.setY(m_kart->getXYZ().y());
    driveToPosition(position, ball, ball_node);
}   // doKickoffPosition

//-----------------------------------------------------------------------------
/** Collect what's needed, and otherwise stay behind the ball to be ready when
 *  it comes back. Each support kart keeps to its own side of the ball.
 */
void SoccerAI::doSupport()
{
    const bool needs_powerup =
        m_kart->getPowerup()->getType() == PowerupManager::POWERUP_NOTHING &&
        m_kart->getAttachment()->getType() != Attachment::ATTACH_SWATTER;
    const bool needs_nitro = m_use_nitro_skid && m_kart->getEnergy() < 2.0f;
    if (needs_powerup || needs_nitro)
    {
        tryCollectItem(&m_target_point, &m_target_node);
        setTargetAvoidingBall(m_target_point, m_target_node);
        return;
    }

    int ball_node = Graph::UNKNOWN_SECTOR;
    const Vec3 ball = findBallIntercept(&ball_node);
    const Vec3 to_goal =
        flatNormal(m_world->getGoalCenter(m_opp_team) - ball);
    const Vec3 side(to_goal.z(), 0.0f, -to_goal.x());
    const float side_sign = m_kart->getWorldKartId() % 2 == 0 ? 1.0f : -1.0f;
    // Close enough for a good hit: take it, the striker can't be everywhere
    const Vec3 to_ball = flat(ball) - flat(m_kart->getXYZ());
    if (to_ball.length() < 12.0f && flatNormal(to_ball).dot(to_goal) > 0.6f)
    {
        playBall();
        return;
    }

    for (float back : { 14.0f, 7.0f })
    {
        Vec3 position = ball - to_goal * back + side * side_sign * back * 0.4f;
        position.setY(m_kart->getXYZ().y());
        const int node = findNode(position);
        if (node != Graph::UNKNOWN_SECTOR)
        {
            setTargetAvoidingBall(position, node);
            return;
        }
    }
    setTarget(ball, ball_node);
}   // doSupport

//-----------------------------------------------------------------------------
/** Go after the opposing kart that's closest to the ball.
 */
void SoccerAI::doAttacker()
{
    if (m_kart->getPowerup()->getType() == PowerupManager::POWERUP_NOTHING &&
        m_kart->getAttachment()->getType() != Attachment::ATTACH_SWATTER)
    {
        tryCollectItem(&m_target_point, &m_target_node);
        setTargetAvoidingBall(m_target_point, m_target_node);
        return;
    }
    // In a good place to shoot: same as support
    const Vec3 ball_pos = m_world->getBallPosition();
    const Vec3 to_ball = flat(ball_pos) - flat(m_kart->getXYZ());
    if (to_ball.length() < 12.0f && flatNormal(to_ball).dot(
        flatNormal(m_world->getGoalCenter(m_opp_team) - ball_pos)) > 0.6f)
    {
        playBall();
        return;
    }
    const AbstractKart* kart =
        m_world->getKart(m_world->getBallChaser(m_opp_team));
    setTargetAvoidingBall(kart->getXYZ(), m_world->getSectorForKart(kart));
}   // doAttacker

//-----------------------------------------------------------------------------
/** Check if a bowling ball fired now would send the soccer ball somewhere
 *  useful. The soccer ball goes away from the point where the two balls touch.
 *  \param backwards True to fire backwards.
 */
bool SoccerAI::isGoodBallShot(bool backwards) const
{
    Vec3 dir = flatNormal(m_kart->getTrans().getBasis().getColumn(2));
    if (backwards)
        dir = -dir;
    const float kart_speed = m_kart->getSpeed();
    const float speed = backwards ?
        (kart_speed < 0.0f ? (4.0f - kart_speed) * 2.0f : BOWLING_BACK_SPEED) :
        std::max(kart_speed * 2.0f + BOWLING_ADD_SPEED, BOWLING_MIN_SPEED);

    const Vec3 start = flat(m_kart->getXYZ()) +
        dir * (m_kart_length * 0.5f + BOWLING_RADIUS);

    float time = (flat(m_world->getBallPosition()) - start).length() / speed;
    const Vec3 ball = predictBall(time);
    const float ball_radius = m_world->getBallDiameter() / 2;
    // A bowling ball stays on the ground
    if (ball.y() - m_kart->getXYZ().y() > ball_radius + 1.5f)
        return false;

    const Vec3 to_ball = flat(ball) - start;
    const float along = to_ball.dot(dir);
    // Too far away it's not accurate enough
    if (along <= 0.0f || along > BOWLING_MAX_RANGE)
        return false;
    const float offset2 = to_ball.length2() - along * along;
    const float touch = ball_radius + BOWLING_RADIUS;
    if (offset2 > touch * touch * 0.8f)
        return false;

    // Karts in the way: an opponent would be a good hit too, but a teammate
    // would take it all
    for (unsigned int i = 0; i < m_world->getNumKarts(); i++)
    {
        const AbstractKart* kart = m_world->getKart(i);
        if (kart == m_kart || m_world->getKartTeam(i) != m_cur_team)
            continue;
        const Vec3 to_kart = flat(kart->getXYZ()) - start;
        const float kart_along = to_kart.dot(dir);
        if (kart_along > 0.0f && kart_along < along &&
            to_kart.length2() - kart_along * kart_along < 2.25f)
            return false;
    }

    const Vec3 contact = start + dir * (along - sqrtf(touch * touch - offset2));
    const Vec3 ball_dir = flatNormal(flat(ball) - contact);

    const Vec3 to_opp_goal = m_world->getGoalCenter(m_opp_team) - ball;
    const float goal_distance = to_opp_goal.length_2d();
    const float goal_cos = ball_dir.dot(flatNormal(to_opp_goal));
    const float goal_angle = atan2f(
        m_world->getGoalHalfWidth(m_opp_team) * 0.9f, goal_distance);
    // On target
    if (goal_cos >= cosf(goal_angle))
        return true;
    // Or clearly up the field from nearby
    if (goal_cos > 0.85f && along < 25.0f)
        return true;
    // Or where the ball is wanted to go
    if (m_playing_ball && ball_dir.dot(m_shot_dir) > 0.9f)
        return true;

    // Or away when the ball is close to own goal
    const Vec3 to_own_goal = m_world->getGoalCenter(m_cur_team) - ball;
    const float field = (flat(m_world->getGoalCenter(m_opp_team)) -
        flat(m_world->getGoalCenter(m_cur_team))).length();
    return to_own_goal.length_2d() < field * 0.45f &&
        ball_dir.dot(flatNormal(to_own_goal)) < -0.3f;
}   // isGoodBallShot

//-----------------------------------------------------------------------------
/** Check if a bowling ball should be fired at an opposing kart straight ahead:
 *  if it's close to the ball and closer than this kart.
 */
bool SoccerAI::isGoodKartShot() const
{
    if (m_time_since_kart_shot < KART_SHOT_INTERVAL || !m_closest_kart ||
        m_closest_kart->isInvulnerable() ||
        m_closest_kart->getKartAnimation())
        return false;
    const Vec3 lc = m_kart->getTrans().inverse()(m_closest_kart_point);
    if (lc.z() < 0.0f || lc.z() > 15.0f ||
        atan2f(fabsf(lc.x()), lc.z()) > 0.15f)
        return false;

    const Vec3 ball = flat(m_world->getBallPosition());
    const float its_distance = (ball - flat(m_closest_kart_point)).length();
    const float my_distance = (ball - flat(m_kart->getXYZ())).length();
    return its_distance < 15.0f && its_distance < my_distance;
}   // isGoodKartShot

//-----------------------------------------------------------------------------
/** Fire bowling balls at the soccer ball when possible, the base class decides
 *  about everything else.
 *  \param dt Time step size.
 */
void SoccerAI::useItems(const float dt)
{
    if (!m_use_ball_shots ||
        m_kart->getPowerup()->getType() != PowerupManager::POWERUP_BOWLING)
    {
        ArenaAI::useItems(dt);
        return;
    }

    m_controls->setFire(false);
    if (m_kart->getKartAnimation() || m_kart->getShieldTime() > 2.0f ||
        m_time_since_shot < BALL_SHOT_INTERVAL)
        return;

    bool fire = isGoodBallShot(false/*backwards*/);
    if (fire || isGoodBallShot(true/*backwards*/))
    {
        m_controls->setFire(true);
        m_controls->setLookBack(!fire);
        m_time_since_shot = 0.0f;
        return;
    }

    findClosestKart(false/*consider_difficulty*/, false/*find_sta*/);
    if (isGoodKartShot())
    {
        m_controls->setFire(true);
        m_time_since_shot = 0.0f;
        m_time_since_kart_shot = 0.0f;
    }
}   // useItems

//-----------------------------------------------------------------------------
/** Use nitro to shoot, and to get to where it's needed when it's far away.
 */
bool SoccerAI::shouldUseNitro()
{
    if (!m_use_nitro_skid)
        return ArenaAI::shouldUseNitro();

    if (m_hold_position || m_kart->getEnergy() <= 0.0f ||
        (m_kart->getSpeed() < 8.0f && !m_saving))
        return false;

    const Vec3 lc = m_kart->getTrans().inverse()(m_target_point);
    if (lc.z() < 0.0f || atan2f(fabsf(lc.x()), lc.z()) > 0.4f)
        return false;
    const float distance = lc.length_2d();
    return m_saving || (m_striking && distance < 60.0f) ||
        (distance > 15.0f && m_kart->getEnergy() > 3.0f);
}   // shouldUseNitro

//-----------------------------------------------------------------------------
/** Skid to turn faster, but don't touch the ball while sliding.
 */
bool SoccerAI::canSkid(float steer_fraction)
{
    if (!m_full_ai)
        return m_mini_skid && !(m_overtake_ball || m_chasing_ball);
    if (!m_use_nitro_skid || !m_direct || m_hold_position ||
        fabsf(steer_fraction) <= 1.0f || m_kart->getSpeed() < 10.0f)
        return false;
    return !m_playing_ball || (flat(m_world->getBallPosition()) -
        flat(m_kart->getXYZ())).length() > 6.0f;
}   // canSkid

//-----------------------------------------------------------------------------
/** Only reverse if the target is so close that driving around it takes longer,
 *  to turn around for looking at the ball when waiting, close to own goal
 *  when facing it, or with a wall in front.
 *  \param lc Target in the local coordinates of the kart.
 */
bool SoccerAI::shouldReverseToTurn(const Vec3& lc) const
{
    // With a wall right in front there's no room to turn around forward
    if (m_full_ai)
    {
        const Vec3 forward =
            flatNormal(m_kart->getTrans().getBasis().getColumn(2));
        if (findNode(m_kart->getXYZ() + forward * 6.0f) ==
            Graph::UNKNOWN_SECTOR)
            return true;
    }
    // Turning around forward when facing own goal close to it would drive the
    // kart into the goal, where it gets stuck
    if (m_full_ai)
    {
        const Vec3 own_goal = flat(m_world->getGoalCenter(m_cur_team));
        const Vec3 kart = flat(m_kart->getXYZ());
        const Vec3 forward =
            flatNormal(m_kart->getTrans().getBasis().getColumn(2));
        if ((own_goal - kart).length() < 20.0f &&
            forward.dot(flatNormal(own_goal - kart)) > 0.5f)
            return true;
    }
    return !m_full_ai || m_hold_position ||
        (lc.length_2d() < (m_saving ? 15.0f : 7.0f) &&
         m_kart->getSpeed() < 8.0f);
}   // shouldReverseToTurn

//-----------------------------------------------------------------------------
/** Jump at a ball in the air using the same jump as players, if the kart
 *  meeting it in the air doesn't send it back toward own side, or to save it.
 */
void SoccerAI::tryBallJump()
{
    if (!m_playing_ball || m_jump_cooldown > 0.0f || !m_kart->isOnGround())
        return;

    const Vec3 ball = m_world->getBallPosition();
    const float above = ball.y() - m_kart->getXYZ().y();
    const Vec3 to_ball = flat(ball) - flat(m_kart->getXYZ());
    if (above < JUMP_MIN_HEIGHT || to_ball.length() > JUMP_RANGE)
        return;

    // Exactly the jump of the players
    Vec3 velocity;
    if (!PlayerController::computeBallJumpVelocity(m_kart, m_world, &velocity))
        return;

    // The ball goes the way the kart hits it, relative to the ball
    const Vec3 to_goal =
        flatNormal(m_world->getGoalCenter(m_opp_team) - ball);
    const Vec3 impact = flatNormal(velocity - m_world->getBallVelocity());
    if (impact.dot(to_goal) < 0.0f &&
        !m_world->isBallMovingTowardGoal(m_cur_team))
        return;

    m_kart->getBody()->setLinearVelocity(velocity);
    m_jump_cooldown = 2.5f;
    m_world->logEvent(m_kart->getWorldKartId(), "jump");
}   // tryBallJump

//-----------------------------------------------------------------------------
/** Find the closest kart around this AI, it won't find the kart with same
 *  team, consider_difficulty and find_sta are not used here.
 *  \param consider_difficulty If take current difficulty into account.
 *  \param find_sta If find \ref SpareTireAI only.
 */
void SoccerAI::findClosestKart(bool consider_difficulty, bool find_sta)
{
    float distance = 99999.9f;
    const unsigned int n = m_world->getNumKarts();
    int closest_kart_num = 0;

    for (unsigned int i = 0; i < n; i++)
    {
        const AbstractKart* kart = m_world->getKart(i);
        if (kart->isEliminated()) continue;

        if (kart->getWorldKartId() == m_kart->getWorldKartId())
            continue; // Skip the same kart

        if (m_world->getKartTeam(kart
            ->getWorldKartId()) == m_world->getKartTeam(m_kart
            ->getWorldKartId()))
            continue; // Skip the kart with the same team

        Vec3 d = kart->getXYZ() - m_kart->getXYZ();
        if (d.length_2d() <= distance)
        {
            distance = d.length_2d();
            closest_kart_num = i;
        }
    }

    m_closest_kart = m_world->getKart(closest_kart_num);
    m_closest_kart_node = m_world->getSectorForKart(m_closest_kart);
    m_closest_kart_point = m_closest_kart->getXYZ();

}   // findClosestKart

//-----------------------------------------------------------------------------
/** Find a suitable target to follow, it will first call
 *  \ref SoccerWorld::getBallChaser to check if this AI should go chasing the
 *  ball and try to score, otherwise it will call \ref tryCollectItem if
 *  needed. After that it will call \ref SoccerWorld::getAttacker to see if
 *  this AI should attack the kart in opposite team which is chasing the ball,
 *  if not go for the closest kart found by \ref findClosestKart.
 */
void SoccerAI::findTarget()
{
    findClosestKart(true/*consider_difficulty*/, false/*find_sta*/);

    if (m_full_ai)
    {
        m_striking = false;
        m_playing_ball = false;
        m_hold_position = false;
        m_saving = false;
        m_direct = false;
        m_force_brake = false;
        switch (m_world->getAIRole(m_kart->getWorldKartId()))
        {
        case SoccerWorld::AI_ROLE_KEEPER:
            if (blockBowlingShot())
                break;
            if (shouldKeeperPlayBall())
                playBall();
            else if (!coverBowlingShot())
                doKeeper();
            break;
        case SoccerWorld::AI_ROLE_SUPPORT:
            m_debug_mode = isKickoff() ? "kickoff_wait" : "support";
            if (isKickoff())
                doKickoffPosition(18.0f/*back*/, 22.0f/*wide*/);
            else
                doSupport();
            break;
        case SoccerWorld::AI_ROLE_ATTACKER:
            m_debug_mode = isKickoff() ? "kickoff_wait" : "attacker";
            if (isKickoff())
                doKickoffPosition(30.0f/*back*/, 8.0f/*wide*/);
            else
                doAttacker();
            break;
        default:
            playBall();
            break;
        }
        return;
    }

    m_debug_mode = "basic";
    // Check if this AI kart is the one who will chase the ball
    if (m_world->getBallChaser(m_cur_team) == (signed)m_kart->getWorldKartId())
    {
        m_target_point = determineBallAimingPosition();
        m_target_node  = m_world->getBallNode();
        return;
    }

    // Always reset this flag,
    // in case the ball chaser lost the ball somehow
    m_overtake_ball = false;

    if (m_kart->getPowerup()->getType() == PowerupManager::POWERUP_NOTHING &&
        m_kart->getAttachment()->getType() != Attachment::ATTACH_SWATTER)
    {
        tryCollectItem(&m_target_point , &m_target_node);
    }
    else if (m_world->getAttacker(m_cur_team) == (signed)m_kart
        ->getWorldKartId())
    {
        // This AI will attack the other team ball chaser
        int id = m_world->getBallChaser(m_opp_team);
        const AbstractKart* kart = m_world->getKart(id);
        m_target_point = kart->getXYZ();
        m_target_node  = m_world->getSectorForKart(kart);
    }
    else
    {
        m_target_point = m_closest_kart_point;
        m_target_node  = m_closest_kart_node;
    }

}   // findTarget

//-----------------------------------------------------------------------------
/** Determine the point for aiming when try to steer or overtake the ball.
 *  AI will overtake the ball if the aiming position calculated by world is
 *  non-reachable.
 *  \return The coordinates to aim at.
 */
Vec3 SoccerAI::determineBallAimingPosition()
{
#ifdef BALL_AIM_DEBUG
    // Choose your favourite team to watch
    if (m_world->getKartTeam(m_kart->getWorldKartId()) == KART_TEAM_BLUE)
    {
        Camera *cam = Camera::getActiveCamera();
        cam->setMode(Camera::CM_NORMAL);
        cam->setKart(m_kart);
    }
#endif

    const Vec3& ball_aim_pos = m_world->getBallAimPosition(m_opp_team);
    const Vec3& orig_pos = m_world->getBallPosition();

    Vec3 ball_lc = m_front_transform.inverse()(orig_pos);
    Vec3 aim_lc = m_front_transform.inverse()(ball_aim_pos);

    // Too far from the ball,
    // use path finding from arena ai to get close
    // ie no extra braking is needed
    if (aim_lc.length_2d() > 10.0f) return ball_aim_pos;

    if (m_overtake_ball)
    {
        Vec3 overtake_lc;
        const bool can_overtake = determineOvertakePosition(ball_lc, aim_lc,
            &overtake_lc);
        if (!can_overtake)
        {
            m_overtake_ball = false;
            return ball_aim_pos;
        }
        else
            return m_front_transform(overtake_lc);
    }
    else
    {
        // Check whether the aim point is non-reachable
        // ie the ball is in front of the kart, which the aim position
        // is behind the ball , if so m_overtake_ball is true
        if (aim_lc.z() > 0 && aim_lc.z() > ball_lc.z())
        {
            if (isOvertakable(ball_lc))
            {
                m_overtake_ball = true;
                return ball_aim_pos;
            }
            else
            {
                // Stop a while to wait for overtaking, prevent own goal too
                // Only do that if the ball is moving
                if (!m_world->ballNotMoving())
                    m_force_brake = true;
                return ball_aim_pos;
            }
        }

        m_chasing_ball = true;
        // Check if reached aim point, which is behind aiming position and
        // in front of the ball, if so use another aiming method
        if (aim_lc.z() < 0 && ball_lc.z() > 0)
        {
            // Return the behind version of aim position, allow pushing to
            // ball towards the it
            return m_world->getBallAimPosition(m_opp_team, true/*reverse*/);
        }
    }

    // Otherwise keep steering until reach aim position
    return ball_aim_pos;

}   // determineBallAimingPosition

//-----------------------------------------------------------------------------
/** Used in \ref determineBallAimingPosition to test if AI can overtake the
 *  ball by testing distance.
 *  \param ball_lc Local coordinates of the ball.
 *  \return False if the kart is too close to the ball which can't overtake
 */
bool SoccerAI::isOvertakable(const Vec3& ball_lc)
{
    // No overtake if ball is behind
    if (ball_lc.z() < 0.0f) return false;

    // Circle equation: (x-a)2 + (y-b)2 = r2
    const float r2 = (ball_lc.length_2d() / 2) * (ball_lc.length_2d() / 2);
    const float a = ball_lc.x();
    const float b = ball_lc.z();

    // Check first if the kart is lies inside the circle, if so no tangent
    // can be drawn ( so can't overtake), minus 0.1 as epslion
    const float test_radius_2 = ((a * a) + (b * b)) - 0.1f;
    if (test_radius_2 < r2)
    {
        return false;
    }
    return true;

}   // isOvertakable

//-----------------------------------------------------------------------------
/** Used in \ref determineBallAimingPosition to pick a correct point to
 *  overtake the ball
 *  \param ball_lc Local coordinates of the ball.
 *  \param aim_lc Local coordinates of the aiming position.
 *  \param[out] overtake_lc Local coordinates of the overtaking position.
 *  \return True if overtaking is possible.
 */
bool SoccerAI::determineOvertakePosition(const Vec3& ball_lc,
                                         const Vec3& aim_lc,
                                         Vec3* overtake_lc)
{
    // This done by drawing a circle using the center of ball local coordinates
    // and the distance / 2 from kart to ball center as radius (which allows
    // more offset for overtaking), then find tangent line from kart (0, 0, 0)
    // to the circle. The intercept point will be used as overtake position

    // Check if overtakable at current location
    if (!isOvertakable(ball_lc)) return false;

    // Otherwise calculate the tangent
    // As all are local coordinates, so center is 0,0 which is y = mx for the
    // tangent equation, and the m (slope) can be calculated by puting y = mx
    // into the general form of circle equation x2 + y2 + Dx + Ey + F = 0
    // This means:  x2 + m2x2 + Dx + Emx + F = 0
    //                 (1+m2)x2 + (D+Em)x +F = 0
    // As only one root for it, so discriminant b2 - 4ac = 0
    // So:              (D+Em)2 - 4(1+m2)(F) = 0
    //           D2 + 2DEm +E2m2 - 4F - 4m2F = 0
    //      (E2 - 4F)m2 + (2DE)m + (D2 - 4F) = 0
    // Now solve the above quadratic equation using
    // x = -b (+/-) sqrt(b2 - 4ac) / 2a

    // Circle equation: (x-a)2 + (y-b)2 = r2
    const float r = ball_lc.length_2d() / 2;
    const float r2 = r * r;
    const float a = ball_lc.x();
    const float b = ball_lc.z();

    const float d = -2 * a;
    const float e = -2 * b;
    const float f = (d * d / 4) + (e * e / 4) - r2;
    const float discriminant = (2 * 2 * d * d * e * e) -
        (4 * ((e * e) - (4 * f)) * ((d * d) - (4 * f)));

    assert(discriminant > 0.0f);
    float t_slope_1 = (-(2 * d * e) + sqrtf(discriminant)) /
        (2 * ((e * e) - (4 * f)));
    float t_slope_2 = (-(2 * d * e) - sqrtf(discriminant)) /
        (2 * ((e * e) - (4 * f)));

    assert(!std::isnan(t_slope_1));
    assert(!std::isnan(t_slope_2));

    // Make the slopes in correct order, allow easier rotate later
    float slope_1 = 0.0f;
    float slope_2 = 0.0f;
    if ((t_slope_1 > 0 && t_slope_2 > 0) || (t_slope_1 < 0 && t_slope_2 < 0))
    {
        if (t_slope_1 > t_slope_2)
        {
            slope_1 = t_slope_1;
            slope_2 = t_slope_2;
        }
        else
        {
            slope_1 = t_slope_2;
            slope_2 = t_slope_1;
        }
    }
    else
    {
        if (t_slope_1 > t_slope_2)
        {
            slope_1 = t_slope_2;
            slope_2 = t_slope_1;
        }
        else
        {
            slope_1 = t_slope_1;
            slope_2 = t_slope_2;
        }
    }

    // Calculate two intercept points, as we already put y=mx into circle
    // equation and know that only one root for each slope, so x can be
    // calculated easily with -b / 2a
    // From (1+m2)x2 + (D+Em)x +F = 0:
    const float x1 = -(d + (e * slope_1)) / (2 * (1 + (slope_1 * slope_1)));
    const float x2 = -(d + (e * slope_2)) / (2 * (1 + (slope_2 * slope_2)));
    const float y1 = slope_1 * x1;
    const float y2 = slope_2 * x2;

    const Vec3 point1(x1, 0, y1);
    const Vec3 point2(x2, 0, y2);

    const float d1 = (point1 - aim_lc).length_2d();
    const float d2 = (point2 - aim_lc).length_2d();

    // Use the tangent closest to the ball aiming position to aim
    const bool use_tangent_one = d1 < d2;

    // Adjust x and y if r < ball diameter,
    // which will likely push to ball forward
    // Notice: we cannot increase the radius before, as the kart location
    // will likely lie inside the enlarged circle
    if (r < m_world->getBallDiameter())
    {
        // Constuctor a equation using y = (rotateSlope(old_m)) x which is
        // a less steep or steeper line, and find out the new adjusted position
        // using the distance to the original point * 2 at new line

        // Determine if the circle is drawn around the side of kart
        // ie point1 or point2 z() < 0, if so reverse the below logic
        const float m = ((point1.z() < 0 || point2.z() < 0) ?
            (use_tangent_one ? rotateSlope(slope_1, false/*rotate_up*/) :
            rotateSlope(slope_2, true/*rotate_up*/)) :
            (use_tangent_one ? rotateSlope(slope_1, true/*rotate_up*/) :
            rotateSlope(slope_2, false/*rotate_up*/)));

        // Calculate new distance from kart to new adjusted position
        const float dist = (use_tangent_one ? point1 : point2).length_2d() * 2;
        const float dist2 = dist * dist;

        // x2 + y2 = dist2
        // so y = m * sqrtf (dist2 - y2)
        // y = sqrtf(m2 * dist2 / (1 + m2))
        const float y = sqrtf((m * m * dist2) / (1 + (m * m)));
        const float x = y / m;
        *overtake_lc = Vec3(x, 0, y);
    }
    else
    {
        // Use the calculated position depends on distance to aim position
        if (use_tangent_one)
            *overtake_lc = point1;
        else
            *overtake_lc = point2;
    }

    return true;
}   // determineOvertakePosition

//-----------------------------------------------------------------------------
/** Used in \ref determineOvertakePosition to adjust the overtake position
 *  which is calculated by slope of line if it's too close.
 *  \param old_slope Old slope calculated.
 *  \param rotate_up If adjust the slope upwards.
 *  \return A newly calculated slope.
 */
float SoccerAI::rotateSlope(float old_slope, bool rotate_up)
{
    const float theta = atanf(old_slope) + (old_slope < 0 ? M_PI : 0);
    float new_theta = theta + (rotate_up ? M_PI / 6 : -M_PI /6);
    if (new_theta > ((M_PI / 2) - 0.02f) && new_theta < ((M_PI / 2) + 0.02f))
    {
        // Avoid almost tan 90
        new_theta = (M_PI / 2) - 0.02f;
    }
    // Check if over-rotated
    if (new_theta > M_PI)
        new_theta = M_PI - 0.1f;
    else if (new_theta < 0)
        new_theta = 0.1f;

    return tanf(new_theta);
}   // rotateSlope

//-----------------------------------------------------------------------------
int SoccerAI::getCurrentNode() const
{
    return m_world->getSectorForKart(m_kart);
}   // getCurrentNode

//-----------------------------------------------------------------------------
bool SoccerAI::isWaiting() const
{
    return m_world->isStartPhase();
}   // isWaiting

//-----------------------------------------------------------------------------
float SoccerAI::getKartDistance(const AbstractKart* kart) const
{
    return m_graph->getDistance(getCurrentNode(),
        m_world->getSectorForKart(kart));
}   // getKartDistance

//-----------------------------------------------------------------------------
bool SoccerAI::isKartOnRoad() const
{
    return m_world->isOnRoad(m_kart->getWorldKartId());
}   // isKartOnRoad
