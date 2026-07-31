#include "quakedef.h"

// Radial weapon selector. The VR layer (QuakeQuest_OpenXR.c) holds the grip, freezes the wheel
// plane and drives the cursor. This file owns the weapon list, the ring geometry, the hit test,
// the models and the drawing.

cvar_t vr_weaponwheel = {CVAR_SAVE, "vr_weaponwheel", "1", "enable the radial weapon selector on the dominant hand grip"};
cvar_t vr_weaponwheel_distance = {CVAR_SAVE, "vr_weaponwheel_distance", "0.35", "distance in metres from the hand to the centre of the weapon wheel"};
cvar_t vr_weaponwheel_radius = {CVAR_SAVE, "vr_weaponwheel_radius", "0.2", "radius in metres of the weapon wheel"};
cvar_t vr_weaponwheel_modelsize = {CVAR_SAVE, "vr_weaponwheel_modelsize", "0.11", "size in metres of the weapon models on the wheel"};
cvar_t vr_weaponwheel_modelpitch = {CVAR_SAVE, "vr_weaponwheel_modelpitch", "20", "pitch of the weapon models on the wheel"};
cvar_t vr_weaponwheel_modelyaw = {CVAR_SAVE, "vr_weaponwheel_modelyaw", "-145", "yaw of the weapon models on the wheel, relative to the wheel"};
cvar_t vr_weaponwheel_spin = {CVAR_SAVE, "vr_weaponwheel_spin", "45", "degrees per second that the weapon models turn on the wheel, 0 stops them"};
cvar_t vr_weaponwheel_deflection = {CVAR_SAVE, "vr_weaponwheel_deflection", "22.5", "degrees of controller rotation needed to move the cursor to the edge of the wheel"};
cvar_t vr_weaponwheel_slowmo = {CVAR_SAVE, "vr_weaponwheel_slowmo", "0.3", "game speed while the weapon wheel is open, 1 disables the slow down"};

// written by the VR layer
int weaponwheel_active;
float weaponwheel_angles[3];
float weaponwheel_cursor[2];
// written here, read by the VR layer for haptics
int weaponwheel_selection = -1;

extern float gunorg[3];
extern cvar_t vr_worldscale;
extern cvar_t cl_righthanded;
extern cvar_t slowmo;

void R_DrawBLineMesh(vec3_t mins, vec3_t maxs, float thickness, float cr, float cg, float cb, float ca);
void TBXR_Vibrate(int duration, int chan, float intensity);

typedef struct wheelweapon_s
{
	int itembit;
	int impulse;
	const char *modelname;
	const char *name;
	int ammostat;	// -1 when the weapon needs no ammo
}
wheelweapon_t;

// impulse and item bit values must agree with IN_BestWeapon_ResetData in cl_input.c
static const wheelweapon_t wheelweapons_quake[] =
{
	{IT_AXE,				1, "progs/v_axe.mdl",	"Axe",				-1},
	{IT_SHOTGUN,			2, "progs/v_shot.mdl",	"Shotgun",			STAT_SHELLS},
	{IT_SUPER_SHOTGUN,		3, "progs/v_shot2.mdl",	"Super Shotgun",	STAT_SHELLS},
	{IT_NAILGUN,			4, "progs/v_nail.mdl",	"Nailgun",			STAT_NAILS},
	{IT_SUPER_NAILGUN,		5, "progs/v_nail2.mdl",	"Super Nailgun",	STAT_NAILS},
	{IT_GRENADE_LAUNCHER,	6, "progs/v_rock.mdl",	"Grenade Launcher",	STAT_ROCKETS},
	{IT_ROCKET_LAUNCHER,	7, "progs/v_rock2.mdl",	"Rocket Launcher",	STAT_ROCKETS},
	{IT_LIGHTNING,			8, "progs/v_light.mdl",	"Thunderbolt",		STAT_CELLS},
};

