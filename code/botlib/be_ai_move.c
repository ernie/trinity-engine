/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*****************************************************************************
 * name:		be_ai_move.c
 *
 * desc:		bot movement AI
 *
 * $Archive: /MissionPack/code/botlib/be_ai_move.c $
 *
 *****************************************************************************/

#include "../qcommon/q_shared.h"
#include "l_memory.h"
#include "l_libvar.h"
#include "l_utils.h"
#include "l_script.h"
#include "l_precomp.h"
#include "l_struct.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_aas_funcs.h"
#include "be_interface.h"

#include "be_ea.h"
#include "be_ai_goal.h"
#include "be_ai_move.h"


//#define DEBUG_AI_MOVE
//#define DEBUG_ELEVATOR
//#define DEBUG_GRAPPLE

//movement state
//NOTE: the moveflags MFL_ONGROUND, MFL_TELEPORTED, MFL_WATERJUMP and
//		MFL_GRAPPLEPULL must be set outside the movement code
typedef struct bot_movestate_s
{
	//input vars (all set outside the movement code)
	vec3_t origin;								//origin of the bot
	vec3_t velocity;							//velocity of the bot
	vec3_t viewoffset;							//view offset
	int entitynum;								//entity number of the bot
	int client;									//client number of the bot
	float thinktime;							//time the bot thinks
	int presencetype;							//presencetype of the bot
	vec3_t viewangles;							//view angles of the bot
	//state vars
	int areanum;								//area the bot is in
	int lastareanum;							//last area the bot was in
	int lastgoalareanum;						//last goal area number
	int lastreachnum;							//last reachability number
	vec3_t lastorigin;							//origin previous cycle
	int reachareanum;							//area number of the reachabilty
	int moveflags;								//movement flags
	int jumpreach;								//set when jumped
	int grapplefire_reachnum;					//reach a fired hook belongs to, nonzero from the
												//shot until the tow ends
	float grapplevisible_time;					//last time the grapple was visible
	float lastgrappledist;						//last HORIZONTAL distance to the grapple end
												//(the legacy ride-in endings' semantics)
	float lastgrappledist3;						//last 3D distance to the anchor: the published
												//release axis, stall-watched for windowed tows
	float grapplearm_dist;						//armed release distance from the anchor; the
												//per-frame input hook fires the button-up when
												//the tow closes to it. 0 = disarmed
	int grapplearm_hw;							//half-width of the armed window
	int grapplearm_rtol;						//the shot's published anchor tolerance
	float grapplearm_lastdist;					//the tether length the frame hook saw last frame,
												//0 before the first pull frame: its delta is the
												//tow's measured step, the release's sampling unit
	//the shot's anchor, and whether the frame hook has seen its bite
	vec3_t grapplefire_anchor;
	int grapplefire_bitten;
	int grapplefire_center;						//the fired reach's window and tolerance, banked
	int grapplefire_hw;							//at the shot: the frame hook's latch check and
	int grapplefire_rtol;						//its ANCHOR_INVALID line read these, not the reach
	int grapplefire_invalid;					//the frame hook let go of an invalid bite; the
												//next think remembers the reach and ends it
	float grapplespot_time;						//first think inside the fire radius, hook ready; 0 outside it
	float reachability_time;					//time to use current reachability
	int avoidreach[MAX_AVOIDREACH];				//reachabilities to avoid
	float avoidreachtimes[MAX_AVOIDREACH];		//times to avoid the reachabilities
	int avoidreachtries[MAX_AVOIDREACH];		//number of tries before avoiding
	//
	bot_avoidspot_t avoidspots[MAX_AVOIDSPOTS];	//spots to avoid
	int numavoidspots;
} bot_movestate_t;

//used to avoid reachability links for some time after being used
#define AVOIDREACH
#define AVOIDREACH_TIME			6		//avoid links for 6 seconds after use
#define AVOIDREACH_TRIES		4
//prediction times
#define PREDICTIONTIME_JUMP	3		//in seconds
#define PREDICTIONTIME_MOVE	2		//in seconds
//weapon indexes for weapon jumping
#define WEAPONINDEX_ROCKET_LAUNCHER		5
#define WEAPONINDEX_BFG					9

#define MODELTYPE_FUNC_PLAT		1
#define MODELTYPE_FUNC_BOB		2
#define MODELTYPE_FUNC_DOOR		3
#define MODELTYPE_FUNC_STATIC	4

static libvar_t *sv_maxstep;
static libvar_t *sv_maxbarrier;
static libvar_t *sv_gravity;
static libvar_t *weapindex_rocketlauncher;
static libvar_t *weapindex_bfg10k;
static libvar_t *weapindex_grapple;
static libvar_t *offhandgrapple;
static libvar_t *cmd_grappleoff;
static libvar_t *cmd_grappleon;
//the game's PMF_GRAPPLE_PULL bit, taken as a libvar like weapindex_grapple:
//the flag lives in the game's bg_public.h and the engine cannot include it.
//MUST MATCH: the default carries a must-match note on the game side too.
static libvar_t *pmf_grapplepull;
//the published anchor-tolerance table, selected by the R index in traveltype
//bits 26-28 (aasfile.h). MUST MATCH the .aat compiler's aas_grapple_r_step.
static const float aas_grapple_r_step[AAS_GRAPPLE_R_STEPS] = {8, 16, 24, 32, 48, 64};
//a ride-in has ARRIVED when it stalls within this of its anchor in 3D: the
//body's half-height (32) plus the tow target's inset along the view (16,
//GRAPPLE_MODEL_TOW_TARGET_INSET in the game's bg_grapple_model.h, MUST
//MATCH) plus a frame of slack. A stall farther out is a blocked tow
#define AAS_GRAPPLE_ARRIVE						56
//under a CEILING anchor (one standing at least a body's top above the origin)
//an arrived body hangs within a few units of the anchor's vertical. A stall
//20u or more off it is a lip that stopped the body short, and a drop from
//there lands beside the deck rather than on it, so it is a stall the router
//routes around, not an arrival
#define AAS_GRAPPLE_ARRIVE_CEIL_H				20
#define AAS_GRAPPLE_CEIL_DZ						28
//an arrived body is a body at rest: the stall test compares the anchor
//distance between two thinks, and a body swinging about the anchor shows the
//same distance twice while moving fast. Read off the playerstate's own
//velocity, horizontal
#define AAS_GRAPPLE_ARRIVE_SPEED				40
//the impact speed a DESCENT ride-in may let go at. PM_CrashLand scores a
//landing as delta = v*v*0.0001 and hurts above 40, so 632ups is the edge;
//this keeps a margin under it. Ducking doubles delta, but a towed body is
//not ducked
#define AAS_GRAPPLE_FALL_SAFE_SPEED				600
//one server frame of tow travel, 800ups at 40fps: the armed release's step
//before it has measured one
#define AAS_GRAPPLE_FRAME_STEP					20
//a route shot is pressed within this of the mark, and every fire spot inside
//the disc is covered; EXACT reaches (TRAVELFLAG_GRAPPLEEXACT) fire only from
//the tighter one
#define AAS_GRAPPLE_FIRE_RADIUS					48
#define AAS_GRAPPLE_FIRE_RADIUS_EXACT			16
//type of model, func_plat or func_bobbing
static int modeltypes[MAX_MODELS];

static bot_movestate_t *botmovestates[MAX_CLIENTS+1];
//the frame hook runs once per bot per server frame; it looks its state up
//by client instead of walking every slot
static bot_movestate_t *botmovestate_byclient[MAX_CLIENTS+1];

