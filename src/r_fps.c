// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2000 by Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze, Andrey Budko (prboom)
// Copyright (C) 1999-2019 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_fps.h
/// \brief Uncapped framerate stuff.

#include "r_fps.h"

#include "r_main.h"
#include "g_game.h"
#include "i_video.h"
#include "r_plane.h"
#include "p_spec.h"
#include "r_state.h"
#include "z_zone.h"
#ifdef HWRENDER
#include "hardware/hw_main.h" // for cv_glshearing
#endif

static viewvars_t p1view_old;
static viewvars_t p1view_new;
static viewvars_t p2view_old;
static viewvars_t p2view_new;
static viewvars_t sky1view_old;
static viewvars_t sky1view_new;
static viewvars_t sky2view_old;
static viewvars_t sky2view_new;

static viewvars_t *oldview = &p1view_old;
static boolean oldview_valid = false;
viewvars_t *newview = &p1view_new;


enum viewcontext_e viewcontext = VIEWCONTEXT_PLAYER1;

static levelinterpolator_t **levelinterpolators;
static size_t levelinterpolators_len;
static size_t levelinterpolators_size;


static fixed_t R_LerpFixed(fixed_t from, fixed_t to, fixed_t frac)
{
	return from + FixedMul(frac, to - from);
}

static angle_t R_LerpAngle(angle_t from, angle_t to, fixed_t frac)
{
	return from + FixedMul(frac, to - from);
}

// recalc necessary stuff for mouseaiming
// slopes are already calculated for the full possible view (which is 4*viewheight).
// 18/08/18: (No it's actually 16*viewheight, thanks Jimita for finding this out)
static void R_SetupFreelook(player_t *player, boolean skybox)
{
#ifndef HWRENDER
	(void)player;
	(void)skybox;
#endif

	// clip it in the case we are looking a hardware 90 degrees full aiming
	// (lmps, network and use F12...)
	if (rendermode == render_soft
#ifdef HWRENDER
		|| (rendermode == render_opengl
			&& (cv_glshearing.value == 1
			|| (cv_glshearing.value == 2 && R_IsViewpointThirdPerson(player, skybox))))
#endif
		)
	{
		G_SoftwareClipAimingPitch((INT32 *)&aimingangle);
	}

	centeryfrac = (viewheight/2)<<FRACBITS;

	if (rendermode == render_soft)
		centeryfrac += FixedMul(AIMINGTODY(aimingangle), FixedDiv(viewwidth<<FRACBITS, BASEVIDWIDTH<<FRACBITS));

	centery = FixedInt(FixedRound(centeryfrac));

	if (rendermode == render_soft)
		yslope = &yslopetab[viewheight*8 - centery];
}

#undef AIMINGTODY

void R_InterpolateView(fixed_t frac)
{
	viewvars_t* prevview = oldview;
	boolean skybox = 0;
	if (FIXED_TO_FLOAT(frac) < 0)
		frac = 0;
	if (frac > FRACUNIT)
		frac = FRACUNIT;

	if (oldview_valid == false)
	{
		// interpolate from newview to newview
		prevview = newview;
	}

	viewx = R_LerpFixed(prevview->x, newview->x, frac);
	viewy = R_LerpFixed(prevview->y, newview->y, frac);
	viewz = R_LerpFixed(prevview->z, newview->z, frac);

	viewangle = R_LerpAngle(prevview->angle, newview->angle, frac);
	aimingangle = R_LerpAngle(prevview->aim, newview->aim, frac);

	viewsin = FINESINE(viewangle>>ANGLETOFINESHIFT);
	viewcos = FINECOSINE(viewangle>>ANGLETOFINESHIFT);

	// this is gonna create some interesting visual errors for long distance teleports...
	// might want to recalculate the view sector every frame instead...
	viewplayer = newview->player;
	viewsector = R_PointInSubsector(viewx, viewy)->sector;

	// well, this ain't pretty
	if (newview == &sky1view_new || newview == &sky2view_new)
	{
		skybox = 1;
	}

	R_SetupFreelook(newview->player, skybox);
}

void R_UpdateViewInterpolation(void)
{
	p1view_old = p1view_new;
	p2view_old = p2view_new;
	sky1view_old = sky1view_new;
	sky2view_old = sky2view_new;
	oldview_valid = true;
}

void R_ResetViewInterpolation(void)
{
	oldview_valid = false;
}

void R_SetViewContext(enum viewcontext_e _viewcontext)
{
	I_Assert(_viewcontext == VIEWCONTEXT_PLAYER1
			|| _viewcontext == VIEWCONTEXT_PLAYER2
			|| _viewcontext == VIEWCONTEXT_SKY1
			|| _viewcontext == VIEWCONTEXT_SKY2);
	viewcontext = _viewcontext;

	switch (viewcontext)
	{
		case VIEWCONTEXT_PLAYER1:
			oldview = &p1view_old;
			newview = &p1view_new;
			break;
		case VIEWCONTEXT_PLAYER2:
			oldview = &p2view_old;
			newview = &p2view_new;
			break;
		case VIEWCONTEXT_SKY1:
			oldview = &sky1view_old;
			newview = &sky1view_new;
			break;
		case VIEWCONTEXT_SKY2:
			oldview = &sky2view_old;
			newview = &sky2view_new;
			break;
		default:
			I_Error("viewcontext value is invalid: we should never get here without an assert!!");
			break;
	}
}