// impulses 225 and 226 are the hipnotic weapon selectors that IN_BestWeapon_ResetData documents.
// The proximity gun and every Dissolution of Eternity weapon are left out on purpose: those mods
// toggle two weapons on one impulse, and Rogue reuses the stock item bits for different weapons.
static const wheelweapon_t wheelweapons_hipnotic[] =
{
	{HIT_LASER_CANNON,	225, "progs/v_laserg.mdl",	"Laser Cannon",	STAT_CELLS},
	{HIT_MJOLNIR,		226, "progs/v_hammer.mdl",	"Mjolnir",		-1},
};

#define WHEEL_NUM_QUAKE ((int)(sizeof(wheelweapons_quake) / sizeof(wheelweapons_quake[0])))
#define WHEEL_NUM_HIPNOTIC ((int)(sizeof(wheelweapons_hipnotic) / sizeof(wheelweapons_hipnotic[0])))
#define MAX_WHEEL_SLOTS (WHEEL_NUM_QUAKE + WHEEL_NUM_HIPNOTIC)

typedef struct wheelslot_s
{
	const wheelweapon_t *weapon;
	dp_model_t *model;
	qboolean owned;
	float grow;			// 0 = normal size, 1 = fully highlighted
	vec3_t origin;
}
wheelslot_t;

static wheelslot_t wheelslots[MAX_WHEEL_SLOTS];
static int wheelslotcount;
static float wheelspin;
static vec3_t wheelcursorworld;
static float wheel_savedslowmo = 1.0f;
static qboolean wheel_slowmoactive = false;

void R_WeaponWheel_Init(void)
{
	Cvar_RegisterVariable(&vr_weaponwheel);
	Cvar_RegisterVariable(&vr_weaponwheel_distance);
	Cvar_RegisterVariable(&vr_weaponwheel_radius);
	Cvar_RegisterVariable(&vr_weaponwheel_modelsize);
	Cvar_RegisterVariable(&vr_weaponwheel_modelpitch);
	Cvar_RegisterVariable(&vr_weaponwheel_modelyaw);
	Cvar_RegisterVariable(&vr_weaponwheel_spin);
	Cvar_RegisterVariable(&vr_weaponwheel_deflection);
	Cvar_RegisterVariable(&vr_weaponwheel_slowmo);
}

// prefer the map's own precache, which is always fully loaded and owned by the map
static dp_model_t *CL_WeaponWheel_FindModel(const char *name)
{
	int i;
	dp_model_t *model;

	for (i = 1;i < MAX_MODELS && cl.model_name[i][0];i++)
		if (!strcmp(cl.model_name[i], name))
			return cl.model_precache[i];

	model = Mod_ForName(name, false, false, NULL);
	return (model && model->loaded) ? model : NULL;
}

qboolean CL_WeaponWheel_CanOpen(void)
{
	qboolean cldead = (cl.stats[STAT_HEALTH] <= 0 && cl.stats[STAT_HEALTH] != -666 && cl.stats[STAT_HEALTH] != -2342);

	if (!vr_weaponwheel.integer)
		return false;
	if (cls.state != ca_connected || cls.signon != SIGNONS)
		return false;
	if (cl.intermission || cls.demoplayback || cldead)
		return false;
	return true;
}

static void CL_WeaponWheel_AddWeapons(const wheelweapon_t *weapons, int count)
{
	int i;
	for (i = 0;i < count && wheelslotcount < MAX_WHEEL_SLOTS;i++)
	{
		wheelslot_t *slot = &wheelslots[wheelslotcount++];
		slot->weapon = &weapons[i];
		slot->owned = (cl.stats[STAT_ITEMS] & weapons[i].itembit) != 0;
		slot->model = slot->owned ? CL_WeaponWheel_FindModel(weapons[i].modelname) : NULL;
		slot->grow = 0.0f;
		VectorClear(slot->origin);
	}
}

