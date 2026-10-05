#include "sfvr/sfvr_settings.h"
#include "sfvr_test.h"

static const char *const expected_keys[] = {
	"turn_mode", "snap_angle", "smooth_turn_speed", "vignette", "vignette_strength",
	"move_relative", "roomscale", "lean_limit", "duck_crouch", "height_offset", "seated",
	"horizon_lock", "follow_vehicle_rotation", "head_translation", "camera_shake",
	"hud_mode", "hud_distance", "hud_scale", "hud_visible", "haptics", "dominant_hand",
	"refresh_rate", "resolution_scale", "force_render", "diag_yaw", "diag_hand_yaw",
	"diag_two_handed", "diag_walk_speed", "dump_frame", "probe_seconds", "timing",
	"timing_gpu", NULL
};

static void test_schema(void)
{
	CHECK_INT(SFVR_SETTINGS_SCHEMA, 1);
}

static void test_all_keys_present(void)
{
	size_t i, expected = 0;

	for (i = 0; expected_keys[i]; i++)
	{
		const sfvr_setting_key *k = sfvr_settings_find(expected_keys[i]);

		CHECK(k != NULL);
		if (k)
			CHECK_STR(k->name, expected_keys[i]);
		expected++;
	}
	CHECK_INT(sfvr_settings_count(), expected);
	CHECK(sfvr_settings_find("nope") == NULL);
	CHECK(sfvr_settings_find("") == NULL);
	CHECK(sfvr_settings_find(NULL) == NULL);
	CHECK(sfvr_settings_find("Snap_Angle") == NULL); /* exact match */
	CHECK(sfvr_settings_at(sfvr_settings_count()) == NULL);
}

static void test_table_consistent(void)
{
	size_t i, j;

	for (i = 0; i < sfvr_settings_count(); i++)
	{
		const sfvr_setting_key *k = sfvr_settings_at(i);

		CHECK(k != NULL && k->name != NULL);
		CHECK(k->min <= k->def && k->def <= k->max);
		if (k->type == SFVR_SETTING_ENUM)
		{
			int n = sfvr_settings_enum_count(k);

			CHECK(n >= 2);
			CHECK_NEAR(k->min, 0.0, 1e-9);
			CHECK_NEAR(k->max, n - 1, 1e-9);
			CHECK_NEAR(k->def, (int)k->def, 1e-9);
		}
		else
			CHECK(k->enum_values == NULL);
		if (k->type == SFVR_SETTING_BOOL)
		{
			CHECK_NEAR(k->min, 0.0, 1e-9);
			CHECK_NEAR(k->max, 1.0, 1e-9);
		}
		if (k->type == SFVR_SETTING_INT || k->type == SFVR_SETTING_BOOL)
			CHECK_NEAR(k->def, (int)k->def, 1e-9);
		for (j = i + 1; j < sfvr_settings_count(); j++)
			CHECK(strcmp(k->name, sfvr_settings_at(j)->name) != 0);
	}
}

