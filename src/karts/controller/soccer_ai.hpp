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

#ifndef HEADER_SOCCER_AI_HPP
#define HEADER_SOCCER_AI_HPP

#include "karts/controller/arena_ai.hpp"

#include "LinearMath/btTransform.h"

#undef BALL_AIM_DEBUG
#ifdef BALL_AIM_DEBUG
#include "graphics/irr_driver.hpp"
#endif

class SoccerWorld;

/** The actual soccer AI. It has two levels of play: the basic AI only chases
 *  the ball with the closest kart, it's used in beginner difficulty. The full
 *  AI plays with team roles (see \ref SoccerWorld::AIRole), predicts the ball,
 *  sets up shots and kicks the ball with bowling balls, what it may use and
 *  how much help it gets depends on the difficulty (see \ref setupSkill).
 * \ingroup controller
 */
class SoccerAI : public ArenaAI
{
private:

#ifdef BALL_AIM_DEBUG
    irr::scene::ISceneNode *m_red_sphere;
    irr::scene::ISceneNode *m_blue_sphere;
#endif

    /** Keep a pointer to world. */
    SoccerWorld *m_world;

    /** Save the team this AI belongs to. */
    KartTeam m_cur_team;

    /** Save the opposite team of this AI team. */
    KartTeam m_opp_team;

    /** Define which way to handle to ball, either steer with it,
     *  or overtake it (Defense). */
    bool m_overtake_ball;

    /** True if \ref forceBraking() is needed to be called. */
    bool m_force_brake;

    /** True if AI should steer with the ball. */
    bool m_chasing_ball;

    /** The front point of kart with the same rotation of center mass, used
     *  to determine point for aiming with ball */
    btTransform m_front_transform;

    /** True if the full AI is used instead of the basic one. */
    bool m_full_ai;

    /** If true, the AI uses nitro and skidding to get to the ball and shoot. */
    bool m_use_nitro_skid;

    /** If true, the AI fires bowling balls at the soccer ball. */
    bool m_use_ball_shots;

    /** If true, the AI jumps at the ball like players can. */
    bool m_use_jump;

    /** Nitro this AI gets per second. */
    float m_nitro_regen;

    /** True if AI is on its way to hit the ball towards \ref m_shot_dir. */
    bool m_striking;

    /** True if AI is playing the ball for this frame. */
    bool m_playing_ball;

    /** True if the way to the target is free so no path finding is needed. */
    bool m_direct;

    /** True if the kart reached the position it should defend. */
    bool m_hold_position;

    /** True if AI tries to stop a ball going to own goal for this frame. */
    bool m_saving;

    /** The direction (without height) the AI wants the ball to go. */
    Vec3 m_shot_dir;

    /** Time since this AI fired a bowling ball at something. */
    float m_time_since_shot;

    /** Time since this AI fired a bowling ball at a kart. */
    float m_time_since_kart_shot;

    /** How long the ball hasn't been touched after a kickoff. */
    float m_kickoff_time;

    /** How long ago the ball was touched for the first time after a kickoff,
     *  negative if it hasn't been yet. */
    float m_time_since_first_touch;

    /** The side of the ball (1 or -1) this kart is going around it on. */
    float m_around_side;

    /** How long this kart has been around \ref m_still_position, to tell if
     *  it's stuck. */
    float m_still_time;
    Vec3 m_still_position;

    /** Time until this AI can jump again. */
    float m_jump_cooldown;

    /** What the AI is doing in this frame, for the goal log. */
    const char* m_debug_mode;

    /** How long the kart has been accelerating without moving. */
    float m_blocked_time;

    /** If the red and blue team must use the basic AI, to compare it with the
     *  full AI in the profiling mode. */
    static bool m_baseline_red;
    static bool m_baseline_blue;