void CL_WeaponWheel_Open(void)
{
	wheelslotcount = 0;
	weaponwheel_selection = -1;
	wheelspin = 0.0f;

	CL_WeaponWheel_AddWeapons(wheelweapons_quake, WHEEL_NUM_QUAKE);
	if (gamemode == GAME_HIPNOTIC || gamemode == GAME_QUOTH)
		CL_WeaponWheel_AddWeapons(wheelweapons_hipnotic, WHEEL_NUM_HIPNOTIC);

	weaponwheel_active = 1;

	if (!wheel_slowmoactive && vr_weaponwheel_slowmo.value < 1.0f && vr_weaponwheel_slowmo.value > 0.0f
		&& cl.maxclients <= 1 && !cls.demoplayback)
	{
		wheel_savedslowmo = slowmo.value;
		wheel_slowmoactive = true;
		Cvar_SetValueQuick(&slowmo, vr_weaponwheel_slowmo.value);
	}
}

void CL_WeaponWheel_Close(void)
{
	weaponwheel_active = 0;
	weaponwheel_selection = -1;
	wheelslotcount = 0;

	if (wheel_slowmoactive)
	{
		wheel_slowmoactive = false;
		Cvar_SetValueQuick(&slowmo, wheel_savedslowmo);
	}
}

void CL_WeaponWheel_Select(void)
{
	int selection = weaponwheel_selection;
	int impulse = 0;
	char vabuf[1024];

	if (selection >= 0 && selection < wheelslotcount && wheelslots[selection].owned
		&& cl.stats[STAT_ACTIVEWEAPON] != wheelslots[selection].weapon->itembit)
		impulse = wheelslots[selection].weapon->impulse;

	CL_WeaponWheel_Close();

	if (impulse)
		Cbuf_AddText(va(vabuf, sizeof(vabuf), "impulse %i\n", impulse));
}

// v_*.mdl models have their origin at the grip, and they differ widely in length. Centre each
// model on its slot and scale it so every weapon reads at the same size on the ring.
static float CL_WeaponWheel_ModelScale(const dp_model_t *model, float targetsize)
{
	float biggest = model->normalmaxs[0] - model->normalmins[0];
	float sy = model->normalmaxs[1] - model->normalmins[1];
	float sz = model->normalmaxs[2] - model->normalmins[2];

	if (sy > biggest)
		biggest = sy;
	if (sz > biggest)
		biggest = sz;
	if (biggest < 0.001f)
		biggest = 0.001f;
	return targetsize / biggest;
}

static void CL_WeaponWheel_PlaceModel(entity_render_t *entrender, const vec3_t target, const vec3_t angles, float scale)
{
	matrix4x4_t rotation;
	vec3_t centre, offset, origin;

	centre[0] = (entrender->model->normalmins[0] + entrender->model->normalmaxs[0]) * 0.5f;
	centre[1] = (entrender->model->normalmins[1] + entrender->model->normalmaxs[1]) * 0.5f;
	centre[2] = (entrender->model->normalmins[2] + entrender->model->normalmaxs[2]) * 0.5f;

	Matrix4x4_CreateFromQuakeEntity(&rotation, 0, 0, 0, angles[0], angles[1], angles[2], scale);
	Matrix4x4_Transform(&rotation, centre, offset);
	VectorSubtract(target, offset, origin);

	Matrix4x4_CreateFromQuakeEntity(&entrender->matrix, origin[0], origin[1], origin[2],
		angles[0], angles[1], angles[2], scale);
}

static float CL_WeaponWheel_AngleDifference(float a, float b)
{
	float difference = a - b;
	while (difference > 180.0f)
		difference -= 360.0f;
	while (difference < -180.0f)
		difference += 360.0f;
	return difference;
}