static void test_standard_defaults(void)
{
	const sfvr_setting_key *k;

	k = sfvr_settings_find("turn_mode");
	CHECK_INT(k->type, SFVR_SETTING_ENUM);
	CHECK_STR(k->enum_values[0], "snap");
	CHECK_STR(k->enum_values[1], "smooth");
	CHECK_STR(k->enum_values[2], "off");
	CHECK(k->enum_values[3] == NULL);
	CHECK_INT(k->def, 0);
	k = sfvr_settings_find("snap_angle");
	CHECK_INT(k->type, SFVR_SETTING_INT);
	CHECK_NEAR(k->def, 30, 1e-9);
	CHECK_NEAR(k->min, 15, 1e-9);
	CHECK_NEAR(k->max, 90, 1e-9);
	k = sfvr_settings_find("smooth_turn_speed");
	CHECK_NEAR(k->def, 120, 1e-9);
	CHECK_NEAR(k->min, 30, 1e-9);
	CHECK_NEAR(k->max, 360, 1e-9);
	k = sfvr_settings_find("vignette");
	CHECK_INT(k->type, SFVR_SETTING_BOOL);
	CHECK_INT(k->def, 0);
	CHECK_NEAR(sfvr_settings_find("vignette_strength")->def, 0.6, 1e-9);
	k = sfvr_settings_find("move_relative");
	CHECK_STR(k->enum_values[0], "head");
	CHECK_STR(k->enum_values[1], "left");
	CHECK_STR(k->enum_values[2], "right");
	k = sfvr_settings_find("lean_limit");
	CHECK_NEAR(k->def, 0.35, 1e-9);
	CHECK_NEAR(k->min, 0.15, 1e-9);
	CHECK_NEAR(k->max, 0.5, 1e-9);
	CHECK_INT(sfvr_settings_find("roomscale")->def, 0);
	CHECK_INT(sfvr_settings_find("duck_crouch")->def, 1);
	CHECK_NEAR(sfvr_settings_find("height_offset")->def, 0, 1e-9);
	CHECK_INT(sfvr_settings_find("seated")->def, 0);
	CHECK_INT(sfvr_settings_find("horizon_lock")->def, 0);
	CHECK_INT(sfvr_settings_find("follow_vehicle_rotation")->def, 0);
	k = sfvr_settings_find("head_translation");
	CHECK_NEAR(k->def, 1.0, 1e-9);
	CHECK_NEAR(k->min, 0.0, 1e-9);
	CHECK_NEAR(k->max, 2.0, 1e-9);
	CHECK_INT(sfvr_settings_find("camera_shake")->def, 0);
	k = sfvr_settings_find("hud_mode");
	CHECK_STR(k->enum_values[0], "near");
	CHECK_STR(k->enum_values[1], "far");
	CHECK_STR(k->enum_values[2], "leash");
	k = sfvr_settings_find("hud_scale");
	CHECK_NEAR(k->min, 50, 1e-9);
	CHECK_NEAR(k->max, 200, 1e-9);
	CHECK_NEAR(k->def, 100, 1e-9);
	CHECK_INT(sfvr_settings_find("hud_visible")->def, 1);
	CHECK_NEAR(sfvr_settings_find("haptics")->def, 0.6, 1e-9);
	k = sfvr_settings_find("dominant_hand");
	CHECK_STR(k->enum_values[0], "right");
	CHECK_STR(k->enum_values[1], "left");
	CHECK_INT(k->def, 0);
	CHECK_NEAR(sfvr_settings_find("refresh_rate")->def, 90, 1e-9);
	k = sfvr_settings_find("resolution_scale");
	CHECK_NEAR(k->def, 1.0, 1e-9);
	CHECK_NEAR(k->min, 0.75, 1e-9);
	CHECK_NEAR(k->max, 1.5, 1e-9);
}

static void test_clamp(void)
{
	const sfvr_setting_key *snap = sfvr_settings_find("snap_angle");
	const sfvr_setting_key *lean = sfvr_settings_find("lean_limit");

	CHECK_INT(sfvr_settings_clamp_int(snap, 45), 45);
	CHECK_INT(sfvr_settings_clamp_int(snap, 5), 15);
	CHECK_INT(sfvr_settings_clamp_int(snap, 15), 15);
	CHECK_INT(sfvr_settings_clamp_int(snap, 90), 90);
	CHECK_INT(sfvr_settings_clamp_int(snap, 1000), 90);
	CHECK_INT(sfvr_settings_clamp_int(snap, -50), 15);
	CHECK_INT(sfvr_settings_clamp_int(NULL, 1234), 1234);
	CHECK_NEAR(sfvr_settings_clamp_float(lean, 0.3f), 0.3, 1e-6);
	CHECK_NEAR(sfvr_settings_clamp_float(lean, 0.0f), 0.15, 1e-6);
	CHECK_NEAR(sfvr_settings_clamp_float(lean, 9.0f), 0.5, 1e-6);
	CHECK_NEAR(sfvr_settings_clamp_float(lean, -9.0f), 0.15, 1e-6);
	CHECK_NEAR(sfvr_settings_clamp_float(lean, NAN), 0.35, 1e-6); /* NaN -> default */
	CHECK_NEAR(sfvr_settings_clamp_float(lean, INFINITY), 0.5, 1e-6);
	CHECK_NEAR(sfvr_settings_clamp_float(NULL, 77.0f), 77.0, 1e-6);
	CHECK_NEAR(sfvr_settings_clamp_float(sfvr_settings_find("haptics"), 1.5f), 1.0, 1e-9);
	CHECK_NEAR(sfvr_settings_clamp_float(sfvr_settings_find("hud_scale"), 10.0f), 50.0, 1e-9);
	CHECK_NEAR(sfvr_settings_clamp_float(sfvr_settings_find("resolution_scale"), 3.0f), 1.5, 1e-9);
}