    // ------------------------------------------------------------------------
    void  setupSkill();
    // ------------------------------------------------------------------------
    void  applyHelp(float dt);
    // ------------------------------------------------------------------------
    Vec3  predictBall(float time) const;
    // ------------------------------------------------------------------------
    Vec3  findBallIntercept(int* node) const;
    // ------------------------------------------------------------------------
    int   findNode(const Vec3& point) const;
    // ------------------------------------------------------------------------
    void  setTarget(const Vec3& point, int node);
    // ------------------------------------------------------------------------
    void  setTargetAvoidingBall(const Vec3& point, int node);
    // ------------------------------------------------------------------------
    void  playBall();
    // ------------------------------------------------------------------------
    void  doSave();
    // ------------------------------------------------------------------------
    void  driveBehindBall(const Vec3& ball, const Vec3& dir, float clearance,
                          int fallback_node);
    // ------------------------------------------------------------------------
    virtual bool  brakeForTurns() const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual float getMinBrakeSpeed() const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual float getMaxUTurnTime() const OVERRIDE;
    // ------------------------------------------------------------------------
    bool  isUnderPressure(const Vec3& ball, float my_distance) const;
    // ------------------------------------------------------------------------
    bool  shouldKeeperPlayBall() const;
    // ------------------------------------------------------------------------
    bool  isShotAtOwnGoal(const Vec3& from, const Vec3& dir) const;
    // ------------------------------------------------------------------------
    Vec3  getPointOnShotLine(const Vec3& from, const Vec3& dir,
                             float from_goal) const;
    // ------------------------------------------------------------------------
    bool  blockBowlingShot();
    // ------------------------------------------------------------------------
    bool  coverBowlingShot();
    // ------------------------------------------------------------------------
    void  driveToPosition(const Vec3& position, const Vec3& ball, int ball_node);
    // ------------------------------------------------------------------------
    void  doKeeper();
    // ------------------------------------------------------------------------
    bool  isKickoff() const;
    // ------------------------------------------------------------------------
    void  doKickoffPosition(float back, float wide);
    // ------------------------------------------------------------------------
    void  doSupport();
    // ------------------------------------------------------------------------
    void  doAttacker();
    // ------------------------------------------------------------------------
    bool  isGoodBallShot(bool backwards) const;
    // ------------------------------------------------------------------------
    bool  isGoodKartShot() const;
    // ------------------------------------------------------------------------
    void  tryBallJump();
    // ------------------------------------------------------------------------
    Vec3  determineBallAimingPosition();
    // ------------------------------------------------------------------------
    bool  determineOvertakePosition(const Vec3& ball_lc, const Vec3& aim_lc,
                                    Vec3* overtake_lc);
    // ------------------------------------------------------------------------
    bool  isOvertakable(const Vec3& ball_lc);
    // ------------------------------------------------------------------------
    float rotateSlope(float old_slope, bool rotate_up);
    // ------------------------------------------------------------------------
    virtual bool  canSkid(float steer_fraction) OVERRIDE;
    // ------------------------------------------------------------------------
    virtual void  findClosestKart(bool consider_difficulty,
                                  bool find_sta) OVERRIDE;
    // ------------------------------------------------------------------------
    virtual void  findTarget() OVERRIDE;
    // ------------------------------------------------------------------------
    virtual void  useItems(const float dt) OVERRIDE;
    // ------------------------------------------------------------------------
    virtual bool  shouldUseNitro() OVERRIDE;
    // ------------------------------------------------------------------------
    virtual bool  holdPosition() const OVERRIDE     { return m_hold_position; }
    // ------------------------------------------------------------------------
    virtual bool  isBlocked() const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual bool  shouldReverseToTurn(const Vec3& lc) const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual bool  forceBraking() OVERRIDE             { return m_force_brake; }
    // ------------------------------------------------------------------------
    virtual int   getCurrentNode() const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual float getKartDistance(const AbstractKart* kart) const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual bool  ignorePathFinding() OVERRIDE
    {
        return m_full_ai ? m_direct : m_overtake_ball || m_chasing_ball;
    }
    // ------------------------------------------------------------------------
    virtual bool  isKartOnRoad() const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual bool  isWaiting() const OVERRIDE;
    // ------------------------------------------------------------------------
    virtual void  resetAfterStop() OVERRIDE        { m_overtake_ball = false; }

public:
                 SoccerAI(AbstractKart *kart);
                ~SoccerAI();
    virtual void update (int ticks) OVERRIDE;
    virtual void reset() OVERRIDE;
    // ------------------------------------------------------------------------
    /** What the AI is doing in this frame, for the goal log. */
    const char* getDebugMode() const                  { return m_debug_mode; }
    // ------------------------------------------------------------------------
    /** Where the AI is driving to. */
    const Vec3& getTargetPoint() const              { return m_target_point; }
    // ------------------------------------------------------------------------
    /** Make teams use the basic AI, for --soccer-ai-baseline. */
    static void setBaselineTeams(bool red, bool blue)
    {
        m_baseline_red = red;
        m_baseline_blue = blue;
    }

};

#endif