static entity_render_t *CL_WeaponWheel_NewEntity(dp_model_t *model)
{
	entity_render_t *entrender = CL_NewTempEntity(0);

	if (!entrender)
		return NULL;

	entrender->model = model;
	// no RENDER_LIGHT, so the models render fullbright and stay readable in a dark room.
	// RENDER_NODEPTHTEST keeps the wheel in front of nearby walls.
	entrender->flags = RENDER_NODEPTHTEST;
	entrender->alpha = 1.0f;
	// CL_NewTempEntity zeroes this, and an animated model draws nothing at lerp 0
	entrender->framegroupblend[0].frame = 0;
	entrender->framegroupblend[0].start = cl.time;
	entrender->framegroupblend[0].lerp = 1.0f;
	return entrender;
}

// places the slots in world space, picks the one under the cursor and adds the models to the scene
void CL_WeaponWheel_Relink(void)
{
	int i;
	int previousselection = weaponwheel_selection;
	float worldscale, dist, radius, cursorlength, cursorangle, bestangle;
	float modelsize, framelerp;
	vec3_t forward, right, up, hub, angles;

	if (!weaponwheel_active || !wheelslotcount)
		return;

	worldscale = bound(1.0f, vr_worldscale.value, 60.0f);
	dist = vr_weaponwheel_distance.value * worldscale;
	radius = vr_weaponwheel_radius.value * worldscale;
	modelsize = vr_weaponwheel_modelsize.value * worldscale;

	AngleVectors(weaponwheel_angles, forward, right, up);
	VectorMA(gunorg, dist, forward, hub);

	VectorMA(hub, radius * weaponwheel_cursor[0], right, wheelcursorworld);
	VectorMA(wheelcursorworld, radius * weaponwheel_cursor[1], up, wheelcursorworld);

	// walk the ring by rolling the wheel plane, which keeps every slot on the disc
	for (i = 0;i < wheelslotcount;i++)
	{
		vec3_t slotangles, slotup;

		slotangles[PITCH] = weaponwheel_angles[PITCH];
		slotangles[YAW] = weaponwheel_angles[YAW];
		slotangles[ROLL] = (360.0f / wheelslotcount) * i;
		AngleVectors(slotangles, NULL, NULL, slotup);
		VectorMA(hub, radius, slotup, wheelslots[i].origin);
	}

	// pick the owned slot closest in angle to the cursor. Measuring the slot angle from its own
	// world position avoids any assumption about which way the roll turns.
	cursorlength = sqrt(weaponwheel_cursor[0] * weaponwheel_cursor[0] + weaponwheel_cursor[1] * weaponwheel_cursor[1]);
	weaponwheel_selection = -1;
	bestangle = 360.0f;
	if (cursorlength >= 0.35f)
	{
		cursorangle = RAD2DEG(atan2(weaponwheel_cursor[0], weaponwheel_cursor[1]));
		for (i = 0;i < wheelslotcount;i++)
		{
			vec3_t slotdir;
			float difference;

			if (!wheelslots[i].owned)
				continue;

			VectorSubtract(wheelslots[i].origin, hub, slotdir);
			difference = fabs(CL_WeaponWheel_AngleDifference(cursorangle,
				RAD2DEG(atan2(DotProduct(slotdir, right), DotProduct(slotdir, up)))));
			if (difference < bestangle)
			{
				bestangle = difference;
				weaponwheel_selection = i;
			}
		}
	}

	if (weaponwheel_selection != previousselection && weaponwheel_selection >= 0)
		TBXR_Vibrate(50, cl_righthanded.integer ? 2 : 1, 0.6f);

	// the wheel runs while the game is slowed down, so drive both animations off real time
	framelerp = bound(0.0f, (float)(cl.realframetime * 12.0), 1.0f);
	wheelspin += (float)(cl.realframetime * vr_weaponwheel_spin.value);
	while (wheelspin >= 360.0f)
		wheelspin -= 360.0f;
	while (wheelspin < 0.0f)
		wheelspin += 360.0f;

	VectorClear(angles);
	angles[PITCH] = vr_weaponwheel_modelpitch.value;
	angles[YAW] = weaponwheel_angles[YAW] + vr_weaponwheel_modelyaw.value + wheelspin;

	for (i = 0;i < wheelslotcount;i++)
	{
		entity_render_t *entrender;
		float shade, size;

		if (!wheelslots[i].owned || !wheelslots[i].model)
			continue;

		wheelslots[i].grow += ((i == weaponwheel_selection ? 1.0f : 0.0f) - wheelslots[i].grow) * framelerp;

		entrender = CL_WeaponWheel_NewEntity(wheelslots[i].model);
		if (!entrender)
			return;

		size = modelsize * (1.0f + 0.6f * wheelslots[i].grow);
		shade = 0.55f + 0.95f * wheelslots[i].grow;
		VectorSet(entrender->colormod, shade, shade, shade * 0.9f);
		CL_WeaponWheel_PlaceModel(entrender, wheelslots[i].origin, angles,
			CL_WeaponWheel_ModelScale(wheelslots[i].model, size));
		CL_UpdateRenderEntity(entrender);
	}

	// the weapon currently held sits at the hub, so the player can see what they are swapping from
	for (i = 0;i < wheelslotcount;i++)
	{
		entity_render_t *entrender;

		if (cl.stats[STAT_ACTIVEWEAPON] != wheelslots[i].weapon->itembit || !wheelslots[i].model)
			continue;

		entrender = CL_WeaponWheel_NewEntity(wheelslots[i].model);
		if (!entrender)
			return;

		VectorSet(entrender->colormod, 0.45f, 0.45f, 0.5f);
		CL_WeaponWheel_PlaceModel(entrender, hub, angles,
			CL_WeaponWheel_ModelScale(wheelslots[i].model, modelsize * 0.7f));
		CL_UpdateRenderEntity(entrender);
		break;
	}
}