static void test_parse_enum(void)
{
	const sfvr_setting_key *turn = sfvr_settings_find("turn_mode");
	const sfvr_setting_key *hand = sfvr_settings_find("dominant_hand");
	const sfvr_setting_key *hud = sfvr_settings_find("hud_mode");

	CHECK_INT(sfvr_settings_parse_enum(turn, "snap"), 0);
	CHECK_INT(sfvr_settings_parse_enum(turn, "smooth"), 1);
	CHECK_INT(sfvr_settings_parse_enum(turn, "off"), 2);
	CHECK_INT(sfvr_settings_parse_enum(turn, "SMOOTH"), 1);
	CHECK_INT(sfvr_settings_parse_enum(turn, "Off"), 2);
	CHECK_INT(sfvr_settings_parse_enum(turn, "  smooth \n"), 1);
	CHECK_INT(sfvr_settings_parse_enum(turn, "bogus"), 0);   /* fallback: the default */
	CHECK_INT(sfvr_settings_parse_enum(turn, ""), 0);
	CHECK_INT(sfvr_settings_parse_enum(turn, NULL), 0);
	CHECK_INT(sfvr_settings_parse_enum(turn, "smoot"), 0);   /* no prefix match */
	CHECK_INT(sfvr_settings_parse_enum(turn, "smoothx"), 0);
	CHECK_INT(sfvr_settings_parse_enum(hand, "LEFT"), 1);
	CHECK_INT(sfvr_settings_parse_enum(hand, "middle"), 0);
	CHECK_INT(sfvr_settings_parse_enum(hud, "leash"), 2);
	CHECK_INT(sfvr_settings_parse_enum(NULL, "x"), 0);
	CHECK_INT(sfvr_settings_enum_count(turn), 3);
	CHECK_INT(sfvr_settings_enum_count(sfvr_settings_find("roomscale")), 0);
	CHECK_INT(sfvr_settings_enum_count(NULL), 0);
}

static void test_parse_bool(void)
{
	static const char *const yes[] = { "1", "true", "yes", "on", "TRUE", "Yes", "ON", " true ", "tRuE" };
	static const char *const no[] = { "0", "false", "no", "off", "FALSE", "No", "OFF", "\toff\n" };
	static const char *const junk[] = { "", "2", "maybe", "truee", "ye", "of", " " };
	size_t i;

	for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
	{
		CHECK_INT(sfvr_settings_parse_bool(yes[i], 0), 1);
		CHECK_INT(sfvr_settings_parse_bool(yes[i], 1), 1);
	}
	for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
	{
		CHECK_INT(sfvr_settings_parse_bool(no[i], 1), 0);
		CHECK_INT(sfvr_settings_parse_bool(no[i], 0), 0);
	}
	for (i = 0; i < sizeof(junk) / sizeof(junk[0]); i++)
	{
		CHECK_INT(sfvr_settings_parse_bool(junk[i], 1), 1);
		CHECK_INT(sfvr_settings_parse_bool(junk[i], 0), 0);
	}
	CHECK_INT(sfvr_settings_parse_bool(NULL, 1), 1);
	CHECK_INT(sfvr_settings_parse_bool(NULL, 0), 0);
}

static void test_env_name(void)
{
	char buf[64];

	CHECK_INT(sfvr_settings_env_name("HALO", "snap_angle", buf, sizeof(buf)), 1);
	CHECK_STR(buf, "HALO_VR_SNAP_ANGLE");
	CHECK_INT(sfvr_settings_env_name("gdx", "turn_mode", buf, sizeof(buf)), 1);
	CHECK_STR(buf, "GDX_VR_TURN_MODE");
	CHECK_INT(sfvr_settings_env_name("SFX", "follow_vehicle_rotation", buf, sizeof(buf)), 1);
	CHECK_STR(buf, "SFX_VR_FOLLOW_VEHICLE_ROTATION");
	CHECK_INT(sfvr_settings_env_name("DFU", "hud-Scale.2", buf, sizeof(buf)), 1);
	CHECK_STR(buf, "DFU_VR_HUD_SCALE_2");
	/* exactly fits: 4 + 4 + 10 = 18 chars + NUL */
	CHECK_INT(sfvr_settings_env_name("HALO", "snap_angle", buf, 19), 1);
	CHECK_STR(buf, "HALO_VR_SNAP_ANGLE");
	/* one byte short: failure, empty string */
	CHECK_INT(sfvr_settings_env_name("HALO", "snap_angle", buf, 18), 0);
	CHECK_STR(buf, "");
	CHECK_INT(sfvr_settings_env_name("HALO", "snap_angle", buf, 1), 0);
	CHECK_STR(buf, "");
	CHECK_INT(sfvr_settings_env_name("HALO", "snap_angle", buf, 0), 0);
	CHECK_INT(sfvr_settings_env_name(NULL, "snap_angle", buf, sizeof(buf)), 0);
	CHECK_INT(sfvr_settings_env_name("HALO", NULL, buf, sizeof(buf)), 0);
	CHECK_INT(sfvr_settings_env_name("HALO", "x", NULL, 10), 0);
}

int main(void)
{
	RUN(test_schema);
	RUN(test_all_keys_present);
	RUN(test_table_consistent);
	RUN(test_standard_defaults);
	RUN(test_clamp);
	RUN(test_parse_enum);
	RUN(test_parse_bool);
	RUN(test_env_name);
	return SFVR_TEST_RESULT("test_settings");
}