//========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//========================================================================
int BotAllocMoveState(void)
{
	int i;

	for (i = 1; i <= MAX_CLIENTS; i++)
	{
		if (!botmovestates[i])
		{
			botmovestates[i] = GetClearedMemory(sizeof(bot_movestate_t));
			return i;
		} //end if
	} //end for
	return 0;
} //end of the function BotAllocMoveState
//========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//========================================================================
void BotFreeMoveState(int handle)
{
	bot_movestate_t *ms;

	if (handle <= 0 || handle > MAX_CLIENTS)
	{
		botimport.Print(PRT_FATAL, "move state handle %d out of range\n", handle);
		return;
	} //end if
	if (!botmovestates[handle])
	{
		botimport.Print(PRT_FATAL, "invalid move state %d\n", handle);
		return;
	} //end if
	ms = botmovestates[handle];
	if (ms->client >= 0 && ms->client < MAX_CLIENTS && botmovestate_byclient[ms->client] == ms)
		botmovestate_byclient[ms->client] = NULL;
	FreeMemory(botmovestates[handle]);
	botmovestates[handle] = NULL;
} //end of the function BotFreeMoveState
//========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//========================================================================
static bot_movestate_t *BotMoveStateFromHandle(int handle)
{
	if (handle <= 0 || handle > MAX_CLIENTS)
	{
		botimport.Print(PRT_FATAL, "move state handle %d out of range\n", handle);
		return NULL;
	} //end if
	if (!botmovestates[handle])
	{
		botimport.Print(PRT_FATAL, "invalid move state %d\n", handle);
		return NULL;
	} //end if
	return botmovestates[handle];
} //end of the function BotMoveStateFromHandle
//========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//========================================================================
void BotInitMoveState(int handle, bot_initmove_t *initmove)
{
	bot_movestate_t *ms;

	ms = BotMoveStateFromHandle(handle);
	if (!ms) return;
	VectorCopy(initmove->origin, ms->origin);
	VectorCopy(initmove->velocity, ms->velocity);
	VectorCopy(initmove->viewoffset, ms->viewoffset);
	ms->entitynum = initmove->entitynum;
	ms->client = initmove->client;
	if (ms->client >= 0 && ms->client < MAX_CLIENTS) botmovestate_byclient[ms->client] = ms;
	ms->thinktime = initmove->thinktime;
	ms->presencetype = initmove->presencetype;
	VectorCopy(initmove->viewangles, ms->viewangles);
	//
	ms->moveflags &= ~MFL_ONGROUND;
	if (initmove->or_moveflags & MFL_ONGROUND) ms->moveflags |= MFL_ONGROUND;
	ms->moveflags &= ~MFL_TELEPORTED;	
	if (initmove->or_moveflags & MFL_TELEPORTED) ms->moveflags |= MFL_TELEPORTED;
	ms->moveflags &= ~MFL_WATERJUMP;
	if (initmove->or_moveflags & MFL_WATERJUMP) ms->moveflags |= MFL_WATERJUMP;
	ms->moveflags &= ~MFL_WALK;
	if (initmove->or_moveflags & MFL_WALK) ms->moveflags |= MFL_WALK;
	ms->moveflags &= ~MFL_GRAPPLEPULL;
	if (initmove->or_moveflags & MFL_GRAPPLEPULL) ms->moveflags |= MFL_GRAPPLEPULL;
	ms->moveflags &= ~MFL_HOOKREADY;
	if (initmove->or_moveflags & MFL_HOOKREADY) ms->moveflags |= MFL_HOOKREADY;
	ms->moveflags &= ~MFL_GRAPPLEAIM;
	if (initmove->or_moveflags & MFL_GRAPPLEAIM) ms->moveflags |= MFL_GRAPPLEAIM;
	ms->moveflags &= ~MFL_HOOKOUT;
	if (initmove->or_moveflags & MFL_HOOKOUT) ms->moveflags |= MFL_HOOKOUT;
} //end of the function BotInitMoveState
//========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//========================================================================
static float AngleDiff(float ang1, float ang2)
{
	float diff;

	diff = ang1 - ang2;
	if (ang1 > ang2)
	{
		if (diff > 180.0) diff -= 360.0;
	} //end if
	else
	{
		if (diff < -180.0) diff += 360.0;
	} //end else
	return diff;
} //end of the function AngleDiff
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int BotFuzzyPointReachabilityArea(vec3_t origin)
{
	int firstareanum, j, x, y, z;
	int areas[10], numareas, areanum, bestareanum;
	float dist, bestdist;
	vec3_t points[10], v, end;

	firstareanum = 0;
	areanum = AAS_PointAreaNum(origin);
	if (areanum)
	{
		firstareanum = areanum;
		if (AAS_AreaReachability(areanum)) return areanum;
	} //end if
	VectorCopy(origin, end);
	end[2] += 4;
	numareas = AAS_TraceAreas(origin, end, areas, points, 10);
	for (j = 0; j < numareas; j++)
	{
		if (AAS_AreaReachability(areas[j])) return areas[j];
	} //end for
	bestdist = 999999;
	bestareanum = 0;
	for (z = 1; z >= -1; z -= 1)
	{
		for (x = 1; x >= -1; x -= 1)
		{
			for (y = 1; y >= -1; y -= 1)
			{
				VectorCopy(origin, end);
				end[0] += x * 8;
				end[1] += y * 8;
				end[2] += z * 12;
				numareas = AAS_TraceAreas(origin, end, areas, points, 10);
				for (j = 0; j < numareas; j++)
				{
					if (AAS_AreaReachability(areas[j]))
					{
						VectorSubtract(points[j], origin, v);
						dist = VectorLength(v);
						if (dist < bestdist)
						{
							bestareanum = areas[j];
							bestdist = dist;
						} //end if
					} //end if
					if (!firstareanum) firstareanum = areas[j];
				} //end for
			} //end for
		} //end for
		if (bestareanum) return bestareanum;
	} //end for
	return firstareanum;
} //end of the function BotFuzzyPointReachabilityArea
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int BotReachabilityArea(vec3_t origin, int client)
{
	int modelnum, modeltype, reachnum, areanum;
	aas_reachability_t reach;
	vec3_t org, end, mins, maxs, up = {0, 0, 1};
	bsp_trace_t bsptrace;
	aas_trace_t trace;

	//check if the bot is standing on something
	AAS_PresenceTypeBoundingBox(PRESENCE_CROUCH, mins, maxs);
	VectorMA(origin, -3, up, end);
	bsptrace = AAS_Trace(origin, mins, maxs, end, client, CONTENTS_SOLID|CONTENTS_PLAYERCLIP);
	if (!bsptrace.startsolid && bsptrace.fraction < 1 && bsptrace.ent != ENTITYNUM_NONE)
	{
		//if standing on the world the bot should be in a valid area
		if (bsptrace.ent == ENTITYNUM_WORLD)
		{
			return BotFuzzyPointReachabilityArea(origin);
		} //end if

		modelnum = AAS_EntityModelindex(bsptrace.ent);
		modeltype = modeltypes[modelnum];

		//if standing on a func_plat or func_bobbing then the bot is assumed to be
		//in the area the reachability points to
		if (modeltype == MODELTYPE_FUNC_PLAT || modeltype == MODELTYPE_FUNC_BOB)
		{
			reachnum = AAS_NextModelReachability(0, modelnum);
			if (reachnum)
			{
				AAS_ReachabilityFromNum(reachnum, &reach);
				return reach.areanum;
			} //end if
		} //end else if

		//if the bot is swimming the bot should be in a valid area
		if (AAS_Swimming(origin))
		{
			return BotFuzzyPointReachabilityArea(origin);
		} //end if
		//
		areanum = BotFuzzyPointReachabilityArea(origin);
		//if the bot is in an area with reachabilities
		if (areanum && AAS_AreaReachability(areanum)) return areanum;
		//trace down till the ground is hit because the bot is standing on some other entity
		VectorCopy(origin, org);
		VectorCopy(org, end);
		end[2] -= 800;
		trace = AAS_TraceClientBBox(org, end, PRESENCE_CROUCH, -1);
		if (!trace.startsolid)
		{
			VectorCopy(trace.endpos, org);
		} //end if
		//
		return BotFuzzyPointReachabilityArea(org);
	} //end if
	//
	return BotFuzzyPointReachabilityArea(origin);
} //end of the function BotReachabilityArea
//===========================================================================
// returns the reachability area the bot is in
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
/*
int BotReachabilityArea(vec3_t origin, int testground)
{
	int firstareanum, i, j, x, y, z;
	int areas[10], numareas, areanum, bestareanum;
	float dist, bestdist;
	vec3_t org, end, points[10], v;
	aas_trace_t trace;

	firstareanum = 0;
	for (i = 0; i < 2; i++)
	{
		VectorCopy(origin, org);
		//if test at the ground (used when bot is standing on an entity)
		if (i > 0)
		{
			VectorCopy(origin, end);
			end[2] -= 800;
			trace = AAS_TraceClientBBox(origin, end, PRESENCE_CROUCH, -1);
			if (!trace.startsolid)
			{
				VectorCopy(trace.endpos, org);
			} //end if
		} //end if

		firstareanum = 0;
		areanum = AAS_PointAreaNum(org);
		if (areanum)
		{
			firstareanum = areanum;
			if (AAS_AreaReachability(areanum)) return areanum;
		} //end if
		bestdist = 999999;
		bestareanum = 0;
		for (z = 1; z >= -1; z -= 1)
		{
			for (x = 1; x >= -1; x -= 1)
			{
				for (y = 1; y >= -1; y -= 1)
				{
					VectorCopy(org, end);
					end[0] += x * 8;
					end[1] += y * 8;
					end[2] += z * 12;
					numareas = AAS_TraceAreas(org, end, areas, points, 10);
					for (j = 0; j < numareas; j++)
					{
						if (AAS_AreaReachability(areas[j]))
						{
							VectorSubtract(points[j], org, v);
							dist = VectorLength(v);
							if (dist < bestdist)
							{
								bestareanum = areas[j];
								bestdist = dist;
							} //end if
						} //end if
					} //end for
				} //end for
			} //end for
			if (bestareanum) return bestareanum;
		} //end for
		if (!testground) break;
	} //end for
//#ifdef DEBUG
	//botimport.Print(PRT_MESSAGE, "no reachability area\n");
//#endif //DEBUG
	return firstareanum;
} //end of the function BotReachabilityArea*/
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotOnMover(vec3_t origin, int entnum, aas_reachability_t *reach)
{
	int i, modelnum;
	vec3_t mins, maxs, modelorigin, org, end;
	vec3_t angles = {0, 0, 0};
	vec3_t boxmins = {-16, -16, -8}, boxmaxs = {16, 16, 8};
	bsp_trace_t trace;

	modelnum = reach->facenum & 0x0000FFFF;
	//get some bsp model info
	AAS_BSPModelMinsMaxsOrigin(modelnum, angles, mins, maxs, NULL);
	//
	if (!AAS_OriginOfMoverWithModelNum(modelnum, modelorigin))
	{
		botimport.Print(PRT_MESSAGE, "no entity with model %d\n", modelnum);
		return qfalse;
	} //end if
	//
	for (i = 0; i < 2; i++)
	{
		if (origin[i] > modelorigin[i] + maxs[i] + 16) return qfalse;
		if (origin[i] < modelorigin[i] + mins[i] - 16) return qfalse;
	} //end for
	//
	VectorCopy(origin, org);
	org[2] += 24;
	VectorCopy(origin, end);
	end[2] -= 48;
	//
	trace = AAS_Trace(org, boxmins, boxmaxs, end, entnum, CONTENTS_SOLID|CONTENTS_PLAYERCLIP);
	if (!trace.startsolid && !trace.allsolid)
	{
		//NOTE: the reachability face number is the model number of the elevator
		if (trace.ent != ENTITYNUM_NONE && AAS_EntityModelNum(trace.ent) == modelnum)
		{
			return qtrue;
		} //end if
	} //end if
	return qfalse;
} //end of the function BotOnMover
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int MoverDown(aas_reachability_t *reach)
{
	int modelnum;
	vec3_t mins, maxs, origin;
	vec3_t angles = {0, 0, 0};

	modelnum = reach->facenum & 0x0000FFFF;
	//get some bsp model info
	AAS_BSPModelMinsMaxsOrigin(modelnum, angles, mins, maxs, origin);
	//
	if (!AAS_OriginOfMoverWithModelNum(modelnum, origin))
	{
		botimport.Print(PRT_MESSAGE, "no entity with model %d\n", modelnum);
		return qfalse;
	} //end if
	//if the top of the plat is below the reachability start point
	if (origin[2] + maxs[2] < reach->start[2]) return qtrue;
	return qfalse;
} //end of the function MoverDown
//========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//========================================================================
void BotSetBrushModelTypes(void)
{
	int ent, modelnum;
	char classname[MAX_EPAIRKEY], model[MAX_EPAIRKEY];

	Com_Memset(modeltypes, 0, MAX_MODELS * sizeof(int));
	//
	for (ent = AAS_NextBSPEntity(0); ent; ent = AAS_NextBSPEntity(ent))
	{
		if (!AAS_ValueForBSPEpairKey(ent, "classname", classname, MAX_EPAIRKEY)) continue;
		if (!AAS_ValueForBSPEpairKey(ent, "model", model, MAX_EPAIRKEY)) continue;
		if (model[0]) modelnum = atoi(model+1);
		else modelnum = 0;

		if (modelnum < 0 || modelnum >= MAX_MODELS)
		{
			botimport.Print(PRT_MESSAGE, "entity %s model number out of range\n", classname);
			continue;
		} //end if

		if (!Q_stricmp(classname, "func_bobbing"))
			modeltypes[modelnum] = MODELTYPE_FUNC_BOB;
		else if (!Q_stricmp(classname, "func_plat"))
			modeltypes[modelnum] = MODELTYPE_FUNC_PLAT;
		else if (!Q_stricmp(classname, "func_door"))
			modeltypes[modelnum] = MODELTYPE_FUNC_DOOR;
		else if (!Q_stricmp(classname, "func_static"))
			modeltypes[modelnum] = MODELTYPE_FUNC_STATIC;
	} //end for
} //end of the function BotSetBrushModelTypes
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotOnTopOfEntity(bot_movestate_t *ms)
{
	vec3_t mins, maxs, end, up = {0, 0, 1};
	bsp_trace_t trace;

	AAS_PresenceTypeBoundingBox(ms->presencetype, mins, maxs);
	VectorMA(ms->origin, -3, up, end);
	trace = AAS_Trace(ms->origin, mins, maxs, end, ms->entitynum, CONTENTS_SOLID|CONTENTS_PLAYERCLIP);
	if (!trace.startsolid && (trace.ent != ENTITYNUM_WORLD && trace.ent != ENTITYNUM_NONE) )
	{
		return trace.ent;
	} //end if
	return -1;
} //end of the function BotOnTopOfEntity
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotValidTravel(vec3_t origin, aas_reachability_t *reach, int travelflags)
{
	//if the reachability uses an unwanted travel type
	if (AAS_TravelFlagForType(reach->traveltype) & ~travelflags) return qfalse;
	//don't go into areas with bad travel types
	if (AAS_AreaContentsTravelFlags(reach->areanum) & ~travelflags) return qfalse;
	return qtrue;
} //end of the function BotValidTravel
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static void BotAddToAvoidReach(bot_movestate_t *ms, int number, float avoidtime)
{
	int i;

	for (i = 0; i < MAX_AVOIDREACH; i++)
	{
		if (ms->avoidreach[i] == number)
		{
			if (ms->avoidreachtimes[i] > AAS_Time()) ms->avoidreachtries[i]++;
			else ms->avoidreachtries[i] = 1;
			ms->avoidreachtimes[i] = AAS_Time() + avoidtime;
			return;
		} //end if
	} //end for
	//add the reachability to the reachabilities to avoid for a while
	for (i = 0; i < MAX_AVOIDREACH; i++)
	{
		if (ms->avoidreachtimes[i] < AAS_Time())
		{
			ms->avoidreach[i] = number;
			ms->avoidreachtimes[i] = AAS_Time() + avoidtime;
			ms->avoidreachtries[i] = 1;
			return;
		} //end if
	} //end for
} //end of the function BotAddToAvoidReach
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static float DistanceFromLineSquared(vec3_t p, vec3_t lp1, vec3_t lp2)
{
	vec3_t proj, dir;
	int j;

	AAS_ProjectPointOntoVector(p, lp1, lp2, proj);
	for (j = 0; j < 3; j++)
		if ((proj[j] > lp1[j] && proj[j] > lp2[j]) ||
			(proj[j] < lp1[j] && proj[j] < lp2[j]))
			break;
	if (j < 3) {
		if (fabs(proj[j] - lp1[j]) < fabs(proj[j] - lp2[j]))
			VectorSubtract(p, lp1, dir);
		else
			VectorSubtract(p, lp2, dir);
		return VectorLengthSquared(dir);
	}
	VectorSubtract(p, proj, dir);
	return VectorLengthSquared(dir);
} //end of the function DistanceFromLineSquared
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static float VectorDistanceSquared(const vec3_t p1, const vec3_t p2)
{
	vec3_t dir;
	VectorSubtract(p2, p1, dir);
	return VectorLengthSquared(dir);
} //end of the function VectorDistanceSquared
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotAvoidSpots(vec3_t origin, aas_reachability_t *reach, bot_avoidspot_t *avoidspots, int numavoidspots)
{
	int checkbetween, i, type;
	float squareddist, squaredradius;

	switch(reach->traveltype & TRAVELTYPE_MASK)
	{
		case TRAVEL_WALK: checkbetween = qtrue; break;
		case TRAVEL_CROUCH: checkbetween = qtrue; break;
		case TRAVEL_BARRIERJUMP: checkbetween = qtrue; break;
		case TRAVEL_LADDER: checkbetween = qtrue; break;
		case TRAVEL_WALKOFFLEDGE: checkbetween = qfalse; break;
		case TRAVEL_JUMP: checkbetween = qfalse; break;
		case TRAVEL_SWIM: checkbetween = qtrue; break;
		case TRAVEL_WATERJUMP: checkbetween = qtrue; break;
		case TRAVEL_TELEPORT: checkbetween = qfalse; break;
		case TRAVEL_ELEVATOR: checkbetween = qfalse; break;
		case TRAVEL_GRAPPLEHOOK: checkbetween = qfalse; break;
		case TRAVEL_ROCKETJUMP: checkbetween = qfalse; break;
		case TRAVEL_BFGJUMP: checkbetween = qfalse; break;
		case TRAVEL_JUMPPAD: checkbetween = qfalse; break;
		case TRAVEL_FUNCBOB: checkbetween = qfalse; break;
		default: checkbetween = qtrue; break;
	} //end switch

	type = AVOID_CLEAR;
	for (i = 0; i < numavoidspots; i++)
	{
		squaredradius = Square(avoidspots[i].radius);
		squareddist = DistanceFromLineSquared(avoidspots[i].origin, origin, reach->start);
		// if moving towards the avoid spot
		if (squareddist < squaredradius &&
			VectorDistanceSquared(avoidspots[i].origin, origin) > squareddist)
		{
			type = avoidspots[i].type;
		} //end if
		else if (checkbetween) {
			squareddist = DistanceFromLineSquared(avoidspots[i].origin, reach->start, reach->end);
			// if moving towards the avoid spot
			if (squareddist < squaredradius &&
				VectorDistanceSquared(avoidspots[i].origin, reach->start) > squareddist)
			{
				type = avoidspots[i].type;
			} //end if
		} //end if
		else
		{
			VectorDistanceSquared(avoidspots[i].origin, reach->end);
			// if the reachability leads closer to the avoid spot
			if (squareddist < squaredradius && 
				VectorDistanceSquared(avoidspots[i].origin, reach->start) > squareddist)
			{
				type = avoidspots[i].type;
			} //end if
		} //end else
		if (type == AVOID_ALWAYS)
			return type;
	} //end for
	return type;
} //end of the function BotAvoidSpots
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
void BotAddAvoidSpot(int movestate, const vec3_t origin, float radius, int type)
{
	bot_movestate_t *ms;

	ms = BotMoveStateFromHandle(movestate);
	if (!ms) return;
	if (type == AVOID_CLEAR)
	{
		ms->numavoidspots = 0;
		return;
	} //end if

	if (ms->numavoidspots >= MAX_AVOIDSPOTS)
		return;
	VectorCopy(origin, ms->avoidspots[ms->numavoidspots].origin);
	ms->avoidspots[ms->numavoidspots].radius = radius;
	ms->avoidspots[ms->numavoidspots].type = type;
	ms->numavoidspots++;
} //end of the function BotAddAvoidSpot
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int BotGetReachabilityToGoal(vec3_t origin, int areanum,
									  int lastgoalareanum, int lastareanum,
									  int *avoidreach, float *avoidreachtimes, int *avoidreachtries,
									  bot_goal_t *goal, int travelflags,
									  struct bot_avoidspot_s *avoidspots, int numavoidspots, int *flags)
{
	int i, t, besttime, bestreachnum, reachnum;
	aas_reachability_t reach;

	//if not in a valid area
	if (!areanum) return 0;
	//
	if (AAS_AreaDoNotEnter(areanum) || AAS_AreaDoNotEnter(goal->areanum))
	{
		travelflags |= TFL_DONOTENTER;
	} //end if
	//use the routing to find the next area to go to
	besttime = 0;
	bestreachnum = 0;
	//
	for (reachnum = AAS_NextAreaReachability(areanum, 0); reachnum;
		reachnum = AAS_NextAreaReachability(areanum, reachnum))
	{
#ifdef AVOIDREACH
		//check if it isn't a reachability to avoid
		for (i = 0; i < MAX_AVOIDREACH; i++)
		{
			if (avoidreach[i] == reachnum && avoidreachtimes[i] >= AAS_Time()) break;
		} //end for
		if (i != MAX_AVOIDREACH && avoidreachtries[i] > AVOIDREACH_TRIES)
		{
#ifdef DEBUG
			if (botDeveloper)
			{
				botimport.Print(PRT_MESSAGE, "avoiding reachability %d\n", avoidreach[i]);
			} //end if
#endif //DEBUG
			continue;
		} //end if
#endif //AVOIDREACH
		//get the reachability from the number
		AAS_ReachabilityFromNum(reachnum, &reach);
		//NOTE: do not go back to the previous area if the goal didn't change
		//NOTE: is this actually avoidance of local routing minima between two areas???
		if (lastgoalareanum == goal->areanum && reach.areanum == lastareanum) continue;
		//if (AAS_AreaContentsTravelFlags(reach.areanum) & ~travelflags) continue;
		//if the travel isn't valid
		if (!BotValidTravel(origin, &reach, travelflags)) continue;
		//get the travel time
		t = AAS_AreaTravelTimeToGoalArea(reach.areanum, reach.end, goal->areanum, travelflags);
		//if the goal area isn't reachable from the reachable area
		if (!t) continue;
		//if the bot should not use this reachability to avoid bad spots
		if (BotAvoidSpots(origin, &reach, avoidspots, numavoidspots)) {
			if (flags) {
				*flags |= MOVERESULT_BLOCKEDBYAVOIDSPOT;
			}
			continue;
		}
		//add the travel time towards the area
		t += reach.traveltime;// + AAS_AreaTravelTime(areanum, origin, reach.start);
		//if the travel time is better than the ones already found
		if (!besttime || t < besttime)
		{
			besttime = t;
			bestreachnum = reachnum;
		} //end if
	} //end for
	//
	return bestreachnum;
} //end of the function BotGetReachabilityToGoal
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int BotAddToTarget(vec3_t start, vec3_t end, float maxdist, float *dist, vec3_t target)
{
	vec3_t dir;
	float curdist;

	VectorSubtract(end, start, dir);
	curdist = VectorNormalize(dir);
	if (*dist + curdist < maxdist)
	{
		VectorCopy(end, target);
		*dist += curdist;
		return qfalse;
	} //end if
	else
	{
		VectorMA(start, maxdist - *dist, dir, target);
		*dist = maxdist;
		return qtrue;
	} //end else
} //end of the function BotAddToTarget