void R_DrawWeaponWheel(void)
{
	float thickness, crosssize;
	vec3_t forward, right, up, a, b;

	if (!weaponwheel_active || !wheelslotcount)
		return;

	thickness = 0.008f * bound(1.0f, vr_worldscale.value, 60.0f);
	crosssize = thickness * 4.0f;
	AngleVectors(weaponwheel_angles, forward, right, up);

	GL_DepthTest(false);
	GL_CullFace(r_refdef.view.cullface_front);

	// pointer beam from the hand to the cursor
	R_DrawBLineMesh(gunorg, wheelcursorworld, thickness, 0.2f, 0.6f, 1.0f, 1.0f);

	// small cross so the cursor stays visible against a busy background
	VectorMA(wheelcursorworld, -crosssize, right, a);
	VectorMA(wheelcursorworld, crosssize, right, b);
	R_DrawBLineMesh(a, b, thickness, 1.0f, 1.0f, 1.0f, 1.0f);
	VectorMA(wheelcursorworld, -crosssize, up, a);
	VectorMA(wheelcursorworld, crosssize, up, b);
	R_DrawBLineMesh(a, b, thickness, 1.0f, 1.0f, 1.0f, 1.0f);
}

// the name of the highlighted weapon, drawn flat with the same per eye parallax the crosshair uses
void R_WeaponWheel_DrawText(void)
{
	const char *text;
	float width, x, y;
	float hudoffsetx, hudoffsety;
	int stereooffset;

	if (!weaponwheel_active || weaponwheel_selection < 0 || weaponwheel_selection >= wheelslotcount)
		return;

	text = wheelslots[weaponwheel_selection].weapon->name;
	width = DrawQ_TextWidth(text, 0, 16, 16, true, FONT_CENTERPRINT);
	stereooffset = vr_worldscale.value > 200.0f ? 12 : 5;
	GetHUDOffset(&hudoffsetx, &hudoffsety);
	x = (vid_conwidth.integer - width) * 0.5f + (r_stereo_side ? -stereooffset : stereooffset) + hudoffsetx;
	y = vid_conheight.integer * 0.68f + hudoffsety;

	DrawQ_String(x, y, text, 0, 16, 16, 1, 0.9f, 0.5f, 1, 0, NULL, true, FONT_CENTERPRINT);
}