void R_InterpolateMobjState(mobj_t *mobj, fixed_t frac, interpmobjstate_t *out)
{
	out->x = R_LerpFixed(mobj->old_x, mobj->x, frac);
	out->y = R_LerpFixed(mobj->old_y, mobj->y, frac);
	out->z = R_LerpFixed(mobj->old_z, mobj->z, frac);

	if (mobj->player)
	{
		out->angle = mobj->player->drawangle;
	}
	else
	{
		out->angle = mobj->angle;
	}
}

void R_InterpolatePrecipMobjState(precipmobj_t *mobj, fixed_t frac, interpmobjstate_t *out)
{
	out->x = R_LerpFixed(mobj->old_x, mobj->x, frac);
	out->y = R_LerpFixed(mobj->old_y, mobj->y, frac);
	out->z = R_LerpFixed(mobj->old_z, mobj->z, frac);
	out->angle = mobj->angle;
}

static void AddInterpolator(levelinterpolator_t* interpolator)
{
	if (levelinterpolators_len >= levelinterpolators_size)
	{
		if (levelinterpolators_size == 0)
		{
			levelinterpolators_size = 128;
		}
		else
		{
			levelinterpolators_size *= 2;
		}
		
		levelinterpolators = Z_ReallocAlign(
			(void*) levelinterpolators,
			sizeof(levelinterpolator_t*) * levelinterpolators_size,
			PU_LEVEL,
			NULL,
			sizeof(levelinterpolator_t*) * 8
		);
	}

	levelinterpolators[levelinterpolators_len] = interpolator;
	levelinterpolators_len += 1;
}

static levelinterpolator_t *CreateInterpolator(levelinterpolator_type_e type, thinker_t *thinker)
{
	levelinterpolator_t *ret = (levelinterpolator_t*) Z_CallocAlign(
		sizeof(levelinterpolator_t),
		PU_LEVEL,
		NULL,
		sizeof(levelinterpolator_t) * 8
	);

	ret->type = type;
	ret->thinker = thinker;

	AddInterpolator(ret);

	return ret;
}

void R_CreateInterpolator_SectorPlane(thinker_t *thinker, sector_t *sector, boolean ceiling)
{
	levelinterpolator_t *interp = CreateInterpolator(LVLINTERP_SectorPlane, thinker);
	interp->sectorplane.sector = sector;
	interp->sectorplane.ceiling = ceiling;
	if (ceiling)
	{
		interp->sectorplane.oldheight = interp->sectorplane.bakheight = sector->ceilingheight;
	}
	else
	{
		interp->sectorplane.oldheight = interp->sectorplane.bakheight = sector->floorheight;
	}
}

void R_InitializeLevelInterpolators(void)
{
	levelinterpolators_len = 0;
	levelinterpolators_size = 0;
	levelinterpolators = NULL;
}

static void UpdateLevelInterpolatorState(levelinterpolator_t *interp)
{
	switch (interp->type)
	{
	case LVLINTERP_SectorPlane:
		interp->sectorplane.oldheight = interp->sectorplane.bakheight;
		interp->sectorplane.bakheight = interp->sectorplane.ceiling ? interp->sectorplane.sector->ceilingheight : interp->sectorplane.sector->floorheight;
		break;
	}
}

void R_UpdateLevelInterpolators(void)
{
	size_t i;

	for (i = 0; i < levelinterpolators_len; i++)
	{
		levelinterpolator_t *interp = levelinterpolators[i];
		
		UpdateLevelInterpolatorState(interp);
	}
}

void R_ClearLevelInterpolatorState(thinker_t *thinker)
{
	
	size_t i;

	for (i = 0; i < levelinterpolators_len; i++)
	{
		levelinterpolator_t *interp = levelinterpolators[i];
		
		if (interp->thinker == thinker)
		{
			// Do it twice to make the old state match the new
			UpdateLevelInterpolatorState(interp);
			UpdateLevelInterpolatorState(interp);
		}
	}
}

void R_ApplyLevelInterpolators(fixed_t frac)
{
	size_t i;

	for (i = 0; i < levelinterpolators_len; i++)
	{
		levelinterpolator_t *interp = levelinterpolators[i];

		switch (interp->type)
		{
		case LVLINTERP_SectorPlane:
			if (interp->sectorplane.ceiling)
			{
				interp->sectorplane.sector->ceilingheight = R_LerpFixed(interp->sectorplane.oldheight, interp->sectorplane.bakheight, frac);
			}
			else
			{
				interp->sectorplane.sector->floorheight = R_LerpFixed(interp->sectorplane.oldheight, interp->sectorplane.bakheight, frac);
			}
		}
	}
}

void R_RestoreLevelInterpolators(void)
{
	size_t i;

	for (i = 0; i < levelinterpolators_len; i++)
	{
		levelinterpolator_t *interp = levelinterpolators[i];
		
		switch (interp->type)
		{
		case LVLINTERP_SectorPlane:
			if (interp->sectorplane.ceiling)
			{
				interp->sectorplane.sector->ceilingheight = interp->sectorplane.bakheight;
			}
			else
			{
				interp->sectorplane.sector->floorheight = interp->sectorplane.bakheight;
			}
		}
	}
}