int BotMovementViewTarget(int movestate, bot_goal_t *goal, int travelflags, float lookahead, vec3_t target)
{
	aas_reachability_t reach;
	int reachnum, lastareanum;
	bot_movestate_t *ms;
	vec3_t end;
	float dist;

	ms = BotMoveStateFromHandle(movestate);
	if (!ms) return qfalse;
	//if the bot has no goal or no last reachability
	if (!ms->lastreachnum || !goal) return qfalse;

	reachnum = ms->lastreachnum;
	VectorCopy(ms->origin, end);
	lastareanum = ms->lastareanum;
	dist = 0;
	while(reachnum && dist < lookahead)
	{
		AAS_ReachabilityFromNum(reachnum, &reach);
		if (BotAddToTarget(end, reach.start, lookahead, &dist, target)) return qtrue;
		//never look beyond teleporters
		if ((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_TELEPORT) return qtrue;
		//never look beyond the weapon jump point
		if ((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_ROCKETJUMP) return qtrue;
		if ((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_BFGJUMP) return qtrue;
		//don't add jump pad distances
		if ((reach.traveltype & TRAVELTYPE_MASK) != TRAVEL_JUMPPAD &&
			(reach.traveltype & TRAVELTYPE_MASK) != TRAVEL_ELEVATOR &&
			(reach.traveltype & TRAVELTYPE_MASK) != TRAVEL_FUNCBOB)
		{
			if (BotAddToTarget(reach.start, reach.end, lookahead, &dist, target)) return qtrue;
		} //end if
		reachnum = BotGetReachabilityToGoal(reach.end, reach.areanum,
						ms->lastgoalareanum, lastareanum,
							ms->avoidreach, ms->avoidreachtimes, ms->avoidreachtries,
									goal, travelflags, NULL, 0, NULL);
		VectorCopy(reach.end, end);
		lastareanum = reach.areanum;
		if (lastareanum == goal->areanum)
		{
			BotAddToTarget(reach.end, goal->origin, lookahead, &dist, target);
			return qtrue;
		} //end if
	} //end while
	//
	return qfalse;
} //end of the function BotMovementViewTarget
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotVisible(int ent, vec3_t eye, vec3_t target)
{
	bsp_trace_t trace;

	trace = AAS_Trace(eye, NULL, NULL, target, ent, CONTENTS_SOLID|CONTENTS_PLAYERCLIP);
	if (trace.fraction >= 1) return qtrue;
	return qfalse;
} //end of the function BotVisible
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int BotPredictVisiblePosition(vec3_t origin, int areanum, bot_goal_t *goal, int travelflags, vec3_t target)
{
	aas_reachability_t reach;
	int reachnum, lastgoalareanum, lastareanum, i;
	int avoidreach[MAX_AVOIDREACH];
	float avoidreachtimes[MAX_AVOIDREACH];
	int avoidreachtries[MAX_AVOIDREACH];
	vec3_t end;

	//if the bot has no goal or no last reachability
	if (!goal) return qfalse;
	//if the areanum is not valid
	if (!areanum) return qfalse;
	//if the goal areanum is not valid
	if (!goal->areanum) return qfalse;

	Com_Memset(avoidreach, 0, MAX_AVOIDREACH * sizeof(int));
	lastgoalareanum = goal->areanum;
	lastareanum = areanum;
	VectorCopy(origin, end);
	//only do 20 hops
	for (i = 0; i < 20 && (areanum != goal->areanum); i++)
	{
		//
		reachnum = BotGetReachabilityToGoal(end, areanum,
						lastgoalareanum, lastareanum,
							avoidreach, avoidreachtimes, avoidreachtries,
									goal, travelflags, NULL, 0, NULL);
		if (!reachnum) return qfalse;
		AAS_ReachabilityFromNum(reachnum, &reach);
		//
		if (BotVisible(goal->entitynum, goal->origin, reach.start))
		{
			VectorCopy(reach.start, target);
			return qtrue;
		} //end if
		//
		if (BotVisible(goal->entitynum, goal->origin, reach.end))
		{
			VectorCopy(reach.end, target);
			return qtrue;
		} //end if
		//
		if (reach.areanum == goal->areanum)
		{
			VectorCopy(reach.end, target);
			return qtrue;
		} //end if
		//
		lastareanum = areanum;
		areanum = reach.areanum;
		VectorCopy(reach.end, end);
		//
	} //end while
	//
	return qfalse;
} //end of the function BotPredictVisiblePosition
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static void MoverBottomCenter(aas_reachability_t *reach, vec3_t bottomcenter)
{
	int modelnum;
	vec3_t mins, maxs, origin, mids;
	vec3_t angles = {0, 0, 0};

	modelnum = reach->facenum & 0x0000FFFF;
	//get some bsp model info
	AAS_BSPModelMinsMaxsOrigin(modelnum, angles, mins, maxs, origin);
	//
	if (!AAS_OriginOfMoverWithModelNum(modelnum, origin))
	{
		botimport.Print(PRT_MESSAGE, "no entity with model %d\n", modelnum);
	} //end if
	//get a point just above the plat in the bottom position
	VectorAdd(mins, maxs, mids);
	VectorMA(origin, 0.5, mids, bottomcenter);
	bottomcenter[2] = reach->start[2];
} //end of the function MoverBottomCenter
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static float BotGapDistance(vec3_t origin, vec3_t hordir, int entnum)
{
	int dist;
	float startz;
	vec3_t start, end;
	aas_trace_t trace;

	//do gap checking
	//startz = origin[2];
	//this enables walking down stairs more fluidly
	{
		VectorCopy(origin, start);
		VectorCopy(origin, end);
		end[2] -= 60;
		trace = AAS_TraceClientBBox(start, end, PRESENCE_CROUCH, entnum);
		if (trace.fraction >= 1) return 1;
		startz = trace.endpos[2] + 1;
	}
	//
	for (dist = 8; dist <= 100; dist += 8)
	{
		VectorMA(origin, dist, hordir, start);
		start[2] = startz + 24;
		VectorCopy(start, end);
		end[2] -= 48 + sv_maxbarrier->value;
		trace = AAS_TraceClientBBox(start, end, PRESENCE_CROUCH, entnum);
		//if solid is found the bot can't walk any further and fall into a gap
		if (!trace.startsolid)
		{
			//if it is a gap
			if (trace.endpos[2] < startz - sv_maxstep->value - 8)
			{
				VectorCopy(trace.endpos, end);
				end[2] -= 20;
				if (AAS_PointContents(end) & CONTENTS_WATER) break;
				//if a gap is found slow down
				//botimport.Print(PRT_MESSAGE, "gap at %i\n", dist);
				return dist;
			} //end if
			startz = trace.endpos[2];
		} //end if
	} //end for
	return 0;
} //end of the function BotGapDistance
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotCheckBarrierJump(bot_movestate_t *ms, vec3_t dir, float speed)
{
	vec3_t start, hordir, end;
	aas_trace_t trace;

	VectorCopy(ms->origin, end);
	end[2] += sv_maxbarrier->value;
	//trace right up
	trace = AAS_TraceClientBBox(ms->origin, end, PRESENCE_NORMAL, ms->entitynum);
	//this shouldn't happen... but we check anyway
	if (trace.startsolid) return qfalse;
	//if very low ceiling it isn't possible to jump up to a barrier
	if (trace.endpos[2] - ms->origin[2] < sv_maxstep->value) return qfalse;
	//
	hordir[0] = dir[0];
	hordir[1] = dir[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	VectorMA(ms->origin, ms->thinktime * speed * 0.5, hordir, end);
	VectorCopy(trace.endpos, start);
	end[2] = trace.endpos[2];
	//trace from previous trace end pos horizontally in the move direction
	trace = AAS_TraceClientBBox(start, end, PRESENCE_NORMAL, ms->entitynum);
	//again this shouldn't happen
	if (trace.startsolid) return qfalse;
	//
	VectorCopy(trace.endpos, start);
	VectorCopy(trace.endpos, end);
	end[2] = ms->origin[2];
	//trace down from the previous trace end pos
	trace = AAS_TraceClientBBox(start, end, PRESENCE_NORMAL, ms->entitynum);
	//if solid
	if (trace.startsolid) return qfalse;
	//if no obstacle at all
	if (trace.fraction >= 1.0) return qfalse;
	//if less than the maximum step height
	if (trace.endpos[2] - ms->origin[2] < sv_maxstep->value) return qfalse;
	//
	EA_Jump(ms->client);
	EA_Move(ms->client, hordir, speed);
	ms->moveflags |= MFL_BARRIERJUMP;
	//there is a barrier
	return qtrue;
} //end of the function BotCheckBarrierJump
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotSwimInDirection(bot_movestate_t *ms, vec3_t dir, float speed, int type)
{
	vec3_t normdir;

	VectorCopy(dir, normdir);
	VectorNormalize(normdir);
	EA_Move(ms->client, normdir, speed);
	return qtrue;
} //end of the function BotSwimInDirection
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotWalkInDirection(bot_movestate_t *ms, vec3_t dir, float speed, int type)
{
	vec3_t hordir, cmdmove, velocity, tmpdir, origin;
	int presencetype, maxframes, cmdframes, stopevent;
	aas_clientmove_t move;
	float dist;

	if (AAS_OnGround(ms->origin, ms->presencetype, ms->entitynum)) ms->moveflags |= MFL_ONGROUND;
	//if the bot is on the ground
	if (ms->moveflags & MFL_ONGROUND)
	{
		//if there is a barrier the bot can jump on
		if (BotCheckBarrierJump(ms, dir, speed)) return qtrue;
		//remove barrier jump flag
		ms->moveflags &= ~MFL_BARRIERJUMP;
		//get the presence type for the movement
		if ((type & MOVE_CROUCH) && !(type & MOVE_JUMP)) presencetype = PRESENCE_CROUCH;
		else presencetype = PRESENCE_NORMAL;
		//horizontal direction
		hordir[0] = dir[0];
		hordir[1] = dir[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//if the bot is not supposed to jump
		if (!(type & MOVE_JUMP))
		{
			//if there is a gap, try to jump over it
			if (BotGapDistance(ms->origin, hordir, ms->entitynum) > 0) type |= MOVE_JUMP;
		} //end if
		//get command movement
		VectorScale(hordir, speed, cmdmove);
		VectorCopy(ms->velocity, velocity);
		//
		if (type & MOVE_JUMP)
		{
			//botimport.Print(PRT_MESSAGE, "trying jump\n");
			cmdmove[2] = 400;
			maxframes = PREDICTIONTIME_JUMP / 0.1;
			cmdframes = 1;
			stopevent = SE_HITGROUND|SE_HITGROUNDDAMAGE|
						SE_ENTERWATER|SE_ENTERSLIME|SE_ENTERLAVA;
		} //end if
		else
		{
			maxframes = 2;
			cmdframes = 2;
			stopevent = SE_HITGROUNDDAMAGE|
						SE_ENTERWATER|SE_ENTERSLIME|SE_ENTERLAVA;
		} //end else
		//AAS_ClearShownDebugLines();
		//
		VectorCopy(ms->origin, origin);
		origin[2] += 0.5;
		AAS_PredictClientMovement(&move, ms->entitynum, origin, presencetype, qtrue,
									velocity, cmdmove, cmdframes, maxframes, 0.1f,
									stopevent, 0, qfalse);//qtrue);
		//if prediction time wasn't enough to fully predict the movement
		if (move.frames >= maxframes && (type & MOVE_JUMP))
		{
			//botimport.Print(PRT_MESSAGE, "client %d: max prediction frames\n", ms->client);
			return qfalse;
		} //end if
		//don't enter slime or lava and don't fall from too high
		if (move.stopevent & (SE_ENTERSLIME|SE_ENTERLAVA|SE_HITGROUNDDAMAGE))
		{
			//botimport.Print(PRT_MESSAGE, "client %d: would be hurt ", ms->client);
			//if (move.stopevent & SE_ENTERSLIME) botimport.Print(PRT_MESSAGE, "slime\n");
			//if (move.stopevent & SE_ENTERLAVA) botimport.Print(PRT_MESSAGE, "lava\n");
			//if (move.stopevent & SE_HITGROUNDDAMAGE) botimport.Print(PRT_MESSAGE, "hitground\n");
			return qfalse;
		} //end if
		//if ground was hit
		if (move.stopevent & SE_HITGROUND)
		{
			//check for nearby gap
			VectorNormalize2(move.velocity, tmpdir);
			dist = BotGapDistance(move.endpos, tmpdir, ms->entitynum);
			if (dist > 0) return qfalse;
			//
			dist = BotGapDistance(move.endpos, hordir, ms->entitynum);
			if (dist > 0) return qfalse;
		} //end if
		//get horizontal movement
		tmpdir[0] = move.endpos[0] - ms->origin[0];
		tmpdir[1] = move.endpos[1] - ms->origin[1];
		tmpdir[2] = 0;
		//
		//AAS_DrawCross(move.endpos, 4, LINECOLOR_BLUE);
		//the bot is blocked by something
		if (VectorLength(tmpdir) < speed * ms->thinktime * 0.5) return qfalse;
		//perform the movement
		if (type & MOVE_JUMP) EA_Jump(ms->client);
		if (type & MOVE_CROUCH) EA_Crouch(ms->client);
		EA_Move(ms->client, hordir, speed);
		//movement was successful
		return qtrue;
	} //end if
	else
	{
		if (ms->moveflags & MFL_BARRIERJUMP)
		{
			//if near the top or going down
			if (ms->velocity[2] < 50)
			{
				EA_Move(ms->client, dir, speed);
			} //end if
		} //end if
		//FIXME: do air control to avoid hazards
		return qtrue;
	} //end else
} //end of the function BotWalkInDirection
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int BotMoveInDirection(int movestate, vec3_t dir, float speed, int type)
{
	bot_movestate_t *ms;

	ms = BotMoveStateFromHandle(movestate);
	if (!ms) return qfalse;
	//if swimming
	if (AAS_Swimming(ms->origin))
	{
		return BotSwimInDirection(ms, dir, speed, type);
	} //end if
	else
	{
		return BotWalkInDirection(ms, dir, speed, type);
	} //end else
} //end of the function BotMoveInDirection
#if 0
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int Intersection(vec2_t p1, vec2_t p2, vec2_t p3, vec2_t p4, vec2_t out)
{
   float x1, dx1, dy1, x2, dx2, dy2, d;

   dx1 = p2[0] - p1[0];
   dy1 = p2[1] - p1[1];
   dx2 = p4[0] - p3[0];
   dy2 = p4[1] - p3[1];

   d = dy1 * dx2 - dx1 * dy2;
   if (d != 0)
   {
      x1 = p1[1] * dx1 - p1[0] * dy1;
      x2 = p3[1] * dx2 - p3[0] * dy2;
      out[0] = (int) ((dx1 * x2 - dx2 * x1) / d);
      out[1] = (int) ((dy1 * x2 - dy2 * x1) / d);
		return qtrue;
   } //end if
   else
   {
      return qfalse;
   } //end else
} //end of the function Intersection
#endif
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static void BotCheckBlocked(bot_movestate_t *ms, vec3_t dir, int checkbottom, bot_moveresult_t *result)
{
	vec3_t mins, maxs, end, up = {0, 0, 1};
	bsp_trace_t trace;

	//test for entities obstructing the bot's path
	AAS_PresenceTypeBoundingBox(ms->presencetype, mins, maxs);
	//
	if (fabs(DotProduct(dir, up)) < 0.7)
	{
		mins[2] += sv_maxstep->value; //if the bot can step on
		maxs[2] -= 10; //a little lower to avoid low ceiling
	} //end if
	VectorMA(ms->origin, 3, dir, end);
	trace = AAS_Trace(ms->origin, mins, maxs, end, ms->entitynum, CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_BODY);
	//if not started in solid and not hitting the world entity
	if (!trace.startsolid && (trace.ent != ENTITYNUM_WORLD && trace.ent != ENTITYNUM_NONE) )
	{
		result->blocked = qtrue;
		result->blockentity = trace.ent;
#ifdef DEBUG
		//botimport.Print(PRT_MESSAGE, "%d: BotCheckBlocked: I'm blocked\n", ms->client);
#endif //DEBUG
	} //end if
	//if not in an area with reachability
	else if (checkbottom && !AAS_AreaReachability(ms->areanum))
	{
		//check if the bot is standing on something
		AAS_PresenceTypeBoundingBox(ms->presencetype, mins, maxs);
		VectorMA(ms->origin, -3, up, end);
		trace = AAS_Trace(ms->origin, mins, maxs, end, ms->entitynum, CONTENTS_SOLID|CONTENTS_PLAYERCLIP);
		if (!trace.startsolid && (trace.ent != ENTITYNUM_WORLD && trace.ent != ENTITYNUM_NONE) )
		{
			result->blocked = qtrue;
			result->blockentity = trace.ent;
			result->flags |= MOVERESULT_ONTOPOFOBSTACLE;
#ifdef DEBUG
			//botimport.Print(PRT_MESSAGE, "%d: BotCheckBlocked: I'm blocked\n", ms->client);
#endif //DEBUG
		} //end if
	} //end else
} //end of the function BotCheckBlocked
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_Walk(bot_movestate_t *ms, aas_reachability_t *reach)
{
	float dist, speed;
	vec3_t hordir;
	bot_moveresult_t_cleared( result );

	//first walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	//
	if (dist < 10)
	{
		//walk straight to the reachability end
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		dist = VectorNormalize(hordir);
	} //end if
	//if going towards a crouch area
	if (!(AAS_AreaPresenceType(reach->areanum) & PRESENCE_NORMAL))
	{
		//if pretty close to the reachable area
		if (dist < 20) EA_Crouch(ms->client);
	} //end if
	//
	dist = BotGapDistance(ms->origin, hordir, ms->entitynum);
	//
	if (ms->moveflags & MFL_WALK)
	{
		if (dist > 0) speed = 200 - (180 - 1 * dist);
		else speed = 200;
		EA_Walk(ms->client);
	} //end if
	else
	{
		if (dist > 0) speed = 400 - (360 - 2 * dist);
		else speed = 400;
	} //end else
	//elementary action move in direction
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_Walk
#if 0
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_Walk(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	float dist, speed;
	bot_moveresult_t_cleared( result );
	//if not on the ground and changed areas... don't walk back!!
	//(doesn't seem to help)
	/*
	ms->areanum = BotFuzzyPointReachabilityArea(ms->origin);
	if (ms->areanum == reach->areanum)
	{
#ifdef DEBUG
		botimport.Print(PRT_MESSAGE, "BotFinishTravel_Walk: already in reach area\n");
#endif //DEBUG
		return result;
	} //end if*/
	//go straight to the reachability end
	hordir[0] = reach->end[0] - ms->origin[0];
	hordir[1] = reach->end[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	if (dist > 100) dist = 100;
	speed = 400 - (400 - 3 * dist);
	//
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotFinishTravel_Walk
#endif
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_Crouch(bot_movestate_t *ms, aas_reachability_t *reach)
{
	float speed;
	vec3_t hordir;
	bot_moveresult_t_cleared( result );

	//
	speed = 400;
	//walk straight to reachability end
	hordir[0] = reach->end[0] - ms->origin[0];
	hordir[1] = reach->end[1] - ms->origin[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	//elementary actions
	EA_Crouch(ms->client);
	EA_Move(ms->client, hordir, speed);
	//
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_Crouch
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_BarrierJump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	float dist, speed;
	vec3_t hordir;
	bot_moveresult_t_cleared( result );

	//walk straight to reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	//if pretty close to the barrier
	if (dist < 9)
	{
		EA_Jump(ms->client);
	} //end if
	else
	{
		if (dist > 60) dist = 60;
		speed = 360 - (360 - 6 * dist);
		EA_Move(ms->client, hordir, speed);
	} //end else
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_BarrierJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_BarrierJump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	bot_moveresult_t_cleared( result );

	//if near the top or going down
	if (ms->velocity[2] < 250)
	{
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		//
		BotCheckBlocked(ms, hordir, qtrue, &result);
		//
		EA_Move(ms->client, hordir, 400);
		VectorCopy(hordir, result.movedir);
	} //end if
	//
	return result;
} //end of the function BotFinishTravel_BarrierJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_Swim(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t dir;
	bot_moveresult_t_cleared( result );

	//swim straight to reachability end
	VectorSubtract(reach->start, ms->origin, dir);
	VectorNormalize(dir);
	//
	BotCheckBlocked(ms, dir, qtrue, &result);
	//elementary actions
	EA_Move(ms->client, dir, 400);
	//
	VectorCopy(dir, result.movedir);
	Vector2Angles(dir, result.ideal_viewangles);
	result.flags |= MOVERESULT_SWIMVIEW;
	//
	return result;
} //end of the function BotTravel_Swim
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_WaterJump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t dir, hordir;
	float dist;
	bot_moveresult_t_cleared( result );

	//swim straight to reachability end
	VectorSubtract(reach->end, ms->origin, dir);
	VectorCopy(dir, hordir);
	hordir[2] = 0;
	dir[2] += 15 + crandom() * 40;
	//botimport.Print(PRT_MESSAGE, "BotTravel_WaterJump: dir[2] = %f\n", dir[2]);
	VectorNormalize(dir);
	dist = VectorNormalize(hordir);
	//elementary actions
	//EA_Move(ms->client, dir, 400);
	EA_MoveForward(ms->client);
	//move up if close to the actual out of water jump spot
	if (dist < 40) EA_MoveUp(ms->client);
	//set the ideal view angles
	Vector2Angles(dir, result.ideal_viewangles);
	result.flags |= MOVERESULT_MOVEMENTVIEW;
	//
	VectorCopy(dir, result.movedir);
	//
	return result;
} //end of the function BotTravel_WaterJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_WaterJump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t dir, pnt;
	bot_moveresult_t_cleared( result );

	//botimport.Print(PRT_MESSAGE, "BotFinishTravel_WaterJump\n");
	//if waterjumping there's nothing to do
	if (ms->moveflags & MFL_WATERJUMP) return result;
	//if not touching any water anymore don't do anything
	//otherwise the bot sometimes keeps jumping?
	VectorCopy(ms->origin, pnt);
	pnt[2] -= 32;	//extra for q2dm4 near red armor/mega health
	if (!(AAS_PointContents(pnt) & (CONTENTS_LAVA|CONTENTS_SLIME|CONTENTS_WATER))) return result;
	//swim straight to reachability end
	VectorSubtract(reach->end, ms->origin, dir);
	dir[0] += crandom() * 10;
	dir[1] += crandom() * 10;
	dir[2] += 70 + crandom() * 10;
	//elementary actions
	EA_Move(ms->client, dir, 400);
	//set the ideal view angles
	Vector2Angles(dir, result.ideal_viewangles);
	result.flags |= MOVERESULT_MOVEMENTVIEW;
	//
	VectorCopy(dir, result.movedir);
	//
	return result;
} //end of the function BotFinishTravel_WaterJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_WalkOffLedge(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir, dir;
	float dist, speed, reachhordist;
	bot_moveresult_t_cleared( result );

	//check if the bot is blocked by anything
	VectorSubtract(reach->start, ms->origin, dir);
	VectorNormalize(dir);
	BotCheckBlocked(ms, dir, qtrue, &result);
	//if the reachability start and end are practically above each other
	VectorSubtract(reach->end, reach->start, dir);
	dir[2] = 0;
	reachhordist = VectorLength(dir);
	//walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//if pretty close to the start focus on the reachability end
	if (dist < 48)
	{
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//
		if (reachhordist < 20)
		{
			speed = 100;
		} //end if
		else if (!AAS_HorizontalVelocityForJump(0, reach->start, reach->end, &speed))
		{
			speed = 400;
		} //end if
	} //end if
	else
	{
		if (reachhordist < 20)
		{
			if (dist > 64) dist = 64;
			speed = 400 - (256 - 4 * dist);
		} //end if
		else
		{
			speed = 400;
		} //end else
	} //end else
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	//elementary action
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_WalkOffLedge
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int BotAirControl(vec3_t origin, vec3_t velocity, vec3_t goal, vec3_t dir, float *speed)
{
	vec3_t org, vel;
	float dist;
	int i;

	VectorCopy(origin, org);
	VectorScale(velocity, 0.1, vel);
	for (i = 0; i < 50; i++)
	{
		vel[2] -= sv_gravity->value * 0.01;
		//if going down and next position would be below the goal
		if (vel[2] < 0 && org[2] + vel[2] < goal[2])
		{
			VectorScale(vel, (goal[2] - org[2]) / vel[2], vel);
			VectorAdd(org, vel, org);
			VectorSubtract(goal, org, dir);
			dist = VectorNormalize(dir);
			if (dist > 32) dist = 32;
			*speed = 400 - (400 - 13 * dist);
			return qtrue;
		} //end if
		else
		{
			VectorAdd(org, vel, org);
		} //end else
	} //end for
	VectorSet(dir, 0, 0, 0);
	*speed = 400;
	return qfalse;
} //end of the function BotAirControl
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_WalkOffLedge(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t dir, hordir, end, v;
	float dist, speed;
	bot_moveresult_t_cleared( result );

	//
	VectorSubtract(reach->end, ms->origin, dir);
	BotCheckBlocked(ms, dir, qtrue, &result);
	//
	VectorSubtract(reach->end, ms->origin, v);
	v[2] = 0;
	dist = VectorNormalize(v);
	if (dist > 16) VectorMA(reach->end, 16, v, end);
	else VectorCopy(reach->end, end);
	//
	if (!BotAirControl(ms->origin, ms->velocity, end, hordir, &speed))
	{
		//go straight to the reachability end
		VectorCopy(dir, hordir);
		hordir[2] = 0;
		//
		speed = 400;
	} //end if
	//
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotFinishTravel_WalkOffLedge
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
/*
static bot_moveresult_t BotTravel_Jump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	float dist, gapdist, speed, horspeed, sv_jumpvel;
	bot_moveresult_t_cleared( result );

	//
	sv_jumpvel = botlibglobals.sv_jumpvel->value;
	//walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	speed = 350;
	//
	gapdist = BotGapDistance(ms, hordir, ms->entitynum);
	//if pretty close to the start focus on the reachability end
	if (dist < 50 || (gapdist && gapdist < 50))
	{
		//NOTE: using max speed (400) works best
		//if (AAS_HorizontalVelocityForJump(sv_jumpvel, ms->origin, reach->end, &horspeed))
		//{
		//	speed = horspeed * 400 / botlibglobals.sv_maxwalkvelocity->value;
		//} //end if
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		VectorNormalize(hordir);
		//elementary action jump
		EA_Jump(ms->client);
		//
		ms->jumpreach = ms->lastreachnum;
		speed = 600;
	} //end if
	else
	{
		if (AAS_HorizontalVelocityForJump(sv_jumpvel, reach->start, reach->end, &horspeed))
		{
			speed = horspeed * 400 / botlibglobals.sv_maxwalkvelocity->value;
		} //end if
	} //end else
	//elementary action
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_Jump*/
/*
static bot_moveresult_t BotTravel_Jump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir, dir1, dir2, mins, maxs, start, end;
	int gapdist;
	float dist1, dist2, speed;
	bot_moveresult_t_cleared( result );
	bsp_trace_t trace;

	//
	hordir[0] = reach->start[0] - reach->end[0];
	hordir[1] = reach->start[1] - reach->end[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	//
	VectorCopy(reach->start, start);
	start[2] += 1;
	//minus back the bouding box size plus 16
	VectorMA(reach->start, 80, hordir, end);
	//
	AAS_PresenceTypeBoundingBox(PRESENCE_NORMAL, mins, maxs);
	//check for solids
	trace = AAS_Trace(start, mins, maxs, end, ms->entitynum, MASK_PLAYERSOLID);
	if (trace.startsolid) VectorCopy(start, trace.endpos);
	//check for a gap
	for (gapdist = 0; gapdist < 80; gapdist += 10)
	{
		VectorMA(start, gapdist+10, hordir, end);
		end[2] += 1;
		if (AAS_PointAreaNum(end) != ms->reachareanum) break;
	} //end for
	if (gapdist < 80) VectorMA(reach->start, gapdist, hordir, trace.endpos);
//	dist1 = BotGapDistance(start, hordir, ms->entitynum);
//	if (dist1 && dist1 <= trace.fraction * 80) VectorMA(reach->start, dist1-20, hordir, trace.endpos);
	//
	VectorSubtract(ms->origin, reach->start, dir1);
	dir1[2] = 0;
	dist1 = VectorNormalize(dir1);
	VectorSubtract(ms->origin, trace.endpos, dir2);
	dir2[2] = 0;
	dist2 = VectorNormalize(dir2);
	//if just before the reachability start
	if (DotProduct(dir1, dir2) < -0.8 || dist2 < 5)
	{
		//botimport.Print(PRT_MESSAGE, "between jump start and run to point\n");
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//elementary action jump
		if (dist1 < 24) EA_Jump(ms->client);
		else if (dist1 < 32) EA_DelayedJump(ms->client);
		EA_Move(ms->client, hordir, 600);
		//
		ms->jumpreach = ms->lastreachnum;
	} //end if
	else
	{
		//botimport.Print(PRT_MESSAGE, "going towards run to point\n");
		hordir[0] = trace.endpos[0] - ms->origin[0];
		hordir[1] = trace.endpos[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//
		if (dist2 > 80) dist2 = 80;
		speed = 400 - (400 - 5 * dist2);
		EA_Move(ms->client, hordir, speed);
	} //end else
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_Jump*/
//*
static bot_moveresult_t BotTravel_Jump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir, dir1, dir2, start, end, runstart;
//	vec3_t runstart, dir1, dir2, hordir;
	int gapdist;
	float dist1, dist2, speed;
	bot_moveresult_t_cleared( result );

	//
	AAS_JumpReachRunStart(reach, runstart);
	//*
	hordir[0] = runstart[0] - reach->start[0];
	hordir[1] = runstart[1] - reach->start[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	//
	VectorCopy(reach->start, start);
	start[2] += 1;
	VectorMA(reach->start, 80, hordir, runstart);
	//check for a gap
	for (gapdist = 0; gapdist < 80; gapdist += 10)
	{
		VectorMA(start, gapdist+10, hordir, end);
		end[2] += 1;
		if (AAS_PointAreaNum(end) != ms->reachareanum) break;
	} //end for
	if (gapdist < 80) VectorMA(reach->start, gapdist, hordir, runstart);
	//
	VectorSubtract(ms->origin, reach->start, dir1);
	dir1[2] = 0;
	dist1 = VectorNormalize(dir1);
	VectorSubtract(ms->origin, runstart, dir2);
	dir2[2] = 0;
	dist2 = VectorNormalize(dir2);
	//if just before the reachability start
	if (DotProduct(dir1, dir2) < -0.8 || dist2 < 5)
	{
//		botimport.Print(PRT_MESSAGE, "between jump start and run start point\n");
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//elementary action jump
		if (dist1 < 24) EA_Jump(ms->client);
		else if (dist1 < 32) EA_DelayedJump(ms->client);
		EA_Move(ms->client, hordir, 600);
		//
		ms->jumpreach = ms->lastreachnum;
	} //end if
	else
	{
//		botimport.Print(PRT_MESSAGE, "going towards run start point\n");
		hordir[0] = runstart[0] - ms->origin[0];
		hordir[1] = runstart[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//
		if (dist2 > 80) dist2 = 80;
		speed = 400 - (400 - 5 * dist2);
		EA_Move(ms->client, hordir, speed);
	} //end else
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_Jump*/
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_Jump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir, hordir2;
	float speed, dist;
	bot_moveresult_t_cleared( result );

	//if not jumped yet
	if (!ms->jumpreach) return result;
	//go straight to the reachability end
	hordir[0] = reach->end[0] - ms->origin[0];
	hordir[1] = reach->end[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	hordir2[0] = reach->end[0] - reach->start[0];
	hordir2[1] = reach->end[1] - reach->start[1];
	hordir2[2] = 0;
	VectorNormalize(hordir2);
	//
	if (DotProduct(hordir, hordir2) < -0.5 && dist < 24) return result;
	//always use max speed when traveling through the air
	speed = 800;
	//
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotFinishTravel_Jump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_Ladder(bot_movestate_t *ms, aas_reachability_t *reach)
{
	//float dist, speed;
	vec3_t dir, viewdir;//, hordir;
	vec3_t origin = {0, 0, 0};
//	vec3_t up = {0, 0, 1};
	bot_moveresult_t_cleared( result );

	//
//	if ((ms->moveflags & MFL_AGAINSTLADDER))
		//NOTE: not a good idea for ladders starting in water
		// || !(ms->moveflags & MFL_ONGROUND))
	{
		//botimport.Print(PRT_MESSAGE, "against ladder or not on ground\n");
		VectorSubtract(reach->end, ms->origin, dir);
		VectorNormalize(dir);
		//set the ideal view angles, facing the ladder up or down
		viewdir[0] = dir[0];
		viewdir[1] = dir[1];
		viewdir[2] = 3 * dir[2];
		Vector2Angles(viewdir, result.ideal_viewangles);
		//elementary action
		EA_Move(ms->client, origin, 0);
		EA_MoveForward(ms->client);
		//set movement view flag so the AI can see the view is focussed
		result.flags |= MOVERESULT_MOVEMENTVIEW;
	} //end if
/*	else
	{
		//botimport.Print(PRT_MESSAGE, "moving towards ladder\n");
		VectorSubtract(reach->end, ms->origin, dir);
		//make sure the horizontal movement is large enough
		VectorCopy(dir, hordir);
		hordir[2] = 0;
		dist = VectorNormalize(hordir);
		//
		dir[0] = hordir[0];
		dir[1] = hordir[1];
		if (dir[2] > 0) dir[2] = 1;
		else dir[2] = -1;
		if (dist > 50) dist = 50;
		speed = 400 - (200 - 4 * dist);
		EA_Move(ms->client, dir, speed);
	} //end else*/
	//save the movement direction
	VectorCopy(dir, result.movedir);
	//
	return result;
} //end of the function BotTravel_Ladder
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_Teleport(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	float dist;
	bot_moveresult_t_cleared( result );

	//if the bot is being teleported
	if (ms->moveflags & MFL_TELEPORTED) return result;

	//walk straight to center of the teleporter
	VectorSubtract(reach->start, ms->origin, hordir);
	if (!(ms->moveflags & MFL_SWIMMING)) hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);

	if (dist < 30) EA_Move(ms->client, hordir, 200);
	else EA_Move(ms->client, hordir, 400);

	if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;

	VectorCopy(hordir, result.movedir);
	return result;
} //end of the function BotTravel_Teleport
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_Elevator(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t dir, dir1, dir2, hordir, bottomcenter;
	float dist, dist1, dist2, speed;
	bot_moveresult_t_cleared( result );

	//if standing on the plat
	if (BotOnMover(ms->origin, ms->entitynum, reach))
	{
#ifdef DEBUG_ELEVATOR
		botimport.Print(PRT_MESSAGE, "bot on elevator\n");
#endif //DEBUG_ELEVATOR
		//if vertically not too far from the end point
		if (fabs(ms->origin[2] - reach->end[2]) < sv_maxbarrier->value)
		{
#ifdef DEBUG_ELEVATOR
			botimport.Print(PRT_MESSAGE, "bot moving to end\n");
#endif //DEBUG_ELEVATOR
			//move to the end point
			VectorSubtract(reach->end, ms->origin, hordir);
			hordir[2] = 0;
			VectorNormalize(hordir);
			if (!BotCheckBarrierJump(ms, hordir, 100))
			{
				EA_Move(ms->client, hordir, 400);
			} //end if
			VectorCopy(hordir, result.movedir);
		} //end else
		//if not really close to the center of the elevator
		else
		{
			MoverBottomCenter(reach, bottomcenter);
			VectorSubtract(bottomcenter, ms->origin, hordir);
			hordir[2] = 0;
			dist = VectorNormalize(hordir);
			//
			if (dist > 10)
			{
#ifdef DEBUG_ELEVATOR
				botimport.Print(PRT_MESSAGE, "bot moving to center\n");
#endif //DEBUG_ELEVATOR
				//move to the center of the elevator
				if (dist > 100) dist = 100;
				speed = 400 - (400 - 4 * dist);
				//
				EA_Move(ms->client, hordir, speed);
				VectorCopy(hordir, result.movedir);
			} //end if
		} //end else
	} //end if
	else
	{
#ifdef DEBUG_ELEVATOR
		botimport.Print(PRT_MESSAGE, "bot not on elevator\n");
#endif //DEBUG_ELEVATOR
		//if very near the reachability end
		VectorSubtract(reach->end, ms->origin, dir);
		dist = VectorLength(dir);
		if (dist < 64)
		{
			if (dist > 60) dist = 60;
			speed = 360 - (360 - 6 * dist);
			//
			if ((ms->moveflags & MFL_SWIMMING) || !BotCheckBarrierJump(ms, dir, 50))
			{
				if (speed > 5) EA_Move(ms->client, dir, speed);
			} //end if
			VectorCopy(dir, result.movedir);
			//
			if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;
			//stop using this reachability
			ms->reachability_time = 0;
			return result;
		} //end if
		//get direction and distance to reachability start
		VectorSubtract(reach->start, ms->origin, dir1);
		if (!(ms->moveflags & MFL_SWIMMING)) dir1[2] = 0;
		dist1 = VectorNormalize(dir1);
		//if the elevator isn't down
		if (!MoverDown(reach))
		{
#ifdef DEBUG_ELEVATOR
			botimport.Print(PRT_MESSAGE, "elevator not down\n");
#endif //DEBUG_ELEVATOR
			dist = dist1;
			VectorCopy(dir1, dir);
			//
			BotCheckBlocked(ms, dir, qfalse, &result);
			//
			if (dist > 60) dist = 60;
			speed = 360 - (360 - 6 * dist);
			//
			if (!(ms->moveflags & MFL_SWIMMING) && !BotCheckBarrierJump(ms, dir, 50))
			{
				if (speed > 5) EA_Move(ms->client, dir, speed);
			} //end if
			VectorCopy(dir, result.movedir);
			//
			if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;
			//this isn't a failure... just wait till the elevator comes down
			result.type = RESULTTYPE_ELEVATORUP;
			result.flags |= MOVERESULT_WAITING;
			return result;
		} //end if
		//get direction and distance to elevator bottom center
		MoverBottomCenter(reach, bottomcenter);
		VectorSubtract(bottomcenter, ms->origin, dir2);
		if (!(ms->moveflags & MFL_SWIMMING)) dir2[2] = 0;
		dist2 = VectorNormalize(dir2);
		//if very close to the reachability start or
		//closer to the elevator center or
		//between reachability start and elevator center
		if (dist1 < 20 || dist2 < dist1 || DotProduct(dir1, dir2) < 0)
		{
#ifdef DEBUG_ELEVATOR
			botimport.Print(PRT_MESSAGE, "bot moving to center\n");
#endif //DEBUG_ELEVATOR
			dist = dist2;
			VectorCopy(dir2, dir);
		} //end if
		else //closer to the reachability start
		{
#ifdef DEBUG_ELEVATOR
			botimport.Print(PRT_MESSAGE, "bot moving to start\n");
#endif //DEBUG_ELEVATOR
			dist = dist1;
			VectorCopy(dir1, dir);
		} //end else
		//
		BotCheckBlocked(ms, dir, qfalse, &result);
		//
		if (dist > 60) dist = 60;
		speed = 400 - (400 - 6 * dist);
		//
		if (!(ms->moveflags & MFL_SWIMMING) && !BotCheckBarrierJump(ms, dir, 50))
		{
			EA_Move(ms->client, dir, speed);
		} //end if
		VectorCopy(dir, result.movedir);
		//
		if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;
	} //end else
	return result;
} //end of the function BotTravel_Elevator
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_Elevator(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t bottomcenter, bottomdir, topdir;
	bot_moveresult_t_cleared( result );

	//
	MoverBottomCenter(reach, bottomcenter);
	VectorSubtract(bottomcenter, ms->origin, bottomdir);
	//
	VectorSubtract(reach->end, ms->origin, topdir);
	//
	if (fabs(bottomdir[2]) < fabs(topdir[2]))
	{
		VectorNormalize(bottomdir);
		EA_Move(ms->client, bottomdir, 300);
	} //end if
	else
	{
		VectorNormalize(topdir);
		EA_Move(ms->client, topdir, 300);
	} //end else
	return result;
} //end of the function BotFinishTravel_Elevator
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static void BotFuncBobStartEnd(aas_reachability_t *reach, vec3_t start, vec3_t end, vec3_t origin)
{
	int spawnflags, modelnum;
	vec3_t mins, maxs, mid, angles = {0, 0, 0};
	int num0, num1;

	modelnum = reach->facenum & 0x0000FFFF;
	if (!AAS_OriginOfMoverWithModelNum(modelnum, origin))
	{
		botimport.Print(PRT_MESSAGE, "BotFuncBobStartEnd: no entity with model %d\n", modelnum);
		VectorSet(start, 0, 0, 0);
		VectorSet(end, 0, 0, 0);
		return;
	} //end if
	AAS_BSPModelMinsMaxsOrigin(modelnum, angles, mins, maxs, NULL);
	VectorAdd(mins, maxs, mid);
	VectorScale(mid, 0.5, mid);
	VectorCopy(mid, start);
	VectorCopy(mid, end);
	spawnflags = reach->facenum >> 16;
	num0 = reach->edgenum >> 16;
	if (num0 > 0x00007FFF) num0 |= 0xFFFF0000;
	num1 = reach->edgenum & 0x0000FFFF;
	if (num1 > 0x00007FFF) num1 |= 0xFFFF0000;
	if (spawnflags & 1)
	{
		start[0] = num0;
		end[0] = num1;
		//
		origin[0] += mid[0];
		origin[1] = mid[1];
		origin[2] = mid[2];
	} //end if
	else if (spawnflags & 2)
	{
		start[1] = num0;
		end[1] = num1;
		//
		origin[0] = mid[0];
		origin[1] += mid[1];
		origin[2] = mid[2];
	} //end else if
	else
	{
		start[2] = num0;
		end[2] = num1;
		//
		origin[0] = mid[0];
		origin[1] = mid[1];
		origin[2] += mid[2];
	} //end else
} //end of the function BotFuncBobStartEnd
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_FuncBobbing(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t dir, dir1, dir2, hordir, bottomcenter, bob_start, bob_end, bob_origin;
	float dist, dist1, dist2, speed;
	bot_moveresult_t_cleared( result );

	//
	BotFuncBobStartEnd(reach, bob_start, bob_end, bob_origin);
	//if standing ontop of the func_bobbing
	if (BotOnMover(ms->origin, ms->entitynum, reach))
	{
#ifdef DEBUG_FUNCBOB
		botimport.Print(PRT_MESSAGE, "bot on func_bobbing\n");
#endif
		//if near end point of reachability
		VectorSubtract(bob_origin, bob_end, dir);
		if (VectorLength(dir) < 24)
		{
#ifdef DEBUG_FUNCBOB
			botimport.Print(PRT_MESSAGE, "bot moving to reachability end\n");
#endif
			//move to the end point
			VectorSubtract(reach->end, ms->origin, hordir);
			hordir[2] = 0;
			VectorNormalize(hordir);
			if (!BotCheckBarrierJump(ms, hordir, 100))
			{
				EA_Move(ms->client, hordir, 400);
			} //end if
			VectorCopy(hordir, result.movedir);
		} //end else
		//if not really close to the center of the func_bobbing
		else
		{
			MoverBottomCenter(reach, bottomcenter);
			VectorSubtract(bottomcenter, ms->origin, hordir);
			hordir[2] = 0;
			dist = VectorNormalize(hordir);
			//
			if (dist > 10)
			{
#ifdef DEBUG_FUNCBOB
				botimport.Print(PRT_MESSAGE, "bot moving to func_bobbing center\n");
#endif
				//move to the center of the func_bobbing
				if (dist > 100) dist = 100;
				speed = 400 - (400 - 4 * dist);
				//
				EA_Move(ms->client, hordir, speed);
				VectorCopy(hordir, result.movedir);
			} //end if
		} //end else
	} //end if
	else
	{
#ifdef DEBUG_FUNCBOB
		botimport.Print(PRT_MESSAGE, "bot not ontop of func_bobbing\n");
#endif
		//if very near the reachability end
		VectorSubtract(reach->end, ms->origin, dir);
		dist = VectorLength(dir);
		if (dist < 64)
		{
#ifdef DEBUG_FUNCBOB
			botimport.Print(PRT_MESSAGE, "bot moving to end\n");
#endif
			if (dist > 60) dist = 60;
			speed = 360 - (360 - 6 * dist);
			//if swimming or no barrier jump
			if ((ms->moveflags & MFL_SWIMMING) || !BotCheckBarrierJump(ms, dir, 50))
			{
				if (speed > 5) EA_Move(ms->client, dir, speed);
			} //end if
			VectorCopy(dir, result.movedir);
			//
			if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;
			//stop using this reachability
			ms->reachability_time = 0;
			return result;
		} //end if
		//get direction and distance to reachability start
		VectorSubtract(reach->start, ms->origin, dir1);
		if (!(ms->moveflags & MFL_SWIMMING)) dir1[2] = 0;
		dist1 = VectorNormalize(dir1);
		//if func_bobbing is Not its start position
		VectorSubtract(bob_origin, bob_start, dir);
		if (VectorLength(dir) > 16)
		{
#ifdef DEBUG_FUNCBOB
			botimport.Print(PRT_MESSAGE, "func_bobbing not at start\n");
#endif
			dist = dist1;
			VectorCopy(dir1, dir);
			//
			BotCheckBlocked(ms, dir, qfalse, &result);
			//
			if (dist > 60) dist = 60;
			speed = 360 - (360 - 6 * dist);
			//
			if (!(ms->moveflags & MFL_SWIMMING) && !BotCheckBarrierJump(ms, dir, 50))
			{
				if (speed > 5) EA_Move(ms->client, dir, speed);
			} //end if
			VectorCopy(dir, result.movedir);
			//
			if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;
			//this isn't a failure... just wait till the func_bobbing arrives
			result.type = RESULTTYPE_WAITFORFUNCBOBBING;
			result.flags |= MOVERESULT_WAITING;
			return result;
		} //end if
		//get direction and distance to func_bobbing bottom center
		MoverBottomCenter(reach, bottomcenter);
		VectorSubtract(bottomcenter, ms->origin, dir2);
		if (!(ms->moveflags & MFL_SWIMMING)) dir2[2] = 0;
		dist2 = VectorNormalize(dir2);
		//if very close to the reachability start or
		//closer to the func_bobbing center or
		//between reachability start and func_bobbing center
		if (dist1 < 20 || dist2 < dist1 || DotProduct(dir1, dir2) < 0)
		{
#ifdef DEBUG_FUNCBOB
			botimport.Print(PRT_MESSAGE, "bot moving to func_bobbing center\n");
#endif
			dist = dist2;
			VectorCopy(dir2, dir);
		} //end if
		else //closer to the reachability start
		{
#ifdef DEBUG_FUNCBOB
			botimport.Print(PRT_MESSAGE, "bot moving to reachability start\n");
#endif
			dist = dist1;
			VectorCopy(dir1, dir);
		} //end else
		//
		BotCheckBlocked(ms, dir, qfalse, &result);
		//
		if (dist > 60) dist = 60;
		speed = 400 - (400 - 6 * dist);
		//
		if (!(ms->moveflags & MFL_SWIMMING) && !BotCheckBarrierJump(ms, dir, 50))
		{
			EA_Move(ms->client, dir, speed);
		} //end if
		VectorCopy(dir, result.movedir);
		//
		if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;
	} //end else
	return result;
} //end of the function BotTravel_FuncBobbing
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_FuncBobbing(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t bob_origin, bob_start, bob_end, dir, hordir, bottomcenter;
	bot_moveresult_t_cleared( result );
	float dist, speed;

	//
	BotFuncBobStartEnd(reach, bob_start, bob_end, bob_origin);
	//
	VectorSubtract(bob_origin, bob_end, dir);
	dist = VectorLength(dir);
	//if the func_bobbing is near the end
	if (dist < 16)
	{
		VectorSubtract(reach->end, ms->origin, hordir);
		if (!(ms->moveflags & MFL_SWIMMING)) hordir[2] = 0;
		dist = VectorNormalize(hordir);
		//
		if (dist > 60) dist = 60;
		speed = 360 - (360 - 6 * dist);
		//
		if (speed > 5) EA_Move(ms->client, dir, speed);
		VectorCopy(dir, result.movedir);
		//
		if (ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;
	} //end if
	else
	{
		MoverBottomCenter(reach, bottomcenter);
		VectorSubtract(bottomcenter, ms->origin, hordir);
		if (!(ms->moveflags & MFL_SWIMMING)) hordir[2] = 0;
		dist = VectorNormalize(hordir);
		//
		if (dist > 5)
		{
			//move to the center of the func_bobbing
			if (dist > 100) dist = 100;
			speed = 400 - (400 - 4 * dist);
			//
			EA_Move(ms->client, hordir, speed);
			VectorCopy(hordir, result.movedir);
		} //end if
	} //end else
	return result;
} //end of the function BotFinishTravel_FuncBobbing
//===========================================================================
// 0  no valid grapple hook visible
// 1  the grapple hook is still flying
// 2  the grapple hooked into a wall
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int GrappleState(bot_movestate_t *ms, aas_reachability_t *reach)
{
	//both bits are the game's own word, published every think: the pull
	//bit for an anchored hook, MFL_HOOKOUT for one that exists at all. A
	//game that publishes neither never reaches state 1, and its grapple
	//reaches end at the watchdog
	if (ms->moveflags & MFL_GRAPPLEPULL) return 2;
	if (ms->moveflags & MFL_HOOKOUT) return 1;
	return 0;
} //end of the function GrappleState
//===========================================================================
// Has a DESCENT ride-in's tether finished its work?
//
// PM_CrashLand returns without damage while PMF_GRAPPLE_PULL is set, so on the
// way down the tether's last remaining job is the landing itself. Once letting
// go would put the body on the deck under the fall-damage threshold, that job
// is done, and holding on only bleeds the body's momentum into the tow before
// giving it back on foot.
//
// The impact is the free fall from here: v^2 = vz^2 + 2*g*h, against the bar
// PM_CrashLand actually uses. Not a ceiling ride-in: its drop is measured from
// a SETTLED hang, so a release with speed still on it is not the drop that was
// published, and that one still rides to a stall.
//===========================================================================
static int BotGrappleDescentDone(bot_movestate_t *ms, aas_reachability_t *reach)
{
	aas_areainfo_t deck;
	float h, vz, g;

	if (reach->end[2] - ms->origin[2] >= AAS_GRAPPLE_CEIL_DZ) return qfalse;
	if (!AAS_AreaInfo(reach->areanum, &deck)) return qfalse;
	h = ms->origin[2] - deck.mins[2];
	if (h < 0) h = 0;
	vz = ms->velocity[2];
	g = sv_gravity->value;
	if (g <= 0) g = 800;		//sv_gravity's own default, above
	return (vz * vz + 2 * g * h
			<= (float) AAS_GRAPPLE_FALL_SAFE_SPEED * AAS_GRAPPLE_FALL_SAFE_SPEED)
			? qtrue : qfalse;
} //end of the function BotGrappleDescentDone
//===========================================================================
// Is the deck the reach promised under the body where it hangs? A ride-in
// lets go where it stalled, and the drop starts from under the anchor
// and a hang's width around it, but the body's stall is decided by the map
// rather than the window, and a lip can hold it over the void beside an
// overstated deck. This is a COMPARISON against the published destination:
// the point beneath the hang, at the deck's height, must lie in a grounded
// area at that height. It is never a prediction of the fall. Not arrived is
// a stall, and a stall is the router's to route around.
//===========================================================================
static int BotGrappleDeckBelow(bot_movestate_t *ms, aas_reachability_t *reach)
{
	aas_areainfo_t deck;
	aas_trace_t trace;
	vec3_t end;

	if (!AAS_AreaInfo(reach->areanum, &deck)) return qtrue;
	//an area is a convex leaf with SOME ground under it, not a floor under
	//all of it: a tall area over a deck's edge reaches out over the drop, and
	//a point inside it at deck height can sit over open air.
	//What the landing will do is what the body's own box meets straight down,
	//against the world PM will meet it in: trace that, and arrived means it
	//stops at the promised height
	//a body already standing at the deck's height, a floor anchor's arrival,
	//needs no trace, and a box resting on its floor traces as start-solid
	if (ms->origin[2] <= deck.mins[2] + 8) return qtrue;
	VectorCopy(ms->origin, end);
	end[2] = deck.mins[2] - 8;
	trace = AAS_TraceClientBBox(ms->origin, end, PRESENCE_NORMAL, -1);
	if (trace.startsolid) return qfalse;
	if (trace.fraction >= 1.0f) return qfalse;
	return (fabs(trace.endpos[2] - deck.mins[2]) <= 32) ? qtrue : qfalse;
} //end of the function BotGrappleDeckBelow
//===========================================================================
// A route tow is over: the reach it was fired for is no longer outstanding.
//===========================================================================
static void BotGrappleRouteEnd(bot_movestate_t *ms)
{
	ms->grapplefire_reachnum = 0;
} //end of the function BotGrappleRouteEnd
//===========================================================================
// Consulted by the server at the bot input syscall, once per server frame:
// thinks ARM the published release (BotTravel_Grapple banks grapplearm_dist),
// this EXECUTES it, the same think-decides/frame-acts shape the aim system
// uses, so the button-up lands within one server frame of tow travel of the
// published center at any bot_thinktime. Returns qtrue when the caller must
// clear BUTTON_ATTACK from this frame's usercmd; the state flip happens here,
// so the next think finds the fall already owned by the released-steer branch.
//===========================================================================
int BotGrappleArmedRelease(int client, struct playerState_s *ps)
{
	float dist, step;
	bot_movestate_t *ms;

	if (client < 0 || client >= MAX_CLIENTS) return qfalse;
	ms = botmovestate_byclient[client];
	if (!ms) return qfalse;
	//the first frame the tow pulls, the playerstate's own grapplePoint IS
	//the bite. The latch check lives here and nowhere else: botlib's entity
	//feed is updated on the botlib tick, and a bot thinking off the tick
	//sees its own hook where it WAS, still flying and short of the wall, and
	//after the bite the game retypes it ET_GRAPPLE, which no missile scan
	//finds. A comparison against the published anchor, never a prediction
	if (ms->grapplefire_reachnum && !ms->grapplefire_bitten
			&& (ms->moveflags & MFL_ACTIVEGRAPPLE)
			&& (ps->pm_flags & (int) pmf_grapplepull->value))
	{
		float miss = Distance(ps->grapplePoint, ms->grapplefire_anchor);
		ms->grapplefire_bitten = 1;
		//farther out than the published R and the window does not cover this
		//state: let go now, while it still reads as a hook that did not
		//stick. The button-up is this frame's; the think that follows
		//remembers the reach and ends it (grapplefire_invalid)
		if (ms->grapplefire_rtol > 0 && miss > ms->grapplefire_rtol)
		{
			BotGrappleRouteEnd(ms);
			ms->grapplefire_invalid = 1;
			//an offhand grapple releases by command: the think's job
			if ((int) offhandgrapple->value) return qfalse;
			ms->grapplearm_dist = -1;
			return qtrue;
		} //end if
		//a valid bite on a windowed reach ARMS the release here, from the
		//cargo banked at the shot: waiting for a think to arm it left a tow
		//whose thinks were busy elsewhere (a battle node) unarmed until it
		//was 240u past its window. Thinks re-arm harmlessly
		if (ms->grapplefire_center > 0 && !(int) offhandgrapple->value)
		{
			ms->grapplearm_dist = (float) ms->grapplefire_center;
			ms->grapplearm_hw = ms->grapplefire_hw;
			ms->grapplearm_rtol = ms->grapplefire_rtol;
		} //end if
	} //end if
	//the pull ended without botlib letting go: something else freed the
	//hook: the game's expressive layer taking the weapon for a maneuver,
	//a teleporter, a death. The route tow is over, so disarm, or the armed
	//window would judge whatever hook comes next
	if (ms->grapplefire_bitten && ms->grapplearm_dist >= 0
			&& (ms->moveflags & MFL_ACTIVEGRAPPLE)
			&& !(ps->pm_flags & (int) pmf_grapplepull->value))
	{
		BotGrappleRouteEnd(ms);
		ms->grapplearm_dist = 0;
		ms->grapplefire_invalid = 0;
		ms->moveflags &= ~MFL_ACTIVEGRAPPLE;
		ms->moveflags |= MFL_GRAPPLERESET;
		return qfalse;
	} //end if
	//-1 = released this think interval: the game resends the think's
	//lastucmd: attack still down, every frame until the next think
	//rebuilds input, and a single cleared frame would re-fire the hook
	//on the very next one. Keep suppressing; the released-steer think
	//resets this to 0.
	if (ms->grapplearm_dist < 0) return qtrue;
	if (ms->grapplearm_dist == 0) return qfalse;
	if (!(ms->moveflags & MFL_ACTIVEGRAPPLE)) return qfalse;
	//not pulling: the hook is still flying, or already gone, grapplePoint
	//would be stale and the comparison meaningless
	if (!(ps->pm_flags & (int) pmf_grapplepull->value)) return qfalse;
	dist = Distance(ps->origin, ps->grapplePoint);
	//the window is the whole published span, not its center: release on the
	//in-window frame nearest the center. The tow closes by a measured step
	//per frame: last frame's delta, falling back to a model frame before one
	//is measured. If the next frame would sit farther from the
	//center than this one, this is the frame
	step = (ms->grapplearm_lastdist > 0) ? ms->grapplearm_lastdist - dist : AAS_GRAPPLE_FRAME_STEP;
	if (step < 1) step = 1;
	ms->grapplearm_lastdist = dist;
	if (dist > ms->grapplearm_dist + ms->grapplearm_hw) return qfalse;
	if (dist > ms->grapplearm_dist
			&& fabs(dist - step - ms->grapplearm_dist) < fabs(dist - ms->grapplearm_dist))
		return qfalse;
	BotGrappleRouteEnd(ms);
	ms->grapplearm_dist = -1;
	ms->moveflags &= ~MFL_ACTIVEGRAPPLE;
	ms->moveflags |= MFL_GRAPPLERELEASED;
	return qtrue;
} //end of the function BotGrappleArmedRelease
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static void BotResetGrapple(bot_movestate_t *ms)
{
	aas_reachability_t reach;

	AAS_ReachabilityFromNum(ms->lastreachnum, &reach);
	//if not using the grapple hook reachability anymore
	if ((reach.traveltype & TRAVELTYPE_MASK) != TRAVEL_GRAPPLEHOOK)
	{
		if ((ms->moveflags & (MFL_ACTIVEGRAPPLE|MFL_GRAPPLERELEASED)) || ms->grapplevisible_time)
		{
			//the router moved on while a shot was still live: a reaction to
			//something outside the tow: a goal change, a timeout, a death
			BotGrappleRouteEnd(ms);
			if (offhandgrapple->value)
				EA_Command(ms->client, cmd_grappleoff->string);
			ms->grapplearm_dist = 0;
			ms->grapplefire_invalid = 0;
			ms->grapplespot_time = 0;
			ms->moveflags &= ~(MFL_ACTIVEGRAPPLE|MFL_GRAPPLERELEASED);
			ms->grapplevisible_time = 0;
#ifdef DEBUG_GRAPPLE
			botimport.Print(PRT_MESSAGE, "reset grapple\n");
#endif //DEBUG_GRAPPLE
		} //end if
	} //end if
} //end of the function BotResetGrapple
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_Grapple(bot_movestate_t *ms, aas_reachability_t *reach)
{
	bot_moveresult_t_cleared( result );
	float dist, speed, rtol, aimgate, aimerr;
	vec3_t dir, viewdir, org;
	int state, areanum, ridx, center, halfwidth, exact, aimexact, atspot;
	bsp_trace_t trace;

#ifdef DEBUG_GRAPPLE
	static int debugline;
	if (!debugline) debugline = botimport.DebugLineCreate();
	botimport.DebugLineShow(debugline, reach->start, reach->end, LINECOLOR_BLUE);
#endif //DEBUG_GRAPPLE

	//the reach's published cargo (aasfile.h): the anchor tolerance R it
	//carries, the precision contract, and the release window: the
	//conclusion, shipped, instead of fragments for a consumer to reassemble
	ridx = AAS_UNPACK_GRAPPLE_R_INDEX(reach->traveltype);
	rtol = (ridx >= 1 && ridx <= AAS_GRAPPLE_R_STEPS) ? aas_grapple_r_step[ridx - 1] : 0;
	exact = (reach->traveltype & TRAVELFLAG_GRAPPLEEXACT) != 0;
	center = AAS_GRAPPLE_WINDOW_CENTER(reach->edgenum);
	halfwidth = AAS_GRAPPLE_WINDOW_HALFWIDTH(reach->edgenum);
	//
	if (ms->moveflags & MFL_GRAPPLERESET)
	{
		if (offhandgrapple->value)
			EA_Command(ms->client, cmd_grappleoff->string);
		ms->grapplearm_dist = 0;
		ms->grapplefire_invalid = 0;
		ms->moveflags &= ~(MFL_ACTIVEGRAPPLE|MFL_GRAPPLERELEASED);
		return result;
	} //end if
	//the frame hook let go of a hook that bit outside the published R: remember
	//the reach so the router re-routes instead of retrying, and end it
	if (ms->grapplefire_invalid)
	{
		ms->grapplefire_invalid = 0;
		BotAddToAvoidReach(ms, ms->lastreachnum, AVOIDREACH_TIME);
		if (offhandgrapple->value)
			EA_Command(ms->client, cmd_grappleoff->string);
		ms->grapplearm_dist = 0;
		ms->moveflags &= ~(MFL_ACTIVEGRAPPLE|MFL_GRAPPLERELEASED);
		ms->moveflags |= MFL_GRAPPLERESET;
		ms->reachability_time = 0;	//end the reachability
		return result;
	} //end if
	//the fall after a windowed release: push along the stored face, the
	//horizontal negation of the anchor face's normal, the one direction both
	//any reader derives from the stored reach alone (facenum is
	//a real remapped index precisely so this line can read it)
	if (ms->moveflags & MFL_GRAPPLERELEASED)
	{
		//this think rebuilds input without attack held: the frame hook's
		//post-release suppression (grapplearm_dist == -1) can stand down
		ms->grapplearm_dist = 0;
		if (ms->moveflags & MFL_ONGROUND)
		{
			ms->moveflags &= ~MFL_GRAPPLERELEASED;
			ms->moveflags |= MFL_GRAPPLERESET;
			ms->reachability_time = 0;	//end the reachability
			return result;
		} //end if
		{
			float facedist;
			AAS_FacePlane(abs(reach->facenum), dir, &facedist);
		}
		VectorNegate(dir, dir);
		dir[2] = 0;
		if (VectorNormalize(dir) < 0.1f)
		{
			//floor or ceiling anchor has no horizontal normal: continue along
			//the tow's own horizontal, the one heading the reach itself carries
			VectorSubtract(reach->end, reach->start, dir);
			dir[2] = 0;
			VectorNormalize(dir);
		} //end if
		EA_Move(ms->client, dir, 400);
		Vector2Angles(dir, result.ideal_viewangles);
		result.flags |= MOVERESULT_MOVEMENTVIEW;
		VectorCopy(dir, result.movedir);
		return result;
	} //end if
	//
	if (!(int) offhandgrapple->value)
	{
		result.weapon = weapindex_grapple->value;
		result.flags |= MOVERESULT_MOVEMENTWEAPON;
	} //end if
	//
	if (ms->moveflags & MFL_ACTIVEGRAPPLE)
	{
		float dist3;
#ifdef DEBUG_GRAPPLE
		botimport.Print(PRT_MESSAGE, "BotTravel_Grapple: active grapple\n");
#endif //DEBUG_GRAPPLE
		//
		state = GrappleState(ms, reach);
		//
		VectorSubtract(reach->end, ms->origin, dir);
		dist3 = VectorLength(dir);
		dir[2] = 0;
		dist = VectorLength(dir);
		//mid-tow the view is the mechanism: PM_GrappleMove tows toward the
		//anchor inset back along the VIEW, so a view that wanders moves the tow
		//target with it and the body arrives beside the anchor with a sideways
		//pull still on it. Hold it along the TETHER, and from the origin rather
		//than the eye: under a ceiling the eye sits level with the anchor, a
		//view from it goes horizontal, and the target slides toward the approach
		//side and off the edge of the deck. The release branch takes the view
		//over after the let-go
		VectorSubtract(reach->end, ms->origin, viewdir);
		Vector2Angles(viewdir, result.ideal_viewangles);
		result.flags |= MOVERESULT_MOVEMENTVIEW;
		//the published release is armed here and executed by
		//BotGrappleArmedRelease at the input syscall.
		//Never arm under an OFFHAND grapple: its release is a console
		//command, not a button, so the frame hook's cleared BUTTON_ATTACK
		//would release nothing and the armed tow would ride into the stall
		//watchdog. Trinity runs the weapon grapple; offhand keeps the
		//legacy ending whole.
		if (center > 0 && (ms->moveflags & MFL_GRAPPLEPULL)
				&& !(int) offhandgrapple->value)
		{
			ms->grapplearm_dist = (float) center;
			ms->grapplearm_hw = halfwidth;
			ms->grapplearm_rtol = (int) rtol;
		} //end if
		//the ride-in ending, kept ONLY for reaches with no published window
		//(ceiling/floor/drop): ride to the anchor and let go once ARRIVED,
		//stalled within a body's reach of it in 3D. A stall farther out is a
		//blocked tow (a lip the body cannot get past), and that is a STALL the
		//router must route around, not an arrival
		if ((center == 0 || (int) offhandgrapple->value) && state == 2
				&& dist3 <= AAS_GRAPPLE_ARRIVE
				&& (dist <= AAS_GRAPPLE_ARRIVE_CEIL_H || reach->end[2] - ms->origin[2] < AAS_GRAPPLE_CEIL_DZ)
				//a descent is done when the fall is safe; everything else has to
				//come to rest first, because that is the ending it is published for
				&& (BotGrappleDescentDone(ms, reach)
					|| (ms->lastgrappledist3 - dist3 < 1
						&& sqrt(ms->velocity[0] * ms->velocity[0] + ms->velocity[1] * ms->velocity[1]) <= AAS_GRAPPLE_ARRIVE_SPEED))
				&& BotGrappleDeckBelow(ms, reach))
		{
			BotGrappleRouteEnd(ms);
#ifdef DEBUG_GRAPPLE
			botimport.Print(PRT_ERROR, "grapple normal end\n");
#endif //DEBUG_GRAPPLE
			if (offhandgrapple->value)
				EA_Command(ms->client, cmd_grappleoff->string);
			ms->grapplearm_dist = 0;
			ms->moveflags &= ~MFL_ACTIVEGRAPPLE;
			ms->moveflags |= MFL_GRAPPLERESET;
			ms->reachability_time = 0;	//end the reachability
			return result;
		} //end if
		else if (!state || (state == 2 && dist3 > ms->lastgrappledist3 - 2))
		{
			if (ms->grapplevisible_time < AAS_Time() - 0.4)
			{
				BotGrappleRouteEnd(ms);
				BotAddToAvoidReach(ms, ms->lastreachnum, AVOIDREACH_TIME);
#ifdef DEBUG_GRAPPLE
				botimport.Print(PRT_ERROR, "grapple not visible\n");
#endif //DEBUG_GRAPPLE
				if (offhandgrapple->value)
					EA_Command(ms->client, cmd_grappleoff->string);
				ms->grapplearm_dist = 0;
				ms->moveflags &= ~MFL_ACTIVEGRAPPLE;
				ms->moveflags |= MFL_GRAPPLERESET;
				ms->reachability_time = 0;	//end the reachability
				return result;
			} //end if
		} //end if
		else
		{
			ms->grapplevisible_time = AAS_Time();
		} //end else
		//
		if (!(int) offhandgrapple->value)
		{
			EA_Attack(ms->client);
		} //end if
		//remember the current grapple distances
		ms->lastgrappledist = dist;
		ms->lastgrappledist3 = dist3;
	} //end if
	else
	{
#ifdef DEBUG_GRAPPLE
		botimport.Print(PRT_MESSAGE, "BotTravel_Grapple: inactive grapple\n");
#endif //DEBUG_GRAPPLE
		//
		ms->grapplevisible_time = AAS_Time();
		//
		VectorSubtract(reach->start, ms->origin, dir);
		if (!(ms->moveflags & MFL_SWIMMING)) dir[2] = 0;
		VectorAdd(ms->origin, ms->viewoffset, org);
		VectorSubtract(reach->end, org, viewdir);
		//
		dist = VectorNormalize(dir);
		Vector2Angles(viewdir, result.ideal_viewangles);
		result.flags |= MOVERESULT_MOVEMENTVIEW;
		//
		//the aim gate is derived per reach, not flat: at anchor distance D an
		//aim error of a puts the hook ~D*tan(a) off the anchor, and R
		//covers no more than that, so the gate is atan(R/D) capped at 2
		//degrees. A flat 2 is enough at wide R and too loose on long-tow,
		//low-R reaches. Unpublished R keeps the flat gate
		aimgate = 2.0f;
		if (rtol > 0)
		{
			aimgate = (float) atan2(rtol, Distance(org, reach->end)) * (180.0f / M_PI);
			if (aimgate > 2.0f) aimgate = 2.0f;
		} //end if
		//a game that sets the grapple view to the ideal exactly (MFL_GRAPPLEAIM)
		//leaves no aim error for a gate to cover: the shot fires on position
		//and readiness alone, and one frame of motion is all that
		//remains. Games without the flag keep the gate
		aimexact = (ms->moveflags & MFL_GRAPPLEAIM) != 0;
		//EXACT reaches carry a contract taken within 16u of the mark and fire only
		//there; everything else fires from anywhere in the 48u approach,
		//inside the published 64u envelope. A correct bite ends at the anchor
		//wherever it was fired from, and the arrival rule already turns a
		//lip-caught hang into a stall
		aimerr = fabs(AngleDiff(result.ideal_viewangles[0], ms->viewangles[0]));
		if (fabs(AngleDiff(result.ideal_viewangles[1], ms->viewangles[1])) > aimerr)
			aimerr = fabs(AngleDiff(result.ideal_viewangles[1], ms->viewangles[1]));
		//and the hook must be in hand, raised, and not out (MFL_HOOKREADY, the
		//game's word): a press before that fires what IS in hand, or launches
		//the hook a weapon change later on a drifted view. An offhand grapple
		//is a command and needs no readiness
		atspot = dist < (exact ? AAS_GRAPPLE_FIRE_RADIUS_EXACT : AAS_GRAPPLE_FIRE_RADIUS)
			&& ((int) offhandgrapple->value || (ms->moveflags & MFL_HOOKREADY));
		if (atspot) { if (!ms->grapplespot_time) ms->grapplespot_time = AAS_Time(); }
		else ms->grapplespot_time = 0;
		//the game slews the view like a player and snaps the residual on the
		//firing command, so the gate only bounds the visible flick: the
		//derived gate, or 0.75 s at the mark for a turn that will not settle
		if (atspot
			&& (aimerr < aimgate
				|| (aimexact && AAS_Time() - ms->grapplespot_time >= 0.75f)))
		{
#ifdef DEBUG_GRAPPLE
			botimport.Print(PRT_MESSAGE, "BotTravel_Grapple: activating grapple\n");
#endif //DEBUG_GRAPPLE
			//check if the grapple missile path is clear
			VectorAdd(ms->origin, ms->viewoffset, org);
			trace = AAS_Trace(org, NULL, NULL, reach->end, ms->entitynum, CONTENTS_SOLID);
			VectorSubtract(reach->end, trace.endpos, dir);
			if (VectorLength(dir) > 16)
			{
				result.failure = qtrue;
				return result;
			} //end if
			//activate the grapple
			if (offhandgrapple->value)
			{
				EA_Command(ms->client, cmd_grappleon->string);
			} //end if
			else
			{
				EA_Attack(ms->client);
			} //end else
			ms->moveflags |= MFL_ACTIVEGRAPPLE;
			ms->lastgrappledist = 999999;
			ms->lastgrappledist3 = 999999;
			//bank the shot's cargo, so the frame hook can judge the bite and
			//the release against the reach this hook was fired for whatever
			//the router does in between
			ms->grapplefire_reachnum = ms->lastreachnum;
			VectorCopy(reach->end, ms->grapplefire_anchor);
			ms->grapplefire_bitten = 0;
			ms->grapplefire_center = center;
			ms->grapplefire_hw = halfwidth;
			ms->grapplefire_rtol = (int) rtol;
			ms->grapplefire_invalid = 0;
			ms->grapplearm_lastdist = 0;
			ms->grapplespot_time = 0;
		} //end if
		else
		{
			if (dist < 70) speed = 300 - (300 - 4 * dist);
			else speed = 400;
			//
			BotCheckBlocked(ms, dir, qtrue, &result);
			//elementary action move in direction
			EA_Move(ms->client, dir, speed);
			VectorCopy(dir, result.movedir);
		} //end else
		//if in another area before actually grappling
		areanum = AAS_PointAreaNum(ms->origin);
		if (areanum && areanum != ms->reachareanum) ms->reachability_time = 0;
	} //end else
	return result;
} //end of the function BotTravel_Grapple
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:			-
//===========================================================================
static bot_moveresult_t BotTravel_RocketJump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	float dist, speed;
	bot_moveresult_t_cleared( result );

	//botimport.Print(PRT_MESSAGE, "BotTravel_RocketJump: bah\n");
	//
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	//
	dist = VectorNormalize(hordir);
	//look in the movement direction
	Vector2Angles(hordir, result.ideal_viewangles);
	//look straight down
	result.ideal_viewangles[PITCH] = 90;
	//
	if (dist < 5 &&
			fabs(AngleDiff(result.ideal_viewangles[0], ms->viewangles[0])) < 5 &&
			fabs(AngleDiff(result.ideal_viewangles[1], ms->viewangles[1])) < 5)
	{
		//botimport.Print(PRT_MESSAGE, "between jump start and run start point\n");
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//elementary action jump
		EA_Jump(ms->client);
		EA_Attack(ms->client);
		EA_Move(ms->client, hordir, 800);
		//
		ms->jumpreach = ms->lastreachnum;
	} //end if
	else
	{
		if (dist > 80) dist = 80;
		speed = 400 - (400 - 5 * dist);
		EA_Move(ms->client, hordir, speed);
	} //end else
	//look in the movement direction
	Vector2Angles(hordir, result.ideal_viewangles);
	//look straight down
	result.ideal_viewangles[PITCH] = 90;
	//set the view angles directly
	EA_View(ms->client, result.ideal_viewangles);
	//view is important for the movement
	result.flags |= MOVERESULT_MOVEMENTVIEWSET;
	//select the rocket launcher
	EA_SelectWeapon(ms->client, (int) weapindex_rocketlauncher->value);
	//weapon is used for movement
	result.weapon = (int) weapindex_rocketlauncher->value;
	result.flags |= MOVERESULT_MOVEMENTWEAPON;
	//
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_RocketJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_BFGJump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	float dist, speed;
	bot_moveresult_t_cleared( result );

	//botimport.Print(PRT_MESSAGE, "BotTravel_BFGJump: bah\n");
	//
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	//
	dist = VectorNormalize(hordir);
	//
	if (dist < 5 &&
			fabs(AngleDiff(result.ideal_viewangles[0], ms->viewangles[0])) < 5 &&
			fabs(AngleDiff(result.ideal_viewangles[1], ms->viewangles[1])) < 5)
	{
		//botimport.Print(PRT_MESSAGE, "between jump start and run start point\n");
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//elementary action jump
		EA_Jump(ms->client);
		EA_Attack(ms->client);
		EA_Move(ms->client, hordir, 800);
		//
		ms->jumpreach = ms->lastreachnum;
	} //end if
	else
	{
		if (dist > 80) dist = 80;
		speed = 400 - (400 - 5 * dist);
		EA_Move(ms->client, hordir, speed);
	} //end else
	//look in the movement direction
	Vector2Angles(hordir, result.ideal_viewangles);
	//look straight down
	result.ideal_viewangles[PITCH] = 90;
	//set the view angles directly
	EA_View(ms->client, result.ideal_viewangles);
	//view is important for the movement
	result.flags |= MOVERESULT_MOVEMENTVIEWSET;
	//select the rocket launcher
	EA_SelectWeapon(ms->client, (int) weapindex_bfg10k->value);
	//weapon is used for movement
	result.weapon = (int) weapindex_bfg10k->value;
	result.flags |= MOVERESULT_MOVEMENTWEAPON;
	//
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_BFGJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_WeaponJump(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	float speed;
	bot_moveresult_t_cleared( result );

	//if not jumped yet
	if (!ms->jumpreach) return result;
	/*
	//go straight to the reachability end
	hordir[0] = reach->end[0] - ms->origin[0];
	hordir[1] = reach->end[1] - ms->origin[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	//always use max speed when traveling through the air
	EA_Move(ms->client, hordir, 800);
	VectorCopy(hordir, result.movedir);
	*/
	//
	if (!BotAirControl(ms->origin, ms->velocity, reach->end, hordir, &speed))
	{
		//go straight to the reachability end
		VectorSubtract(reach->end, ms->origin, hordir);
		hordir[2] = 0;
		VectorNormalize(hordir);
		speed = 400;
	} //end if
	//
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotFinishTravel_WeaponJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotTravel_JumpPad(bot_movestate_t *ms, aas_reachability_t *reach)
{
	vec3_t hordir;
	bot_moveresult_t_cleared( result );

	//first walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	//elementary action move in direction
	EA_Move(ms->client, hordir, 400);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotTravel_JumpPad
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotFinishTravel_JumpPad(bot_movestate_t *ms, aas_reachability_t *reach)
{
	float speed;
	vec3_t hordir;
	bot_moveresult_t_cleared( result );

	if (!BotAirControl(ms->origin, ms->velocity, reach->end, hordir, &speed))
	{
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		speed = 400;
	} //end if
	BotCheckBlocked(ms, hordir, qtrue, &result);
	//elementary action move in direction
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
} //end of the function BotFinishTravel_JumpPad
//===========================================================================
// time before the reachability times out
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int BotReachabilityTime(aas_reachability_t *reach)
{
	switch(reach->traveltype & TRAVELTYPE_MASK)
	{
		case TRAVEL_WALK: return 5;
		case TRAVEL_CROUCH: return 5;
		case TRAVEL_BARRIERJUMP: return 5;
		case TRAVEL_LADDER: return 6;
		case TRAVEL_WALKOFFLEDGE: return 5;
		case TRAVEL_JUMP: return 5;
		case TRAVEL_SWIM: return 5;
		case TRAVEL_WATERJUMP: return 5;
		case TRAVEL_TELEPORT: return 5;
		case TRAVEL_ELEVATOR: return 10;
		case TRAVEL_GRAPPLEHOOK: return 8;
		case TRAVEL_ROCKETJUMP: return 6;
		case TRAVEL_BFGJUMP: return 6;
		case TRAVEL_JUMPPAD: return 10;
		case TRAVEL_FUNCBOB: return 10;
		default:
		{
			botimport.Print(PRT_ERROR, "travel type %d not implemented yet\n", reach->traveltype);
			return 8;
		} //end case
	} //end switch
} //end of the function BotReachabilityTime
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static bot_moveresult_t BotMoveInGoalArea(bot_movestate_t *ms, bot_goal_t *goal)
{
	bot_moveresult_t_cleared( result );
	vec3_t dir;
	float dist, speed;

#ifdef DEBUG
	//botimport.Print(PRT_MESSAGE, "%s: moving straight to goal\n", ClientName(ms->entitynum-1));
	//AAS_ClearShownDebugLines();
	//AAS_DebugLine(ms->origin, goal->origin, LINECOLOR_RED);
#endif //DEBUG
	//walk straight to the goal origin
	dir[0] = goal->origin[0] - ms->origin[0];
	dir[1] = goal->origin[1] - ms->origin[1];
	if (ms->moveflags & MFL_SWIMMING)
	{
		dir[2] = goal->origin[2] - ms->origin[2];
		result.traveltype = TRAVEL_SWIM;
	} //end if
	else
	{
		dir[2] = 0;
		result.traveltype = TRAVEL_WALK;
	} //endif
	//
	dist = VectorNormalize(dir);
	if (dist > 100) dist = 100;
	speed = 400 - (400 - 4 * dist);
	if (speed < 10) speed = 0;
	//
	BotCheckBlocked(ms, dir, qtrue, &result);
	//elementary action move in direction
	EA_Move(ms->client, dir, speed);
	VectorCopy(dir, result.movedir);
	//
	if (ms->moveflags & MFL_SWIMMING)
	{
		Vector2Angles(dir, result.ideal_viewangles);
		result.flags |= MOVERESULT_SWIMVIEW;
	} //end if
	//if (!debugline) debugline = botimport.DebugLineCreate();
	//botimport.DebugLineShow(debugline, ms->origin, goal->origin, LINECOLOR_BLUE);
	//
	ms->lastreachnum = 0;
	ms->lastareanum = 0;
	ms->lastgoalareanum = goal->areanum;
	VectorCopy(ms->origin, ms->lastorigin);
	//
	return result;
} //end of the function BotMoveInGoalArea
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void BotMoveToGoal(bot_moveresult_t *result, int movestate, bot_goal_t *goal, int travelflags)
{
	int reachnum, lastreachnum, foundjumppad, ent, resultflags;
	aas_reachability_t reach, lastreach;
	bot_movestate_t *ms;
	//vec3_t mins, maxs, up = {0, 0, 1};
	//bsp_trace_t trace;
	//static int debugline;

	result->failure = qfalse;
	result->type = 0;
	result->blocked = qfalse;
	result->blockentity = 0;
	result->traveltype = 0;
	result->flags = 0;

	//
	ms = BotMoveStateFromHandle(movestate);
	if (!ms) return;
	//reset the grapple before testing if the bot has a valid goal
	//because the bot could lose all its goals when stuck to a wall
	BotResetGrapple(ms);
	//
	if (!goal)
	{
#ifdef DEBUG
		botimport.Print(PRT_MESSAGE, "client %d: movetogoal -> no goal\n", ms->client);
#endif //DEBUG
		result->failure = qtrue;
		return;
	} //end if
	//botimport.Print(PRT_MESSAGE, "numavoidreach = %d\n", ms->numavoidreach);
	//remove some of the move flags
	ms->moveflags &= ~(MFL_SWIMMING|MFL_AGAINSTLADDER);
	//set some of the move flags
	//NOTE: the MFL_ONGROUND flag is also set in the higher AI
	if (AAS_OnGround(ms->origin, ms->presencetype, ms->entitynum)) ms->moveflags |= MFL_ONGROUND;
	//
	if (ms->moveflags & MFL_ONGROUND)
	{
		int modeltype, modelnum;

		ent = BotOnTopOfEntity(ms);

		if (ent != -1)
		{
			modelnum = AAS_EntityModelindex(ent);
			if (modelnum >= 0 && modelnum < MAX_MODELS)
			{
				modeltype = modeltypes[modelnum];

				if (modeltype == MODELTYPE_FUNC_PLAT)
				{
					AAS_ReachabilityFromNum(ms->lastreachnum, &reach);
					//if the bot is Not using the elevator
					if ((reach.traveltype & TRAVELTYPE_MASK) != TRAVEL_ELEVATOR ||
						//NOTE: the face number is the plat model number
						(reach.facenum & 0x0000FFFF) != modelnum)
					{
						reachnum = AAS_NextModelReachability(0, modelnum);
						if (reachnum)
						{
							//botimport.Print(PRT_MESSAGE, "client %d: accidentally ended up on func_plat\n", ms->client);
							AAS_ReachabilityFromNum(reachnum, &reach);
							ms->lastreachnum = reachnum;
							ms->reachability_time = AAS_Time() + BotReachabilityTime(&reach);
						} //end if
						else
						{
							if (botDeveloper)
							{
								botimport.Print(PRT_MESSAGE, "client %d: on func_plat without reachability\n", ms->client);
							} //end if
							result->blocked = qtrue;
							result->blockentity = ent;
							result->flags |= MOVERESULT_ONTOPOFOBSTACLE;
							return;
						} //end else
					} //end if
					result->flags |= MOVERESULT_ONTOPOF_ELEVATOR;
				} //end if
				else if (modeltype == MODELTYPE_FUNC_BOB)
				{
					AAS_ReachabilityFromNum(ms->lastreachnum, &reach);
					//if the bot is Not using the func bobbing
					if ((reach.traveltype & TRAVELTYPE_MASK) != TRAVEL_FUNCBOB ||
						//NOTE: the face number is the func_bobbing model number
						(reach.facenum & 0x0000FFFF) != modelnum)
					{
						reachnum = AAS_NextModelReachability(0, modelnum);
						if (reachnum)
						{
							//botimport.Print(PRT_MESSAGE, "client %d: accidentally ended up on func_bobbing\n", ms->client);
							AAS_ReachabilityFromNum(reachnum, &reach);
							ms->lastreachnum = reachnum;
							ms->reachability_time = AAS_Time() + BotReachabilityTime(&reach);
						} //end if
						else
						{
							if (botDeveloper)
							{
								botimport.Print(PRT_MESSAGE, "client %d: on func_bobbing without reachability\n", ms->client);
							} //end if
							result->blocked = qtrue;
							result->blockentity = ent;
							result->flags |= MOVERESULT_ONTOPOFOBSTACLE;
							return;
						} //end else
					} //end if
					result->flags |= MOVERESULT_ONTOPOF_FUNCBOB;
				} //end if
				else if (modeltype == MODELTYPE_FUNC_STATIC || modeltype == MODELTYPE_FUNC_DOOR)
				{
					// check if ontop of a door bridge ?
					ms->areanum = BotFuzzyPointReachabilityArea(ms->origin);
					// if not in a reachability area
					if (!AAS_AreaReachability(ms->areanum))
					{
						result->blocked = qtrue;
						result->blockentity = ent;
						result->flags |= MOVERESULT_ONTOPOFOBSTACLE;
						return;
					} //end if
				} //end else if
				else
				{
					result->blocked = qtrue;
					result->blockentity = ent;
					result->flags |= MOVERESULT_ONTOPOFOBSTACLE;
					return;
				} //end else
			} //end if
		} //end if
	} //end if
	//if swimming
	if (AAS_Swimming(ms->origin)) ms->moveflags |= MFL_SWIMMING;
	//if against a ladder
	if (AAS_AgainstLadder(ms->origin)) ms->moveflags |= MFL_AGAINSTLADDER;
	//if the bot is on the ground, swimming or against a ladder
	if (ms->moveflags & (MFL_ONGROUND|MFL_SWIMMING|MFL_AGAINSTLADDER))
	{
		//botimport.Print(PRT_MESSAGE, "%s: onground, swimming or against ladder\n", ClientName(ms->entitynum-1));
		//
		AAS_ReachabilityFromNum(ms->lastreachnum, &lastreach);
		//reachability area the bot is in
		//ms->areanum = BotReachabilityArea(ms->origin, ((lastreach.traveltype & TRAVELTYPE_MASK) != TRAVEL_ELEVATOR));
		ms->areanum = BotFuzzyPointReachabilityArea(ms->origin);
		//
		if ( !ms->areanum )
		{
			result->failure = qtrue;
			result->blocked = qtrue;
			result->blockentity = 0;
			result->type = RESULTTYPE_INSOLIDAREA;
			return;
		} //end if
		//if the bot is in the goal area
		if (ms->areanum == goal->areanum)
		{
			*result = BotMoveInGoalArea(ms, goal);
			return;
		} //end if
		//assume we can use the reachability from the last frame
		reachnum = ms->lastreachnum;
		//if there is a last reachability
		if (reachnum)
		{
			AAS_ReachabilityFromNum(reachnum, &reach);
			//check if the reachability is still valid
			if (!(AAS_TravelFlagForType(reach.traveltype) & travelflags))
			{
				reachnum = 0;
			} //end if
			//special grapple hook case
			else if ((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_GRAPPLEHOOK)
			{
				if (ms->reachability_time < AAS_Time() ||
					(ms->moveflags & MFL_GRAPPLERESET))
				{
					reachnum = 0;
				} //end if
			} //end if
			//special elevator case
			else if ((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_ELEVATOR ||
				(reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_FUNCBOB)
			{
				if ((result->flags & MOVERESULT_ONTOPOF_ELEVATOR) ||
					(result->flags & MOVERESULT_ONTOPOF_FUNCBOB))
				{
					ms->reachability_time = AAS_Time() + 5;
				} //end if
				//if the bot was going for an elevator and reached the reachability area
				if (ms->areanum == reach.areanum ||
					ms->reachability_time < AAS_Time())
				{
					reachnum = 0;
				} //end if
			} //end if
			else
			{
#ifdef DEBUG
				if (botDeveloper)
				{
					if (ms->reachability_time < AAS_Time())
					{
						botimport.Print(PRT_MESSAGE, "client %d: reachability timeout in ", ms->client);
						AAS_PrintTravelType(reach.traveltype & TRAVELTYPE_MASK);
						botimport.Print(PRT_MESSAGE, "\n");
					} //end if
					/*
					if (ms->lastareanum != ms->areanum)
					{
						botimport.Print(PRT_MESSAGE, "changed from area %d to %d\n", ms->lastareanum, ms->areanum);
					} //end if*/
				} //end if
#endif //DEBUG
				//if the goal area changed or the reachability timed out
				//or the area changed
				if (ms->lastgoalareanum != goal->areanum ||
						ms->reachability_time < AAS_Time() ||
						ms->lastareanum != ms->areanum)
				{
					reachnum = 0;
					//botimport.Print(PRT_MESSAGE, "area change or timeout\n");
				} //end else if
			} //end else
		} //end if
		resultflags = 0;
		//if the bot needs a new reachability
		if (!reachnum)
		{
			//if the area has no reachability links
			if (!AAS_AreaReachability(ms->areanum))
			{
#ifdef DEBUG
				if (botDeveloper)
				{
					botimport.Print(PRT_MESSAGE, "area %d no reachability\n", ms->areanum);
				} //end if
#endif //DEBUG
			} //end if
			//get a new reachability leading towards the goal
			reachnum = BotGetReachabilityToGoal(ms->origin, ms->areanum,
								ms->lastgoalareanum, ms->lastareanum,
											ms->avoidreach, ms->avoidreachtimes, ms->avoidreachtries,
														goal, travelflags,
																ms->avoidspots, ms->numavoidspots, &resultflags);
			//the area number the reachability starts in
			ms->reachareanum = ms->areanum;
			//reset some state variables
			ms->jumpreach = 0;						//for TRAVEL_JUMP
			ms->moveflags &= ~MFL_GRAPPLERESET;	//for TRAVEL_GRAPPLEHOOK
			//if there is a reachability to the goal
			if (reachnum)
			{
				AAS_ReachabilityFromNum(reachnum, &reach);
				//set a timeout for this reachability
				ms->reachability_time = AAS_Time() + BotReachabilityTime(&reach);
				//
#ifdef AVOIDREACH
				//add the reachability to the reachabilities to avoid for a while
				BotAddToAvoidReach(ms, reachnum, AVOIDREACH_TIME);
#endif //AVOIDREACH
			} //end if
#ifdef DEBUG
			
			else if (botDeveloper)
			{
				botimport.Print(PRT_MESSAGE, "goal not reachable\n");
				Com_Memset(&reach, 0, sizeof(aas_reachability_t)); //make compiler happy
			} //end else
			if (botDeveloper)
			{
				//if still going for the same goal
				if (ms->lastgoalareanum == goal->areanum)
				{
					if (ms->lastareanum == reach.areanum)
					{
						botimport.Print(PRT_MESSAGE, "same goal, going back to previous area\n");
					} //end if
				} //end if
			} //end if
#endif //DEBUG
		} //end else
		//
		ms->lastreachnum = reachnum;
		ms->lastgoalareanum = goal->areanum;
		ms->lastareanum = ms->areanum;
		//if the bot has a reachability
		if (reachnum)
		{
			//get the reachability from the number
			AAS_ReachabilityFromNum(reachnum, &reach);
			result->traveltype = reach.traveltype;
			//
#ifdef DEBUG_AI_MOVE
			AAS_ClearShownDebugLines();
			AAS_PrintTravelType(reach.traveltype & TRAVELTYPE_MASK);
			AAS_ShowReachability(&reach);
#endif //DEBUG_AI_MOVE
			//
#ifdef DEBUG
			//botimport.Print(PRT_MESSAGE, "client %d: ", ms->client);
			//AAS_PrintTravelType(reach.traveltype);
			//botimport.Print(PRT_MESSAGE, "\n");
#endif //DEBUG
			switch(reach.traveltype & TRAVELTYPE_MASK)
			{
				case TRAVEL_WALK: *result = BotTravel_Walk(ms, &reach); break;
				case TRAVEL_CROUCH: *result = BotTravel_Crouch(ms, &reach); break;
				case TRAVEL_BARRIERJUMP: *result = BotTravel_BarrierJump(ms, &reach); break;
				case TRAVEL_LADDER: *result = BotTravel_Ladder(ms, &reach); break;
				case TRAVEL_WALKOFFLEDGE: *result = BotTravel_WalkOffLedge(ms, &reach); break;
				case TRAVEL_JUMP: *result = BotTravel_Jump(ms, &reach); break;
				case TRAVEL_SWIM: *result = BotTravel_Swim(ms, &reach); break;
				case TRAVEL_WATERJUMP: *result = BotTravel_WaterJump(ms, &reach); break;
				case TRAVEL_TELEPORT: *result = BotTravel_Teleport(ms, &reach); break;
				case TRAVEL_ELEVATOR: *result = BotTravel_Elevator(ms, &reach); break;
				case TRAVEL_GRAPPLEHOOK: *result = BotTravel_Grapple(ms, &reach); break;
				case TRAVEL_ROCKETJUMP: *result = BotTravel_RocketJump(ms, &reach); break;
				case TRAVEL_BFGJUMP: *result = BotTravel_BFGJump(ms, &reach); break;
				case TRAVEL_JUMPPAD: *result = BotTravel_JumpPad(ms, &reach); break;
				case TRAVEL_FUNCBOB: *result = BotTravel_FuncBobbing(ms, &reach); break;
				default:
				{
					botimport.Print(PRT_FATAL, "travel type %d not implemented yet\n", (reach.traveltype & TRAVELTYPE_MASK));
					break;
				} //end case
			} //end switch
			result->traveltype = reach.traveltype;
			result->flags |= resultflags;
		} //end if
		else
		{
			result->failure = qtrue;
			result->flags |= resultflags;
			Com_Memset(&reach, 0, sizeof(aas_reachability_t));
		} //end else
#ifdef DEBUG
		if (botDeveloper)
		{
			if (result->failure)
			{
				botimport.Print(PRT_MESSAGE, "client %d: movement failure in ", ms->client);
				AAS_PrintTravelType(reach.traveltype & TRAVELTYPE_MASK);
				botimport.Print(PRT_MESSAGE, "\n");
			} //end if
		} //end if
#endif //DEBUG
	} //end if
	else
	{
		int i, numareas, areas[16];
		vec3_t end;

		//special handling of jump pads when the bot uses a jump pad without knowing it
		foundjumppad = qfalse;
		VectorMA(ms->origin, -2 * ms->thinktime, ms->velocity, end);
		numareas = AAS_TraceAreas(ms->origin, end, areas, NULL, 16);
		for (i = numareas-1; i >= 0; i--)
		{
			if (AAS_AreaJumpPad(areas[i]))
			{
				//botimport.Print(PRT_MESSAGE, "client %d used a jumppad without knowing, area %d\n", ms->client, areas[i]);
				foundjumppad = qtrue;
				lastreachnum = BotGetReachabilityToGoal(end, areas[i],
							ms->lastgoalareanum, ms->lastareanum,
							ms->avoidreach, ms->avoidreachtimes, ms->avoidreachtries,
							goal, TFL_JUMPPAD, ms->avoidspots, ms->numavoidspots, NULL);
				if (lastreachnum)
				{
					ms->lastreachnum = lastreachnum;
					ms->lastareanum = areas[i];
					//botimport.Print(PRT_MESSAGE, "found jumppad reachability\n");
					break;
				} //end if
				else
				{
					for (lastreachnum = AAS_NextAreaReachability(areas[i], 0); lastreachnum;
						lastreachnum = AAS_NextAreaReachability(areas[i], lastreachnum))
					{
						//get the reachability from the number
						AAS_ReachabilityFromNum(lastreachnum, &reach);
						if ((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_JUMPPAD)
						{
							ms->lastreachnum = lastreachnum;
							ms->lastareanum = areas[i];
							//botimport.Print(PRT_MESSAGE, "found jumppad reachability hard!!\n");
						} //end if
					} //end for
					if (lastreachnum) break;
				} //end else
			} //end if
		} //end for
		if (botDeveloper)
		{
			//if a jumppad is found with the trace but no reachability is found
			if (foundjumppad && !ms->lastreachnum)
			{
				botimport.Print(PRT_MESSAGE, "client %d didn't find jumppad reachability\n", ms->client);
			} //end if
		} //end if
		//
		if (ms->lastreachnum)
		{
			//botimport.Print(PRT_MESSAGE, "%s: NOT onground, swimming or against ladder\n", ClientName(ms->entitynum-1));
			AAS_ReachabilityFromNum(ms->lastreachnum, &reach);
			result->traveltype = reach.traveltype;
#ifdef DEBUG
			//botimport.Print(PRT_MESSAGE, "client %d finish: ", ms->client);
			//AAS_PrintTravelType(reach.traveltype & TRAVELTYPE_MASK);
			//botimport.Print(PRT_MESSAGE, "\n");
#endif //DEBUG
			//
			switch(reach.traveltype & TRAVELTYPE_MASK)
			{
				case TRAVEL_WALK: *result = BotTravel_Walk(ms, &reach); break;//BotFinishTravel_Walk(ms, &reach); break;
				case TRAVEL_CROUCH: /*do nothing*/ break;
				case TRAVEL_BARRIERJUMP: *result = BotFinishTravel_BarrierJump(ms, &reach); break;
				case TRAVEL_LADDER: *result = BotTravel_Ladder(ms, &reach); break;
				case TRAVEL_WALKOFFLEDGE: *result = BotFinishTravel_WalkOffLedge(ms, &reach); break;
				case TRAVEL_JUMP: *result = BotFinishTravel_Jump(ms, &reach); break;
				case TRAVEL_SWIM: *result = BotTravel_Swim(ms, &reach); break;
				case TRAVEL_WATERJUMP: *result = BotFinishTravel_WaterJump(ms, &reach); break;
				case TRAVEL_TELEPORT: /*do nothing*/ break;
				case TRAVEL_ELEVATOR: *result = BotFinishTravel_Elevator(ms, &reach); break;
				case TRAVEL_GRAPPLEHOOK: *result = BotTravel_Grapple(ms, &reach); break;
				case TRAVEL_ROCKETJUMP:
				case TRAVEL_BFGJUMP: *result = BotFinishTravel_WeaponJump(ms, &reach); break;
				case TRAVEL_JUMPPAD: *result = BotFinishTravel_JumpPad(ms, &reach); break;
				case TRAVEL_FUNCBOB: *result = BotFinishTravel_FuncBobbing(ms, &reach); break;
				default:
				{
					botimport.Print(PRT_FATAL, "(last) travel type %d not implemented yet\n", (reach.traveltype & TRAVELTYPE_MASK));
					break;
				} //end case
			} //end switch
			result->traveltype = reach.traveltype;
#ifdef DEBUG
			if (botDeveloper)
			{
				if (result->failure)
				{
					botimport.Print(PRT_MESSAGE, "client %d: movement failure in finish ", ms->client);
					AAS_PrintTravelType(reach.traveltype & TRAVELTYPE_MASK);
					botimport.Print(PRT_MESSAGE, "\n");
				} //end if
			} //end if
#endif //DEBUG
		} //end if
	} //end else
	//FIXME: is it right to do this here?
	if (result->blocked) ms->reachability_time -= 10 * ms->thinktime;
	//copy the last origin
	VectorCopy(ms->origin, ms->lastorigin);
} //end of the function BotMoveToGoal
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void BotResetAvoidReach(int movestate)
{
	bot_movestate_t *ms;

	ms = BotMoveStateFromHandle(movestate);
	if (!ms) return;
	Com_Memset(ms->avoidreach, 0, MAX_AVOIDREACH * sizeof(int));
	Com_Memset(ms->avoidreachtimes, 0, MAX_AVOIDREACH * sizeof(float));
	Com_Memset(ms->avoidreachtries, 0, MAX_AVOIDREACH * sizeof(int));
} //end of the function BotResetAvoidReach
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void BotResetLastAvoidReach(int movestate)
{
	int i, latest;
	float latesttime;
	bot_movestate_t *ms;

	ms = BotMoveStateFromHandle(movestate);
	if (!ms) return;
	latesttime = 0;
	latest = 0;
	for (i = 0; i < MAX_AVOIDREACH; i++)
	{
		if (ms->avoidreachtimes[i] > latesttime)
		{
			latesttime = ms->avoidreachtimes[i];
			latest = i;
		} //end if
	} //end for
	if (latesttime)
	{
		ms->avoidreachtimes[latest] = 0;
		if (ms->avoidreachtries[latest] > 0) ms->avoidreachtries[latest]--;
	} //end if
} //end of the function BotResetLastAvoidReach
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
void BotResetMoveState(int movestate)
{
	bot_movestate_t *ms;

	ms = BotMoveStateFromHandle(movestate);
	if (!ms) return;
	//a reset mid-shot (death, restart) still ends the outstanding route tow
	BotGrappleRouteEnd(ms);
	if (ms->client >= 0 && ms->client < MAX_CLIENTS && botmovestate_byclient[ms->client] == ms)
		botmovestate_byclient[ms->client] = NULL;
	Com_Memset(ms, 0, sizeof(bot_movestate_t));
} //end of the function BotResetMoveState
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int BotSetupMoveAI(void)
{
	BotSetBrushModelTypes();
	sv_maxstep = LibVar("sv_step", "18");
	sv_maxbarrier = LibVar("sv_maxbarrier", "32");
	sv_gravity = LibVar("sv_gravity", "800");
	weapindex_rocketlauncher = LibVar("weapindex_rocketlauncher", "5");
	weapindex_bfg10k = LibVar("weapindex_bfg10k", "9");
	weapindex_grapple = LibVar("weapindex_grapple", "10");
	offhandgrapple = LibVar("offhandgrapple", "0");
	cmd_grappleon = LibVar("cmd_grappleon", "grappleon");
	cmd_grappleoff = LibVar("cmd_grappleoff", "grappleoff");
	pmf_grapplepull = LibVar("pmf_grapplepull", "2048");
	return BLERR_NOERROR;
} //end of the function BotSetupMoveAI
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
void BotShutdownMoveAI(void)
{
	int i;

	for (i = 1; i <= MAX_CLIENTS; i++)
	{
		if (botmovestates[i])
		{
			FreeMemory(botmovestates[i]);
			botmovestates[i] = NULL;
		} //end if
	} //end for
} //end of the function BotShutdownMoveAI


