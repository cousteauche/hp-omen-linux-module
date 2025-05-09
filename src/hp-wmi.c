// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HP WMI hotkeys
 *
 * Copyright (C) 2008 Red Hat <mjg@redhat.com>
 * Copyright (C) 2010, 2011 Anssi Hannula <anssi.hannula@iki.fi>
 *
 * Portions based on wistron_btns.c:
 * Copyright (C) 2005 Miloslav Trmac <mitr@volny.cz>
 * Copyright (C) 2005 Bernhard Rosenkraenzer <bero@arklinux.org>
 * Copyright (C) 2005 Dmitry Torokhov <dtor@mail.ru>
 *
 * RGB Four-Zone keyboard support adapted from community forks.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>      // Standard include
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include <linux/power_supply.h>
#include <linux/rfkill.h>
#include <linux/string.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/ctype.h> // For isxdigit

MODULE_AUTHOR("Matthew Garrett <mjg59@srcf.ucam.org>, Community Contributors");
MODULE_DESCRIPTION("HP laptop WMI driver with RGB keyboard support");
MODULE_LICENSE("GPL");

MODULE_ALIAS("wmi:95F24279-4D7B-4334-9387-ACCDC67EF61C");
MODULE_ALIAS("wmi:5FB7F034-2C63-45E9-BE91-3D44E2C707E4");

#define HPWMI_EVENT_GUID "95F24279-4D7B-4334-9387-ACCDC67EF61C"
#define HPWMI_BIOS_GUID "5FB7F034-2C63-45E9-BE91-3D44E2C707E4"

#define HP_OMEN_EC_THERMAL_PROFILE_FLAGS_OFFSET 0x62
#define HP_OMEN_EC_THERMAL_PROFILE_TIMER_OFFSET 0x63
#define HP_OMEN_EC_THERMAL_PROFILE_OFFSET 0x95

#define HP_FAN_SPEED_AUTOMATIC	 0x00
#define HP_POWER_LIMIT_DEFAULT	 0x00
#define HP_POWER_LIMIT_NO_CHANGE 0xFF

#define ACPI_AC_CLASS "ac_adapter"

#define zero_if_sup(tmp) (zero_insize_support?0:sizeof(tmp))

static const char * const omen_thermal_profile_boards[] = {
	"84DA", "84DB", "84DC", "8574", "8575", "860A", "87B5", "8572", "8573",
	"8600", "8601", "8602", "8605", "8606", "8607", "8746", "8747", "8749",
	"874A", "8603", "8604", "8748", "886B", "886C", "878A", "878B", "878C",
	"88C8", "88CB", "8786", "8787", "8788", "88D1", "88D2", "88F4", "88FD",
	"88F5", "88F6", "88F7", "88FE", "88FF", "8900", "8901", "8902", "8912",
	"8917", "8918", "8949", "894A", "89EB", "8BAD", "8A42", "8A15"
};

static const char * const omen_thermal_profile_force_v0_boards[] = {
	"8607", "8746", "8747", "8749", "874A", "8748"
};

static const char * const omen_timed_thermal_profile_boards[] = {
	"8BAD", "8A42", "8A15"
};

static const char * const victus_thermal_profile_boards[] = { "8A25" };
static const char * const victus_s_thermal_profile_boards[] = { "8C9C" };

enum hp_wmi_radio {
	HPWMI_WIFI	= 0x0, HPWMI_BLUETOOTH	= 0x1,
	HPWMI_WWAN	= 0x2, HPWMI_GPS	= 0x3,
};

enum hp_wmi_event_ids {
	HPWMI_DOCK_EVENT		= 0x01, HPWMI_PARK_HDD			= 0x02,
	HPWMI_SMART_ADAPTER		= 0x03, HPWMI_BEZEL_BUTTON		= 0x04,
	HPWMI_WIRELESS			= 0x05, HPWMI_CPU_BATTERY_THROTTLE	= 0x06,
	HPWMI_LOCK_SWITCH		= 0x07, HPWMI_LID_SWITCH		= 0x08,
	HPWMI_SCREEN_ROTATION		= 0x09, HPWMI_COOLSENSE_SYSTEM_MOBILE	= 0x0A,
	HPWMI_COOLSENSE_SYSTEM_HOT	= 0x0B, HPWMI_PROXIMITY_SENSOR		= 0x0C,
	HPWMI_BACKLIT_KB_BRIGHTNESS	= 0x0D, HPWMI_PEAKSHIFT_PERIOD		= 0x0F,
	HPWMI_BATTERY_CHARGE_PERIOD	= 0x10, HPWMI_SANITIZATION_MODE		= 0x17,
	HPWMI_CAMERA_TOGGLE		= 0x1A, HPWMI_OMEN_KEY			= 0x1D,
	HPWMI_SMART_EXPERIENCE_APP	= 0x21,
};

struct bios_args {
	u32 signature; u32 command; u32 commandtype;
	u32 datasize; u8 data[];
};

enum hp_wmi_commandtype {
	HPWMI_DISPLAY_QUERY		= 0x01, HPWMI_HDDTEMP_QUERY		= 0x02,
	HPWMI_ALS_QUERY			= 0x03, HPWMI_HARDWARE_QUERY		= 0x04,
	HPWMI_WIRELESS_QUERY		= 0x05, HPWMI_BATTERY_QUERY		= 0x07,
	HPWMI_BIOS_QUERY		= 0x09, HPWMI_FEATURE_QUERY		= 0x0b,
	HPWMI_HOTKEY_QUERY		= 0x0c, HPWMI_FEATURE2_QUERY		= 0x0d,
	HPWMI_WIRELESS2_QUERY		= 0x1b, HPWMI_POSTCODEERROR_QUERY	= 0x2a,
	HPWMI_SYSTEM_DEVICE_MODE	= 0x40, HPWMI_THERMAL_PROFILE_QUERY	= 0x4c,
	/* RGB Four-Zone Command Types (used with HPWMI_FOURZONE command) */
	HPWMI_FOURZONE_COLOR_GET_TYPE	= 2,
	HPWMI_FOURZONE_COLOR_SET_TYPE	= 3,
	HPWMI_FOURZONE_BRIGHT_GET_TYPE	= 4,
	HPWMI_FOURZONE_BRIGHT_SET_TYPE	= 5,
	HPWMI_FOURZONE_ANIM_GET_TYPE	= 6,
	HPWMI_FOURZONE_ANIM_SET_TYPE	= 7,
};

struct victus_power_limits { u8 pl1; u8 pl2; u8 pl4; u8 cpu_gpu_concurrent_limit; };
struct victus_gpu_power_modes { u8 ctgp_enable; u8 ppab_enable; u8 dstate; u8 gpu_slowdown_temp; };

enum hp_wmi_gm_commandtype {
	HPWMI_FAN_SPEED_GET_QUERY		= 0x11, HPWMI_SET_PERFORMANCE_MODE		= 0x1A,
	HPWMI_FAN_SPEED_MAX_GET_QUERY		= 0x26, HPWMI_FAN_SPEED_MAX_SET_QUERY		= 0x27,
	HPWMI_GET_SYSTEM_DESIGN_DATA		= 0x28, HPWMI_FAN_COUNT_GET_QUERY		= 0x10,
	HPWMI_GET_GPU_THERMAL_MODES_QUERY	= 0x21, HPWMI_SET_GPU_THERMAL_MODES_QUERY	= 0x22,
	HPWMI_SET_POWER_LIMITS_QUERY		= 0x29, HPWMI_VICTUS_S_FAN_SPEED_GET_QUERY	= 0x2D,
	HPWMI_FAN_SPEED_SET_QUERY		= 0x2E,
};

enum hp_wmi_command {
	HPWMI_READ	= 0x01, HPWMI_WRITE	= 0x02,
	HPWMI_ODM	= 0x03, HPWMI_GM	= 0x20008,
	HPWMI_FOURZONE	= 0x20009, /* Main command for RGB */
};

enum hp_wmi_hardware_mask { HPWMI_DOCK_MASK = 0x01, HPWMI_TABLET_MASK = 0x04 };
struct bios_return { u32 sigpass; u32 return_code; };

enum hp_return_value {
	HPWMI_RET_WRONG_SIGNATURE	= 0x02, HPWMI_RET_UNKNOWN_COMMAND	= 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE	= 0x04, HPWMI_RET_INVALID_PARAMETERS	= 0x05,
};

enum hp_wireless2_bits {
	HPWMI_POWER_STATE	= 0x01, HPWMI_POWER_SOFT	= 0x02,
	HPWMI_POWER_BIOS	= 0x04, HPWMI_POWER_HARD	= 0x08,
	HPWMI_POWER_FW_OR_HW	= HPWMI_POWER_BIOS | HPWMI_POWER_HARD,
};

enum hp_thermal_profile_omen_v0 {
	HP_OMEN_V0_THERMAL_PROFILE_DEFAULT     = 0x00, HP_OMEN_V0_THERMAL_PROFILE_PERFORMANCE = 0x01,
	HP_OMEN_V0_THERMAL_PROFILE_COOL        = 0x02,
};
enum hp_thermal_profile_omen_v1 {
	HP_OMEN_V1_THERMAL_PROFILE_DEFAULT	= 0x30, HP_OMEN_V1_THERMAL_PROFILE_PERFORMANCE	= 0x31,
	HP_OMEN_V1_THERMAL_PROFILE_COOL		= 0x50,
};
enum hp_thermal_profile_omen_flags {
	HP_OMEN_EC_FLAGS_TURBO		= 0x04, HP_OMEN_EC_FLAGS_NOTIMER	= 0x02,
	HP_OMEN_EC_FLAGS_JUSTSET	= 0x01,
};
enum hp_thermal_profile_victus {
	HP_VICTUS_THERMAL_PROFILE_DEFAULT	= 0x00, HP_VICTUS_THERMAL_PROFILE_PERFORMANCE	= 0x01,
	HP_VICTUS_THERMAL_PROFILE_QUIET		= 0x03,
};
enum hp_thermal_profile_victus_s {
	HP_VICTUS_S_THERMAL_PROFILE_DEFAULT	= 0x00, HP_VICTUS_S_THERMAL_PROFILE_PERFORMANCE	= 0x01,
};
enum hp_thermal_profile {
	HP_THERMAL_PROFILE_PERFORMANCE	= 0x00, HP_THERMAL_PROFILE_DEFAULT	= 0x01,
	HP_THERMAL_PROFILE_COOL		= 0x02, HP_THERMAL_PROFILE_QUIET	= 0x03,
};

#define IS_HWBLOCKED(x) ((x & HPWMI_POWER_FW_OR_HW) != HPWMI_POWER_FW_OR_HW)
#define IS_SWBLOCKED(x) !(x & HPWMI_POWER_SOFT)

struct bios_rfkill2_device_state {
	u8 radio_type; u8 bus_type; u16 vendor_id; u16 product_id;
	u16 subsys_vendor_id; u16 subsys_product_id; u8 rfkill_id; u8 power;
	u8 unknown[4];
};
#define HPWMI_MAX_RFKILL2_DEVICES	7
struct bios_rfkill2_state {
	u8 unknown[7]; u8 count; u8 pad[8];
	struct bios_rfkill2_device_state device[HPWMI_MAX_RFKILL2_DEVICES];
};

static const struct key_entry hp_wmi_keymap[] = {
	{ KE_KEY, 0x02,    { KEY_BRIGHTNESSUP } }, { KE_KEY, 0x03,    { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x270,   { KEY_MICMUTE } },      { KE_KEY, 0x20e6,  { KEY_PROG1 } },
	{ KE_KEY, 0x20e8,  { KEY_MEDIA } },        { KE_KEY, 0x2142,  { KEY_MEDIA } },
	{ KE_KEY, 0x213b,  { KEY_INFO } },         { KE_KEY, 0x2169,  { KEY_ROTATE_DISPLAY } },
	{ KE_KEY, 0x216a,  { KEY_SETUP } },        { KE_IGNORE, 0x21a4,  },
	{ KE_IGNORE, 0x121a4, },                   { KE_KEY, 0x21a5,  { KEY_PROG2 } },
	{ KE_KEY, 0x21a7,  { KEY_FN_ESC } },       { KE_KEY, 0x21a8,  { KEY_PROG2 } },
	{ KE_KEY, 0x21a9,  { KEY_TOUCHPAD_OFF } }, { KE_KEY, 0x121a9, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, 0x231b,  { KEY_HELP } },         { KE_END, 0 }
};

static DEFINE_MUTEX(active_platform_profile_lock);

static struct input_dev *hp_wmi_input_dev;
static struct input_dev *camera_shutter_input_dev;
static struct platform_device *hp_wmi_platform_dev;
static struct device *platform_profile_device;
static struct notifier_block platform_power_source_nb;
static enum platform_profile_option active_platform_profile;
static bool platform_profile_support;
static bool zero_insize_support;

static struct rfkill *wifi_rfkill;
static struct rfkill *bluetooth_rfkill;
static struct rfkill *wwan_rfkill;

struct rfkill2_device { u8 id; int num; struct rfkill *rfkill; };
static int rfkill2_count;
static struct rfkill2_device rfkill2[HPWMI_MAX_RFKILL2_DEVICES];

static const char * const tablet_chassis_types[] = { "30", "31", "32" };
#define DEVICE_MODE_TABLET	0x06

/* RGB Four-Zone Structures & Globals */
#define FOURZONE_COUNT 4
struct color_platform { u8 b; u8 g; u8 r; } __packed;
struct platform_zone {
	u8 offset; struct device_attribute *attr;
	struct color_platform colors; char *name_ptr;
};

static struct device_attribute *zone_dev_attrs_rgb;
static struct attribute **zone_attrs_rgb;
static struct platform_zone *zone_data_rgb;
static struct attribute_group zone_attribute_group_rgb = { .name = "rgb_zones", };
/* End RGB Globals */

static inline int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096) { return -EINVAL; }
	if (outsize > 1024) { return 5; }
	if (outsize > 128) { return 4; }
	if (outsize > 4) { return 3; }
	if (outsize > 0) { return 2; }
	return 1;
}

static int hp_wmi_perform_query(int query, enum hp_wmi_command command,
				void *buffer, int insize, int outsize)
{
	struct acpi_buffer input, output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct bios_return *bios_return;
	union acpi_object *obj = NULL;
	struct bios_args *args = NULL;
	int mid, actual_insize, actual_outsize;
	size_t bios_args_size;
	int ret;

	mid = encode_outsize_for_pvsz(outsize);
	if (WARN_ON(mid < 0))
		return mid;

	actual_insize = max(insize, 128);
	bios_args_size = struct_size(args, data, actual_insize);
	args = kmalloc(bios_args_size, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	input.length = bios_args_size;
	input.pointer = args;

	args->signature = 0x55434553; args->command = command;
	args->commandtype = query; args->datasize = insize;
	memset(args->data, 0, actual_insize); /* Initialize to zero */
	if (insize > 0 && buffer) /* Copy input data if provided */
		memcpy(args->data, buffer, flex_array_size(args, data, insize));

	ret = wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	if (ret)
		goto out_free;

	obj = output.pointer;
	if (!obj) { ret = -EINVAL; goto out_free; }

	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_warn("query 0x%x cmd 0x%x: invalid object type 0x%x\n", query, command, obj->type);
		ret = -EINVAL; goto out_free;
	}
	if (obj->buffer.length < sizeof(*bios_return)) {
		pr_warn("query 0x%x cmd 0x%x: short buffer len %u\n", query, command, obj->buffer.length);
		ret = -EIO; goto out_free;
	}

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;

	if (ret && ret != HPWMI_RET_UNKNOWN_COMMAND && ret != HPWMI_RET_UNKNOWN_CMDTYPE)
		pr_warn("query 0x%x cmd 0x%x: error 0x%x\n", query, command, ret);
	if (ret) /* If WMI reported an error, don't process output */
		goto out_free;

	/* Ignore output data of zero size or if no buffer provided */
	if (!outsize || !buffer)
		goto out_free;

	actual_outsize = min_t(int, outsize, (int)(obj->buffer.length - sizeof(*bios_return)));
	memcpy(buffer, obj->buffer.pointer + sizeof(*bios_return), actual_outsize);
	if (outsize > actual_outsize) /* Zero out remaining part of user buffer */
		memset(buffer + actual_outsize, 0, outsize - actual_outsize);
out_free:
	kfree(obj); kfree(args); return ret;
}

static int hp_wmi_get_fan_count_userdefine_trigger(void)
{
	u8 fan_data[4] = {}; int ret;
	ret = hp_wmi_perform_query(HPWMI_FAN_COUNT_GET_QUERY, HPWMI_GM, &fan_data, sizeof(u8), sizeof(fan_data));
	return (ret != 0) ? -EINVAL : fan_data[0];
}

static int hp_wmi_get_fan_speed(int fan)
{
	u8 fsh, fsl; char fan_data[4] = { (u8)fan, 0, 0, 0 }; int ret;
	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_GET_QUERY, HPWMI_GM, &fan_data, sizeof(char), sizeof(fan_data));
	if (ret != 0) return -EINVAL;
	fsh = fan_data[2]; fsl = fan_data[3]; return (fsh << 8) | fsl;
}

static int hp_wmi_get_fan_speed_victus_s(int fan)
{
	u8 fan_data[128] = {}; int ret; u8 fan_idx_buf = (u8)fan;
	if (fan < 0) return -EINVAL;
	ret = hp_wmi_perform_query(HPWMI_VICTUS_S_FAN_SPEED_GET_QUERY, HPWMI_GM, &fan_idx_buf, sizeof(fan_idx_buf), sizeof(fan_data));
	return (ret != 0) ? -EINVAL : (fan_data[fan] * 100);
}

static int hp_wmi_read_int(int query)
{
	int val = 0, ret;
	ret = hp_wmi_perform_query(query, HPWMI_READ, &val, zero_if_sup(val), sizeof(val));
	return ret ? (ret < 0 ? ret : -EINVAL) : val;
}

static int hp_wmi_get_dock_state(void)
{
	int state = hp_wmi_read_int(HPWMI_HARDWARE_QUERY);
	return (state < 0) ? state : !!(state & HPWMI_DOCK_MASK);
}

static int hp_wmi_get_tablet_mode(void)
{
	char sdm[4] = {0}; const char *ct; bool tf; int ret;
	ct = dmi_get_system_info(DMI_CHASSIS_TYPE); if (!ct) return -ENODEV;
	tf = match_string(tablet_chassis_types, ARRAY_SIZE(tablet_chassis_types), ct) >= 0;
	if (!tf) return -ENODEV;
	ret = hp_wmi_perform_query(HPWMI_SYSTEM_DEVICE_MODE, HPWMI_READ, sdm, zero_if_sup(sdm), sizeof(sdm));
	if (ret < 0)
		return ret;
	if (ret > 0)
		return -EIO;
	return (sdm[0] == DEVICE_MODE_TABLET);
}

static int omen_thermal_profile_set(int mode)
{
	char buffer[2] = {-1, (u8)mode}; int ret;
	ret = hp_wmi_perform_query(HPWMI_SET_PERFORMANCE_MODE, HPWMI_GM, &buffer, sizeof(buffer), 0);
	return ret ? (ret < 0 ? ret : -EINVAL) : mode;
}

static bool is_omen_thermal_profile(void)
{
	const char *bn = dmi_get_system_info(DMI_BOARD_NAME);
	return bn ? (match_string(omen_thermal_profile_boards, ARRAY_SIZE(omen_thermal_profile_boards), bn) >= 0) : false;
}

static int omen_get_thermal_policy_version(void)
{
	u8 buf[8] = {0}; int ret; const char *bn = dmi_get_system_info(DMI_BOARD_NAME);
	if (bn && match_string(omen_thermal_profile_force_v0_boards, ARRAY_SIZE(omen_thermal_profile_force_v0_boards), bn) >= 0)
		return 0;
	ret = hp_wmi_perform_query(HPWMI_GET_SYSTEM_DESIGN_DATA, HPWMI_GM, &buf, sizeof(buf), sizeof(buf));
	return ret ? (ret < 0 ? ret : -EINVAL) : buf[3];
}

static int omen_thermal_profile_get(void) { u8 d; int r = ec_read(HP_OMEN_EC_THERMAL_PROFILE_OFFSET, &d); return r ? r : d; }


static int hp_wmi_fan_speed_max_set(int en)
{
	int ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, HPWMI_GM, &en, sizeof(en), 0);
	return ret ? (ret < 0 ? ret : -EINVAL) : en;
}

static int hp_wmi_fan_speed_reset(void)
{
	u8 fs[2] = {HP_FAN_SPEED_AUTOMATIC, HP_FAN_SPEED_AUTOMATIC};
	return hp_wmi_perform_query(HPWMI_FAN_SPEED_SET_QUERY, HPWMI_GM, &fs, sizeof(fs), 0);
}

static int hp_wmi_fan_speed_max_reset(void) { int r = hp_wmi_fan_speed_max_set(0); return r ? r : hp_wmi_fan_speed_reset(); }


static int hp_wmi_fan_speed_max_get(void)
{
	int val = 0, ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, HPWMI_GM, &val, zero_if_sup(val), sizeof(val));
	return ret ? (ret < 0 ? ret : -EINVAL) : val;
}

static int __init hp_wmi_bios_2008_later(void)
{
	int s=0, r=hp_wmi_perform_query(HPWMI_FEATURE_QUERY,HPWMI_READ,&s,zero_if_sup(s),sizeof(s));
	return !r ? 1 : (r == HPWMI_RET_UNKNOWN_CMDTYPE ? 0 : -ENXIO);
}
static int __init hp_wmi_bios_2009_later(void)
{
	u8 s[128]; int r=hp_wmi_perform_query(HPWMI_FEATURE2_QUERY,HPWMI_READ,&s,zero_if_sup(s),sizeof(s));
	return !r ? 1 : (r == HPWMI_RET_UNKNOWN_CMDTYPE ? 0 : -ENXIO);
}
static int __init hp_wmi_enable_hotkeys(void)
{
	int v=0x6e, r=hp_wmi_perform_query(HPWMI_BIOS_QUERY,HPWMI_WRITE,&v,sizeof(v),0);
	return r <= 0 ? r : -EINVAL;
}

static int hp_wmi_set_block(void *data, bool blocked)
{
	enum hp_wmi_radio r = (long)data; int q = BIT(r + 8) | ((!blocked) << r);
	int ret = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE, &q, sizeof(q), 0);
	return ret <= 0 ? ret : -EINVAL;
}
static const struct rfkill_ops hp_wmi_rfkill_ops = { .set_block = hp_wmi_set_block };

static bool hp_wmi_get_sw_state(enum hp_wmi_radio r)
{
	int m=0x200<<(r*8), w=hp_wmi_read_int(HPWMI_WIRELESS_QUERY);
	WARN_ONCE(w<0, "err HPWMI_WIRELESS_QUERY SW"); return !(w & m);
}
static bool hp_wmi_get_hw_state(enum hp_wmi_radio r)
{
	int m=0x800<<(r*8), w=hp_wmi_read_int(HPWMI_WIRELESS_QUERY);
	WARN_ONCE(w<0, "err HPWMI_WIRELESS_QUERY HW"); return !(w & m);
}

static int hp_wmi_rfkill2_set_block(void *data, bool blocked)
{
	int id=(long)data; char b[4]={0x01,0x00,(u8)id,!blocked};
	int ret = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_WRITE, b, sizeof(b), 0);
	return ret <= 0 ? ret : -EINVAL;
}
static const struct rfkill_ops hp_wmi_rfkill2_ops = { .set_block = hp_wmi_rfkill2_set_block };

static int hp_wmi_rfkill2_refresh(void)
{
	struct bios_rfkill2_state s; int err, i;
	err=hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY,HPWMI_READ,&s,zero_if_sup(s),sizeof(s));
	if(err) return err;
	for(i=0; i<rfkill2_count; i++){
		int num=rfkill2[i].num; struct bios_rfkill2_device_state *ds;
		if(num>=s.count || rfkill2[i].id==0xFF) continue;
		ds=&s.device[num];
		if(ds->rfkill_id!=rfkill2[i].id){pr_warn("rfkill config changed\n"); continue;}
		if(rfkill2[i].rfkill) rfkill_set_states(rfkill2[i].rfkill,IS_SWBLOCKED(ds->power),IS_HWBLOCKED(ds->power));
	} return 0;
}

static ssize_t display_show(struct device *dev, struct device_attribute *attr, char *buf)
{ int v=hp_wmi_read_int(HPWMI_DISPLAY_QUERY); return v<0?v:sysfs_emit(buf,"%d\n",v); }
static ssize_t hddtemp_show(struct device *dev, struct device_attribute *attr, char *buf)
{ int v=hp_wmi_read_int(HPWMI_HDDTEMP_QUERY); return v<0?v:sysfs_emit(buf,"%d\n",v); }
static ssize_t als_show(struct device *dev, struct device_attribute *attr, char *buf)
{ int v=hp_wmi_read_int(HPWMI_ALS_QUERY); return v<0?v:sysfs_emit(buf,"%d\n",v); }
static ssize_t dock_show(struct device *dev, struct device_attribute *attr, char *buf)
{ int v=hp_wmi_get_dock_state(); return v<0?v:sysfs_emit(buf,"%d\n",v); }
static ssize_t tablet_show(struct device *dev, struct device_attribute *attr, char *buf)
{ int v=hp_wmi_get_tablet_mode(); return v<0?v:sysfs_emit(buf,"%d\n",v); }
static ssize_t postcode_show(struct device *dev, struct device_attribute *attr, char *buf)
{ int v=hp_wmi_read_int(HPWMI_POSTCODEERROR_QUERY); return v<0?v:sysfs_emit(buf,"0x%x\n",v); }

static ssize_t als_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	u32 tmp; int ret=kstrtou32(buf,10,&tmp); if(ret)return ret;
	ret=hp_wmi_perform_query(HPWMI_ALS_QUERY,HPWMI_WRITE,&tmp,sizeof(tmp),0);
	if (ret) {
		return ret < 0 ? ret : -EINVAL;
	}
	return count;
}
static ssize_t postcode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	u32 tmp=1; bool cl; int ret=kstrtobool(buf,&cl); if(ret)return ret;
	if(!cl)return -EINVAL;
	ret=hp_wmi_perform_query(HPWMI_POSTCODEERROR_QUERY,HPWMI_WRITE,&tmp,sizeof(tmp),0);
	if (ret) {
		return ret < 0 ? ret : -EINVAL;
	}
	return count;
}

static int camera_shutter_input_setup(void)
{
	int err; camera_shutter_input_dev=input_allocate_device();
	if(!camera_shutter_input_dev)return -ENOMEM;
	camera_shutter_input_dev->name="HP WMI camera shutter"; camera_shutter_input_dev->phys="wmi/input1";
	camera_shutter_input_dev->id.bustype=BUS_HOST;
	__set_bit(EV_SW,camera_shutter_input_dev->evbit); __set_bit(SW_CAMERA_LENS_COVER,camera_shutter_input_dev->swbit);
	err=input_register_device(camera_shutter_input_dev);
	if(err){input_free_device(camera_shutter_input_dev); camera_shutter_input_dev=NULL; return err;}
	return 0;
}

static DEVICE_ATTR_RO(display); static DEVICE_ATTR_RO(hddtemp); static DEVICE_ATTR_RW(als);
static DEVICE_ATTR_RO(dock); static DEVICE_ATTR_RO(tablet); static DEVICE_ATTR_RW(postcode);
static struct attribute *hp_wmi_attrs[]={&dev_attr_display.attr,&dev_attr_hddtemp.attr,&dev_attr_als.attr,
	&dev_attr_dock.attr,&dev_attr_tablet.attr,&dev_attr_postcode.attr,NULL,};
ATTRIBUTE_GROUPS(hp_wmi);

static void hp_wmi_notify(union acpi_object *obj, void *context)
{
	u32 eid, ed, *loc; int kc;
	if(!obj || obj->type!=ACPI_TYPE_BUFFER){pr_info("Unk WMI evt type %d\n",obj?obj->type:-1); return;}
	loc=(u32*)obj->buffer.pointer;
	if(obj->buffer.length==8){eid=loc[0];ed=loc[1];}
	else if(obj->buffer.length==16){eid=loc[0];ed=loc[2];}
	else {pr_info("Unk WMI evt len %d\n",obj->buffer.length); return;}

	switch(eid){
	case HPWMI_DOCK_EVENT:
		if(test_bit(SW_DOCK,hp_wmi_input_dev->swbit)) input_report_switch(hp_wmi_input_dev,SW_DOCK,hp_wmi_get_dock_state());
		if(test_bit(SW_TABLET_MODE,hp_wmi_input_dev->swbit)) input_report_switch(hp_wmi_input_dev,SW_TABLET_MODE,hp_wmi_get_tablet_mode());
		input_sync(hp_wmi_input_dev);
		break;
	case HPWMI_BEZEL_BUTTON:
		kc=hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		if(kc<0) {
			break;
		}
		if(!sparse_keymap_report_event(hp_wmi_input_dev,kc,1,true)) {
			pr_info("Unk Bezel key 0x%x\n",kc);
		}
		break;
	case HPWMI_OMEN_KEY:
		if(ed && ed!=0xFFFFFFFF) {
			kc=ed;
		} else {
			kc=hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		}
		if(kc<0) {
			break;
		}
		if(!sparse_keymap_report_event(hp_wmi_input_dev,kc,1,true)) {
			pr_info("Unk Omen key 0x%x(ed:0x%x)\n",kc,ed);
		}
		break;
	case HPWMI_WIRELESS:
		if(rfkill2_count) {
			hp_wmi_rfkill2_refresh();
		} else {
			if(wifi_rfkill)rfkill_set_states(wifi_rfkill,hp_wmi_get_sw_state(HPWMI_WIFI),hp_wmi_get_hw_state(HPWMI_WIFI));
			if(bluetooth_rfkill)rfkill_set_states(bluetooth_rfkill,hp_wmi_get_sw_state(HPWMI_BLUETOOTH),hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
			if(wwan_rfkill)rfkill_set_states(wwan_rfkill,hp_wmi_get_sw_state(HPWMI_WWAN),hp_wmi_get_hw_state(HPWMI_WWAN));
		}
		break;
	case HPWMI_CAMERA_TOGGLE:
		if(!camera_shutter_input_dev && camera_shutter_input_setup()) {
			pr_err("Fail cam shutter setup\n");
			break; 
		}
		if(camera_shutter_input_dev) {
			if(ed==0xff) {
				input_report_switch(camera_shutter_input_dev,SW_CAMERA_LENS_COVER,1);
			} else if(ed==0xfe) {
				input_report_switch(camera_shutter_input_dev,SW_CAMERA_LENS_COVER,0);
			} else {
				pr_warn("Unk cam shutter state 0x%x\n",ed);
			}
			input_sync(camera_shutter_input_dev);
		}
		break;
	case HPWMI_PARK_HDD: break;
	case HPWMI_SMART_ADAPTER: pr_debug("Smart Adapter evt:0x%x\n",ed); break;
	case HPWMI_CPU_BATTERY_THROTTLE: pr_info("CPU throttle evt (0x%x)\n",ed); break;
	case HPWMI_LOCK_SWITCH: pr_debug("Lock switch evt:0x%x\n",ed); break;
	case HPWMI_LID_SWITCH: pr_debug("Lid evt:0x%x\n",ed); break;
	case HPWMI_SCREEN_ROTATION: pr_debug("Screen rot evt:0x%x\n",ed); break;
	case HPWMI_COOLSENSE_SYSTEM_MOBILE: pr_debug("Coolsense Mobile evt:0x%x\n",ed); break;
	case HPWMI_COOLSENSE_SYSTEM_HOT: pr_debug("Coolsense Hot evt:0x%x\n",ed); break;
	case HPWMI_PROXIMITY_SENSOR: pr_debug("Proximity evt:0x%x\n",ed); break;
	case HPWMI_BACKLIT_KB_BRIGHTNESS: pr_debug("KB backlight evt:0x%x\n",ed); break;
	case HPWMI_PEAKSHIFT_PERIOD: pr_debug("Peakshift evt:0x%x\n",ed); break;
	case HPWMI_BATTERY_CHARGE_PERIOD: pr_debug("Batt Charge Period evt:0x%x\n",ed); break;
	case HPWMI_SANITIZATION_MODE: pr_info("Sanitization evt:0x%x\n",ed); break;
	case HPWMI_SMART_EXPERIENCE_APP: pr_info("Smart Exp App evt:0x%x\n",ed); break;
	default:pr_info("Unk WMI evt_id:0x%x,data:0x%x\n",eid,ed); break;
	}
}

static int __init hp_wmi_input_setup(void)
{
	acpi_status status;
	int err, val_dock, val_tablet;

	hp_wmi_input_dev = input_allocate_device();
	if (!hp_wmi_input_dev)
		return -ENOMEM;

	hp_wmi_input_dev->name = "HP WMI hotkeys";
	hp_wmi_input_dev->phys = "wmi/input0";
	hp_wmi_input_dev->id.bustype = BUS_HOST;

	err = sparse_keymap_setup(hp_wmi_input_dev, hp_wmi_keymap, NULL);
	if (err)
		goto err_free_dev;

	__set_bit(EV_SW, hp_wmi_input_dev->evbit);

	val_dock = hp_wmi_get_dock_state();
	if (val_dock >= 0) {
		__set_bit(SW_DOCK, hp_wmi_input_dev->swbit);
		input_report_switch(hp_wmi_input_dev, SW_DOCK, val_dock);
	} else {
		pr_warn("Failed to get initial dock state: %d\n", val_dock);
	}

	val_tablet = hp_wmi_get_tablet_mode();
	if (val_tablet >= 0) {
		__set_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit);
		input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, val_tablet);
	} else if (val_tablet != -ENODEV) {
		pr_warn("Failed to get initial tablet mode: %d\n", val_tablet);
	}

	input_sync(hp_wmi_input_dev);

	if (!hp_wmi_bios_2009_later() && hp_wmi_bios_2008_later()) {
		err = hp_wmi_enable_hotkeys();
		if (err)
			pr_warn("Failed to enable hotkeys: %d\n", err);
	}

	status = wmi_install_notify_handler(HPWMI_EVENT_GUID, hp_wmi_notify, NULL);
	if (ACPI_FAILURE(status)) {
		/* MODYFIKACJA: Usunięcie acpi_status_to_errno, użycie -EIO */
		pr_err("Failed to install WMI notify handler (ACPI Status: 0x%x)\n", status);
		err = -EIO; 
		goto err_cleanup_keymap;
	}

	err = input_register_device(hp_wmi_input_dev);
	if (err)
		goto err_uninstall_notifier;

	return 0;

err_uninstall_notifier:
	wmi_remove_notify_handler(HPWMI_EVENT_GUID);
err_cleanup_keymap:
	/* sparse_keymap_free usunięte, bo nie ma go w nowszym API */
err_free_dev:
	input_free_device(hp_wmi_input_dev);
	hp_wmi_input_dev = NULL;
	return err;
}

static void hp_wmi_input_destroy(void)
{
	if (hp_wmi_input_dev) {
		wmi_remove_notify_handler(HPWMI_EVENT_GUID);
		input_unregister_device(hp_wmi_input_dev);
		hp_wmi_input_dev = NULL;
	}
}

static int __init hp_wmi_rfkill_setup(struct platform_device *device)
{
	int err, wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);
	if (wireless < 0)
		return wireless;

	err = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE, &wireless, sizeof(wireless), 0);
	if (err)
		pr_warn("Failed to acknowledge wireless query: %d\n", err); /* Nie zwracaj błędu, kontynuuj */

	if (wireless & 0x1) {
		wifi_rfkill = rfkill_alloc("hp-wifi", &device->dev, RFKILL_TYPE_WLAN,
					   &hp_wmi_rfkill_ops, (void *)(uintptr_t)HPWMI_WIFI);
		if (!wifi_rfkill) { err = -ENOMEM; goto reg_wifi_err; }
		rfkill_init_sw_state(wifi_rfkill, hp_wmi_get_sw_state(HPWMI_WIFI));
		rfkill_set_hw_state(wifi_rfkill, hp_wmi_get_hw_state(HPWMI_WIFI));
		err = rfkill_register(wifi_rfkill);
		if (err) goto reg_wifi_err;
	}
	if (wireless & 0x2) {
		bluetooth_rfkill = rfkill_alloc("hp-bluetooth", &device->dev, RFKILL_TYPE_BLUETOOTH,
						&hp_wmi_rfkill_ops, (void *)(uintptr_t)HPWMI_BLUETOOTH);
		if (!bluetooth_rfkill) { err = -ENOMEM; goto reg_bt_err; }
		rfkill_init_sw_state(bluetooth_rfkill, hp_wmi_get_sw_state(HPWMI_BLUETOOTH));
		rfkill_set_hw_state(bluetooth_rfkill, hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		err = rfkill_register(bluetooth_rfkill);
		if (err) goto reg_bt_err;
	}
	if (wireless & 0x4) {
		wwan_rfkill = rfkill_alloc("hp-wwan", &device->dev, RFKILL_TYPE_WWAN,
					   &hp_wmi_rfkill_ops, (void *)(uintptr_t)HPWMI_WWAN);
		if (!wwan_rfkill) { err = -ENOMEM; goto reg_wwan_err; }
		rfkill_init_sw_state(wwan_rfkill, hp_wmi_get_sw_state(HPWMI_WWAN));
		rfkill_set_hw_state(wwan_rfkill, hp_wmi_get_hw_state(HPWMI_WWAN));
		err = rfkill_register(wwan_rfkill);
		if (err) goto reg_wwan_err;
	}
	return 0;

reg_wwan_err: if(wwan_rfkill){rfkill_destroy(wwan_rfkill); wwan_rfkill=NULL;}
reg_bt_err: if(bluetooth_rfkill){rfkill_unregister(bluetooth_rfkill);rfkill_destroy(bluetooth_rfkill); bluetooth_rfkill=NULL;}
reg_wifi_err: if(wifi_rfkill){rfkill_unregister(wifi_rfkill);rfkill_destroy(wifi_rfkill); wifi_rfkill=NULL;}
	return err;
}

static int __init hp_wmi_rfkill2_setup(struct platform_device *device)
{
	struct bios_rfkill2_state s; int err,i;
	err=hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY,HPWMI_READ,&s,zero_if_sup(s),sizeof(s));
	if(err)return err<0?err:-EINVAL;
	if(s.count>HPWMI_MAX_RFKILL2_DEVICES){pr_warn("rfkill2 count %d > max\n",s.count);return -EINVAL;}
	for(i=0;i<s.count;i++){
		struct rfkill *rfk; enum rfkill_type t; const char *n;
		switch(s.device[i].radio_type){
		case HPWMI_WIFI:t=RFKILL_TYPE_WLAN;n="hp-wifi";break;
		case HPWMI_BLUETOOTH:t=RFKILL_TYPE_BLUETOOTH;n="hp-bluetooth";break;
		case HPWMI_WWAN:t=RFKILL_TYPE_WWAN;n="hp-wwan";break;
		case HPWMI_GPS:t=RFKILL_TYPE_GPS;n="hp-gps";break;
		default:pr_warn("unk rfkill2 type 0x%x\n",s.device[i].radio_type);continue;}
		if(!s.device[i].vendor_id&&!s.device[i].product_id&&s.device[i].rfkill_id==0xFF)continue;
		rfk=rfkill_alloc(n,&device->dev,t,&hp_wmi_rfkill2_ops,(void*)(long)i); if(!rfk){err=-ENOMEM;goto fail;}
		rfkill2[rfkill2_count].id=s.device[i].rfkill_id; rfkill2[rfkill2_count].num=i; rfkill2[rfkill2_count].rfkill=rfk;
		rfkill_init_sw_state(rfk,IS_SWBLOCKED(s.device[i].power)); rfkill_set_hw_state(rfk,IS_HWBLOCKED(s.device[i].power));
		if(!(s.device[i].power&HPWMI_POWER_BIOS))pr_info("rfkill2 %s (0x%x) BIOS blocked\n",n,s.device[i].radio_type);
		err=rfkill_register(rfk); if(err){rfkill_destroy(rfk);rfkill2[rfkill2_count].rfkill=NULL;goto fail;}
		rfkill2_count++;
	} return 0;
fail: for(;rfkill2_count>0;rfkill2_count--){if(rfkill2[rfkill2_count-1].rfkill){
	rfkill_unregister(rfkill2[rfkill2_count-1].rfkill);rfkill_destroy(rfkill2[rfkill2_count-1].rfkill);
	rfkill2[rfkill2_count-1].rfkill=NULL;}} return err;
}

/* RGB Four-Zone functions */
static int parse_rgb(const char *buf, struct color_platform *c) {
	unsigned int r_val, g_val, b_val;
	char clean_buf[7]; /* RRGGBB + null */
	size_t len = strnlen(buf, sizeof(clean_buf) -1 ); /* Max 6 chars + null */

	if (len != 6) { /* Expect exactly 6 hex characters */
		pr_warn("RGB: Invalid length for color string: %zu (expected 6)\n", len);
		return -EINVAL;
	}
	memcpy(clean_buf, buf, len);
	clean_buf[len] = '\0';

	if (sscanf(clean_buf, "%02x%02x%02x", &r_val, &g_val, &b_val) == 3) {
		c->r = r_val; c->g = g_val; c->b = b_val;
		pr_debug("RGB parsed: R:%02x G:%02x B:%02x\n", c->r, c->g, c->b);
		return 0;
	}
	pr_warn("Invalid RGB string format: %s\n", clean_buf);
	return -EINVAL;
}

static struct platform_zone *match_zone_by_attr_rgb(const struct device_attribute *target_attr){
	int i;
	if (!zone_data_rgb || !zone_dev_attrs_rgb)
		return NULL;
	for (i = 0; i < FOURZONE_COUNT; i++) {
		if (&zone_dev_attrs_rgb[i] == target_attr)
			return &zone_data_rgb[i];
	}
	return NULL;
}

static int fourzone_rgb_update_led(struct platform_zone *z, bool read_op){
	u8 wmi_buf[128]; int r;
	/* Always read current state first to have a full valid buffer for SET */
	r=hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_GET_TYPE,HPWMI_FOURZONE,wmi_buf,0,sizeof(wmi_buf));
	if(r){pr_warn("RGB: get_colors failed (err %d) during update\n",r);return r<0?r:-EIO;}

	if(read_op){
		z->colors.r=wmi_buf[z->offset];
		z->colors.g=wmi_buf[z->offset+1];
		z->colors.b=wmi_buf[z->offset+2];
		pr_debug("RGB read z%d(off %d):%02x%02x%02x\n",(int)(z-zone_data_rgb),z->offset,z->colors.r,z->colors.g,z->colors.b);
	} else { /* Write operation */
		wmi_buf[z->offset]=z->colors.r;
		wmi_buf[z->offset+1]=z->colors.g;
		wmi_buf[z->offset+2]=z->colors.b;
		pr_debug("RGB write z%d(off %d):%02x%02x%02x\n",(int)(z-zone_data_rgb),z->offset,z->colors.r,z->colors.g,z->colors.b);
		r=hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_SET_TYPE,HPWMI_FOURZONE,wmi_buf,sizeof(wmi_buf),0);
		if(r){pr_warn("RGB: set_colors failed (err %d)\n",r);return r<0?r:-EIO;}
	}
	return 0;
}

static ssize_t zone_rgb_show(struct device *dev, struct device_attribute *attr, char *buf){
	struct platform_zone *tz=match_zone_by_attr_rgb(attr); int r;
	if(!tz)return -EINVAL;
	r=fourzone_rgb_update_led(tz,true);
	if(r)
		return sysfs_emit(buf,"Error reading zone:%d\n",r);
	return sysfs_emit(buf,"%02X%02X%02X\n",tz->colors.r,tz->colors.g,tz->colors.b);
}

static ssize_t zone_rgb_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	struct platform_zone*tz=match_zone_by_attr_rgb(attr); struct color_platform nc; int r;
	char *trimmed_buf;
	if(!tz)return -EINVAL;
	trimmed_buf = strim((char *)buf); /* strim to remove leading/trailing whitespace, including newline */
	r=parse_rgb(trimmed_buf, &nc);
	if(r)return r;
	tz->colors=nc;
	r=fourzone_rgb_update_led(tz,false);
	return r?r:count;
}

static int fourzone_rgb_setup(struct platform_device *pdev){
	int zi,err=0; char nb[20]; u8 test_buf[128];
	/* Try to detect RGB support by attempting a GET command */
	err=hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_GET_TYPE,HPWMI_FOURZONE,test_buf,0,sizeof(test_buf));
	if(err){
		if(err==HPWMI_RET_UNKNOWN_COMMAND||err==HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_info("Four-zone RGB WMI command not supported by this BIOS (err %d).\n",err);
		else
			pr_warn("Failed to probe Four-zone RGB WMI (err %d), not enabling.\n",err);
		return 0; /* Do not treat as fatal for module load if RGB not supported/detected */
	}
	pr_info("Four-zone RGB WMI detected. Initializing sysfs interface.\n");

	zone_dev_attrs_rgb=kcalloc(FOURZONE_COUNT,sizeof(*zone_dev_attrs_rgb),GFP_KERNEL); if(!zone_dev_attrs_rgb)return -ENOMEM;
	zone_attrs_rgb=kcalloc(FOURZONE_COUNT+1,sizeof(*zone_attrs_rgb),GFP_KERNEL); if(!zone_attrs_rgb){err=-ENOMEM;goto err_fda;}
	zone_data_rgb=kcalloc(FOURZONE_COUNT,sizeof(*zone_data_rgb),GFP_KERNEL); if(!zone_data_rgb){err=-ENOMEM;goto err_fa;}

	for(zi=0;zi<FOURZONE_COUNT;zi++){
		snprintf(nb,sizeof(nb),"zone%02X_rgb",zi); zone_data_rgb[zi].name_ptr=kstrdup(nb,GFP_KERNEL);
		if(!zone_data_rgb[zi].name_ptr){err=-ENOMEM;goto err_fn;}
		sysfs_attr_init(&zone_dev_attrs_rgb[zi].attr);
		zone_dev_attrs_rgb[zi].attr.name=zone_data_rgb[zi].name_ptr;
		zone_dev_attrs_rgb[zi].attr.mode=0664; /* rw-rw-r-- */
		zone_dev_attrs_rgb[zi].show=zone_rgb_show;
		zone_dev_attrs_rgb[zi].store=zone_rgb_set;
		zone_data_rgb[zi].offset=25+(zi*3); zone_data_rgb[zi].attr=&zone_dev_attrs_rgb[zi];
		zone_attrs_rgb[zi]=&zone_dev_attrs_rgb[zi].attr;
	}
	zone_attrs_rgb[FOURZONE_COUNT]=NULL; /* Null terminate the list */
	zone_attribute_group_rgb.attrs=zone_attrs_rgb;

	err=sysfs_create_group(&pdev->dev.kobj,&zone_attribute_group_rgb);
	if(err){pr_err("Failed to create RGB sysfs group: %d\n",err);goto err_fn;}

	pr_info("HP WMI RGB four-zone interface registered.\n"); return 0;

err_fn: for(zi--;zi>=0;zi--) if(zone_data_rgb && zone_data_rgb[zi].name_ptr) kfree(zone_data_rgb[zi].name_ptr);
	if(zone_data_rgb) kfree(zone_data_rgb); zone_data_rgb=NULL;
err_fa: if(zone_attrs_rgb) kfree(zone_attrs_rgb); zone_attrs_rgb=NULL;
err_fda: if(zone_dev_attrs_rgb) kfree(zone_dev_attrs_rgb); zone_dev_attrs_rgb=NULL;
	return err;
}

static void fourzone_rgb_remove(struct platform_device *pdev){
	int i;
	if(zone_attribute_group_rgb.attrs){ /* Check if group was actually created */
		sysfs_remove_group(&pdev->dev.kobj,&zone_attribute_group_rgb);
		if(zone_data_rgb){
			for(i=0;i<FOURZONE_COUNT;i++) {
				if (zone_data_rgb[i].name_ptr) kfree(zone_data_rgb[i].name_ptr);
			}
			kfree(zone_data_rgb); zone_data_rgb=NULL;
		}
		if(zone_attrs_rgb) kfree(zone_attrs_rgb); zone_attrs_rgb=NULL;
		if(zone_dev_attrs_rgb) kfree(zone_dev_attrs_rgb); zone_dev_attrs_rgb=NULL;
		zone_attribute_group_rgb.attrs=NULL; /* Mark as removed */
		pr_info("HP WMI RGB four-zone interface unregistered.\n");
	}
}
/* End RGB functions */


static int platform_profile_omen_get_ec(enum platform_profile_option *p){int t=omen_thermal_profile_get();if(t<0)return t;
	switch(t){case HP_OMEN_V0_THERMAL_PROFILE_PERFORMANCE:case HP_OMEN_V1_THERMAL_PROFILE_PERFORMANCE:*p=PLATFORM_PROFILE_PERFORMANCE;break;
	case HP_OMEN_V0_THERMAL_PROFILE_DEFAULT:case HP_OMEN_V1_THERMAL_PROFILE_DEFAULT:*p=PLATFORM_PROFILE_BALANCED;break;
	case HP_OMEN_V0_THERMAL_PROFILE_COOL:case HP_OMEN_V1_THERMAL_PROFILE_COOL:*p=PLATFORM_PROFILE_COOL;break;
	default:return -EINVAL;}return 0;}
static int platform_profile_omen_get(struct device *d,enum platform_profile_option *p){guard(mutex)(&active_platform_profile_lock);*p=active_platform_profile;return 0;}
static bool has_omen_thermal_profile_ec_timer(void){const char*bn=dmi_get_system_info(DMI_BOARD_NAME);return bn?(match_string(omen_timed_thermal_profile_boards,ARRAY_SIZE(omen_timed_thermal_profile_boards),bn)>=0):false;}
static inline int omen_thermal_profile_ec_flags_set(enum hp_thermal_profile_omen_flags f){return ec_write(HP_OMEN_EC_THERMAL_PROFILE_FLAGS_OFFSET,f);}
static inline int omen_thermal_profile_ec_timer_set(u8 v){return ec_write(HP_OMEN_EC_THERMAL_PROFILE_TIMER_OFFSET,v);}
static int platform_profile_omen_set_ec(enum platform_profile_option p){int err,t,tv=omen_get_thermal_policy_version();enum hp_thermal_profile_omen_flags f=0;
	if(tv<0||tv>1)return -EOPNOTSUPP;
	switch(p){case PLATFORM_PROFILE_PERFORMANCE:t=(tv==0)?HP_OMEN_V0_THERMAL_PROFILE_PERFORMANCE:HP_OMEN_V1_THERMAL_PROFILE_PERFORMANCE;break;
	case PLATFORM_PROFILE_BALANCED:t=(tv==0)?HP_OMEN_V0_THERMAL_PROFILE_DEFAULT:HP_OMEN_V1_THERMAL_PROFILE_DEFAULT;break;
	case PLATFORM_PROFILE_COOL:t=(tv==0)?HP_OMEN_V0_THERMAL_PROFILE_COOL:HP_OMEN_V1_THERMAL_PROFILE_COOL;break;
	default:return -EOPNOTSUPP;} err=omen_thermal_profile_set(t);if(err<0)return err;
	if(has_omen_thermal_profile_ec_timer()){err=omen_thermal_profile_ec_timer_set(0);if(err<0)return err;
	if(p==PLATFORM_PROFILE_PERFORMANCE)f=HP_OMEN_EC_FLAGS_NOTIMER|HP_OMEN_EC_FLAGS_TURBO;
	err=omen_thermal_profile_ec_flags_set(f);if(err<0)return err;}return 0;}
static int platform_profile_omen_set(struct device *d,enum platform_profile_option p){int err;guard(mutex)(&active_platform_profile_lock);
	err=platform_profile_omen_set_ec(p);if(err<0)return err; active_platform_profile=p;return 0;}
static int thermal_profile_get(void){return hp_wmi_read_int(HPWMI_THERMAL_PROFILE_QUERY);}
static int thermal_profile_set(int tp){return hp_wmi_perform_query(HPWMI_THERMAL_PROFILE_QUERY,HPWMI_WRITE,&tp,sizeof(tp),0);}
static int hp_wmi_platform_profile_get(struct device *d,enum platform_profile_option *p){int t=thermal_profile_get();if(t<0)return t;
	switch(t){case HP_THERMAL_PROFILE_PERFORMANCE:*p=PLATFORM_PROFILE_PERFORMANCE;break;
	case HP_THERMAL_PROFILE_DEFAULT:*p=PLATFORM_PROFILE_BALANCED;break;
	case HP_THERMAL_PROFILE_COOL:*p=PLATFORM_PROFILE_COOL;break;
	case HP_THERMAL_PROFILE_QUIET:*p=PLATFORM_PROFILE_QUIET;break;
	default:return -EINVAL;}return 0;}
static int hp_wmi_platform_profile_set(struct device *d,enum platform_profile_option p){int err,t;
	switch(p){case PLATFORM_PROFILE_PERFORMANCE:t=HP_THERMAL_PROFILE_PERFORMANCE;break;
	case PLATFORM_PROFILE_BALANCED:t=HP_THERMAL_PROFILE_DEFAULT;break;
	case PLATFORM_PROFILE_COOL:t=HP_THERMAL_PROFILE_COOL;break;
	case PLATFORM_PROFILE_QUIET:t=HP_THERMAL_PROFILE_QUIET;break;
	default:return -EOPNOTSUPP;} err=thermal_profile_set(t);if(err)return err;return 0;}
static bool is_victus_thermal_profile(void){const char*bn=dmi_get_system_info(DMI_BOARD_NAME);return bn?(match_string(victus_thermal_profile_boards,ARRAY_SIZE(victus_thermal_profile_boards),bn)>=0):false;}
static int platform_profile_victus_get_ec(enum platform_profile_option *p){int t=omen_thermal_profile_get();if(t<0)return t;
	switch(t){case HP_VICTUS_THERMAL_PROFILE_PERFORMANCE:*p=PLATFORM_PROFILE_PERFORMANCE;break;
	case HP_VICTUS_THERMAL_PROFILE_DEFAULT:*p=PLATFORM_PROFILE_BALANCED;break;
	case HP_VICTUS_THERMAL_PROFILE_QUIET:*p=PLATFORM_PROFILE_QUIET;break;
	default:return -EOPNOTSUPP;}return 0;}
static int platform_profile_victus_get(struct device *d,enum platform_profile_option *p){return platform_profile_omen_get(d,p);}
static int platform_profile_victus_set_ec(enum platform_profile_option p){int err,t;
	switch(p){case PLATFORM_PROFILE_PERFORMANCE:t=HP_VICTUS_THERMAL_PROFILE_PERFORMANCE;break;
	case PLATFORM_PROFILE_BALANCED:t=HP_VICTUS_THERMAL_PROFILE_DEFAULT;break;
	case PLATFORM_PROFILE_QUIET:t=HP_VICTUS_THERMAL_PROFILE_QUIET;break;
	default:return -EOPNOTSUPP;} err=omen_thermal_profile_set(t);if(err<0)return err;return 0;}
static bool is_victus_s_thermal_profile(void){const char*bn=dmi_get_system_info(DMI_BOARD_NAME);return bn?(match_string(victus_s_thermal_profile_boards,ARRAY_SIZE(victus_s_thermal_profile_boards),bn)>=0):false;}
static int victus_s_gpu_thermal_profile_get(bool *ctgp,bool *ppab,u8 *ds,u8 *st){struct victus_gpu_power_modes m;int r=hp_wmi_perform_query(HPWMI_GET_GPU_THERMAL_MODES_QUERY,HPWMI_GM,&m,sizeof(m),sizeof(m));
	if(r==0){*ctgp=m.ctgp_enable?true:false;*ppab=m.ppab_enable?true:false;*ds=m.dstate;*st=m.gpu_slowdown_temp;}return r;}
static int victus_s_gpu_thermal_profile_set(bool ctgp,bool ppab,u8 ds){struct victus_gpu_power_modes m;int r;bool c_ctgp,c_ppab;u8 c_ds,c_st;
	r=victus_s_gpu_thermal_profile_get(&c_ctgp,&c_ppab,&c_ds,&c_st);if(r<0){pr_warn("GPU modes not updated, fail get slowdown\n");return r;}
	m.ctgp_enable=ctgp?1:0;m.ppab_enable=ppab?1:0;m.dstate=ds;m.gpu_slowdown_temp=c_st;
	return hp_wmi_perform_query(HPWMI_SET_GPU_THERMAL_MODES_QUERY,HPWMI_GM,&m,sizeof(m),0);}
static int victus_s_set_cpu_pl1_pl2(u8 pl1,u8 pl2){struct victus_power_limits l;if(pl1==HP_POWER_LIMIT_NO_CHANGE||pl2==HP_POWER_LIMIT_NO_CHANGE||pl2<pl1)return -EINVAL;
	l.pl1=pl1;l.pl2=pl2;l.pl4=HP_POWER_LIMIT_NO_CHANGE;l.cpu_gpu_concurrent_limit=HP_POWER_LIMIT_NO_CHANGE;
	return hp_wmi_perform_query(HPWMI_SET_POWER_LIMITS_QUERY,HPWMI_GM,&l,sizeof(l),0);}
static int platform_profile_victus_s_set_ec(enum platform_profile_option p){bool gpu_ctgp,gpu_ppab;u8 gpu_ds;int err,t;
	switch(p){case PLATFORM_PROFILE_PERFORMANCE:t=HP_VICTUS_S_THERMAL_PROFILE_PERFORMANCE;gpu_ctgp=true;gpu_ppab=true;gpu_ds=1;break;
	case PLATFORM_PROFILE_BALANCED:t=HP_VICTUS_S_THERMAL_PROFILE_DEFAULT;gpu_ctgp=false;gpu_ppab=true;gpu_ds=1;break;
	case PLATFORM_PROFILE_LOW_POWER:t=HP_VICTUS_S_THERMAL_PROFILE_DEFAULT;gpu_ctgp=false;gpu_ppab=false;gpu_ds=1;break;
	default:return -EOPNOTSUPP;} hp_wmi_get_fan_count_userdefine_trigger();
	err=omen_thermal_profile_set(t);if(err<0){pr_err("Fail set platform profile %d:%d\n",p,err);return err;}
	err=victus_s_gpu_thermal_profile_set(gpu_ctgp,gpu_ppab,gpu_ds);if(err<0){pr_err("Fail set GPU profile %d:%d\n",p,err);return err;}
	return 0;}
static int platform_profile_victus_s_set(struct device *d,enum platform_profile_option p){int err;guard(mutex)(&active_platform_profile_lock);
	err=platform_profile_victus_s_set_ec(p);if(err<0)return err;active_platform_profile=p;return 0;}
static int platform_profile_victus_set(struct device *d,enum platform_profile_option p){int err;guard(mutex)(&active_platform_profile_lock);
	err=platform_profile_victus_set_ec(p);if(err<0)return err;active_platform_profile=p;return 0;}
static int hp_wmi_platform_profile_probe(void *drv,unsigned long *choices){
	if(is_omen_thermal_profile())set_bit(PLATFORM_PROFILE_COOL,choices);
	else if(is_victus_thermal_profile())set_bit(PLATFORM_PROFILE_QUIET,choices);
	else if(is_victus_s_thermal_profile())set_bit(PLATFORM_PROFILE_LOW_POWER,choices);
	else{set_bit(PLATFORM_PROFILE_QUIET,choices);set_bit(PLATFORM_PROFILE_COOL,choices);}
	set_bit(PLATFORM_PROFILE_BALANCED,choices);set_bit(PLATFORM_PROFILE_PERFORMANCE,choices);return 0;}

static int omen_powersource_event(struct notifier_block *nb,unsigned long val,void *data){
	struct acpi_bus_event *evt=data;enum platform_profile_option actual;int err;
	if(strcmp(evt->device_class,ACPI_AC_CLASS)!=0) {
		return NOTIFY_DONE;
	}
	pr_debug("Power source event\n");
	guard(mutex)(&active_platform_profile_lock);
	if(is_omen_thermal_profile()) {
		err=platform_profile_omen_get_ec(&actual);
	} else {
		err=platform_profile_victus_get_ec(&actual);
	}
	if(err<0){
		pr_warn("Fail read current profile (%d)\n",err);
		return NOTIFY_DONE;
	}
	if(power_supply_is_system_supplied()<=0||active_platform_profile==actual){
		pr_debug("Profile update skipped\n");
		return NOTIFY_DONE;
	}
	if(is_omen_thermal_profile()) {
		err=platform_profile_omen_set_ec(active_platform_profile);
	} else {
		err=platform_profile_victus_set_ec(active_platform_profile);
	}
	if(err<0) {
		pr_warn("Fail restore profile (%d)\n",err);
	}
	return NOTIFY_OK;
}

static int victus_s_powersource_event(struct notifier_block *nb,unsigned long val,void *data){
	struct acpi_bus_event *evt=data;int err;
	if(strcmp(evt->device_class,ACPI_AC_CLASS)!=0) {
		return NOTIFY_DONE;
	}
	pr_debug("Victus S Power event\n");
	if(active_platform_profile==PLATFORM_PROFILE_PERFORMANCE){
		pr_debug("Trigger CPU PL1/PL2\n");
		err=victus_s_set_cpu_pl1_pl2(HP_POWER_LIMIT_DEFAULT,HP_POWER_LIMIT_DEFAULT);
		if(err) {
			pr_warn("Fail actualize PLs:%d\n",err);
		}
	}
	return NOTIFY_OK;
}

static int omen_register_powersource_event_handler(void){platform_power_source_nb.notifier_call=omen_powersource_event;return register_acpi_notifier(&platform_power_source_nb);}
static int victus_s_register_powersource_event_handler(void){platform_power_source_nb.notifier_call=victus_s_powersource_event;return register_acpi_notifier(&platform_power_source_nb);}
static inline void omen_unregister_powersource_event_handler(void){unregister_acpi_notifier(&platform_power_source_nb);}
static inline void victus_s_unregister_powersource_event_handler(void){unregister_acpi_notifier(&platform_power_source_nb);}

static const struct platform_profile_ops platform_profile_omen_ops={.probe=hp_wmi_platform_profile_probe,.profile_get=platform_profile_omen_get,.profile_set=platform_profile_omen_set};
static const struct platform_profile_ops platform_profile_victus_ops={.probe=hp_wmi_platform_profile_probe,.profile_get=platform_profile_victus_get,.profile_set=platform_profile_victus_set};
static const struct platform_profile_ops platform_profile_victus_s_ops={.probe=hp_wmi_platform_profile_probe,.profile_get=platform_profile_omen_get,.profile_set=platform_profile_victus_s_set};
static const struct platform_profile_ops hp_wmi_platform_profile_ops={.probe=hp_wmi_platform_profile_probe,.profile_get=hp_wmi_platform_profile_get,.profile_set=hp_wmi_platform_profile_set};

static int thermal_profile_setup(struct platform_device *device){
	const struct platform_profile_ops *ops;int err,tp;
	if(is_omen_thermal_profile()){err=platform_profile_omen_get_ec(&active_platform_profile);if(err<0)return err;
		err=platform_profile_omen_set_ec(active_platform_profile);if(err<0)return err; ops=&platform_profile_omen_ops;}
	else if(is_victus_thermal_profile()){err=platform_profile_victus_get_ec(&active_platform_profile);if(err<0)return err;
		err=platform_profile_victus_set_ec(active_platform_profile);if(err<0)return err; ops=&platform_profile_victus_ops;}
	else if(is_victus_s_thermal_profile()){active_platform_profile=PLATFORM_PROFILE_BALANCED;
		err=platform_profile_victus_s_set_ec(active_platform_profile);if(err<0)return err; ops=&platform_profile_victus_s_ops;}
	else{tp=thermal_profile_get();if(tp<0)return tp; err=thermal_profile_set(tp);if(err)return err; ops=&hp_wmi_platform_profile_ops;}
	platform_profile_device=devm_platform_profile_register(&device->dev,"hp-wmi",NULL,ops);
	if(IS_ERR(platform_profile_device))return PTR_ERR(platform_profile_device);
	pr_info("Registered as platform profile handler\n");platform_profile_support=true; return 0;
}
static int hp_wmi_hwmon_init(void);

static int __init hp_wmi_bios_setup(struct platform_device *device){
	int err=0;
	wifi_rfkill=NULL; bluetooth_rfkill=NULL; wwan_rfkill=NULL; rfkill2_count=0;

	if(hp_wmi_bios_2009_later() > 0){ /* Prefer rfkill2 on newer BIOS */
		err=hp_wmi_rfkill2_setup(device);
		if(err && err != -ENODEV){
			pr_warn("rfkill2_setup failed:%d, trying legacy\n",err);
			err=hp_wmi_rfkill_setup(device);
			if(err && err != -ENODEV) pr_warn("Legacy rfkill_setup also failed:%d\n",err);
		} else if(err == -ENODEV){ /* No rfkill2 devices found */
			pr_info("No rfkill2 devices found, trying legacy rfkill.\n");
			err=hp_wmi_rfkill_setup(device);
			if(err && err != -ENODEV) pr_warn("Legacy rfkill_setup failed:%d\n",err);
		}
	} else { /* Older BIOS, try legacy first */
		err=hp_wmi_rfkill_setup(device);
		if(err && err != -ENODEV){
			pr_warn("Legacy rfkill_setup failed:%d, trying rfkill2\n",err);
			err=hp_wmi_rfkill2_setup(device);
			if(err && err != -ENODEV) pr_warn("rfkill2_setup also failed:%d\n",err);
		} else if(err == -ENODEV){
			pr_info("No legacy rfkill devices found, trying rfkill2.\n");
			err=hp_wmi_rfkill2_setup(device);
			if(err && err != -ENODEV) pr_warn("rfkill2_setup failed:%d\n",err);
		}
	}
	if(err == -ENODEV) err = 0; /* -ENODEV is not fatal for module load */

	err=fourzone_rgb_setup(device);
	if(err && err != -ENODEV) { /* Report error only if it's not -ENODEV (RGB not supported) */
		pr_err("Failed RGB setup:%d\n",err);
	}
	/* Continue even if RGB/HWMON/Thermal fails, other functionalities might work */
	err=hp_wmi_hwmon_init(); if(err)pr_err("Failed HWMON init:%d\n",err);
	err=thermal_profile_setup(device); if(err)pr_err("Failed thermal profile setup:%d\n",err);
	return 0;
}
static void __exit hp_wmi_bios_remove(struct platform_device *device){
	int i;
	fourzone_rgb_remove(device);
	for(i=0;i<rfkill2_count;i++){if(rfkill2[i].rfkill){rfkill_unregister(rfkill2[i].rfkill);rfkill_destroy(rfkill2[i].rfkill);rfkill2[i].rfkill=NULL;}}
	rfkill2_count=0;
	if(wifi_rfkill){rfkill_unregister(wifi_rfkill);rfkill_destroy(wifi_rfkill);wifi_rfkill=NULL;}
	if(bluetooth_rfkill){rfkill_unregister(bluetooth_rfkill);rfkill_destroy(bluetooth_rfkill);bluetooth_rfkill=NULL;}
	if(wwan_rfkill){rfkill_unregister(wwan_rfkill);rfkill_destroy(wwan_rfkill);wwan_rfkill=NULL;}
}
static int hp_wmi_resume_handler(struct device *d){
	if(hp_wmi_input_dev){
		if(test_bit(SW_DOCK,hp_wmi_input_dev->swbit))input_report_switch(hp_wmi_input_dev,SW_DOCK,hp_wmi_get_dock_state());
		if(test_bit(SW_TABLET_MODE,hp_wmi_input_dev->swbit))input_report_switch(hp_wmi_input_dev,SW_TABLET_MODE,hp_wmi_get_tablet_mode());
		input_sync(hp_wmi_input_dev);}
	if(rfkill2_count)hp_wmi_rfkill2_refresh(); else{
		if(wifi_rfkill)rfkill_set_states(wifi_rfkill,hp_wmi_get_sw_state(HPWMI_WIFI),hp_wmi_get_hw_state(HPWMI_WIFI));
		if(bluetooth_rfkill)rfkill_set_states(bluetooth_rfkill,hp_wmi_get_sw_state(HPWMI_BLUETOOTH),hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		if(wwan_rfkill)rfkill_set_states(wwan_rfkill,hp_wmi_get_sw_state(HPWMI_WWAN),hp_wmi_get_hw_state(HPWMI_WWAN));}
	return 0;
}
static const struct dev_pm_ops hp_wmi_pm_ops={.resume=hp_wmi_resume_handler,.restore=hp_wmi_resume_handler,};
static struct platform_driver hp_wmi_driver __refdata = {
	.driver={.name="hp-wmi",.pm=&hp_wmi_pm_ops,.dev_groups=hp_wmi_groups,},
	.probe=hp_wmi_bios_setup,.remove=__exit_p(hp_wmi_bios_remove),
};

static umode_t hp_wmi_hwmon_is_visible(const void *data,enum hwmon_sensor_types type,u32 attr,int chan){
	switch(type){
	case hwmon_pwm:
		if(attr==hwmon_pwm_enable)
			return 0644;
		break;
	case hwmon_fan:
		if(attr==hwmon_fan_input){
			if(is_victus_s_thermal_profile()){
				if (hp_wmi_get_fan_speed_victus_s(chan)>=0) return 0444;
			} else {
				if (hp_wmi_get_fan_speed(chan)>=0) return 0444;
			}
		}
		break;
	default:
		break;
	}
	return 0;
}
static int hp_wmi_hwmon_read(struct device *d,enum hwmon_sensor_types type,u32 attr,int chan,long *val){
	int ret;
	switch(type){
	case hwmon_fan:
		if(attr==hwmon_fan_input){
			ret=is_victus_s_thermal_profile()?hp_wmi_get_fan_speed_victus_s(chan):hp_wmi_get_fan_speed(chan);
			if(ret<0)
				return ret;
			*val=ret;
			return 0;
		}
		break;
	case hwmon_pwm:
		if(attr==hwmon_pwm_enable){
			ret=hp_wmi_fan_speed_max_get();
			if(ret<0)
				return ret;
			if(ret==0)
				*val=2;
			else if(ret==1)
				*val=0;
			else
				return -ENODATA;
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}
static int hp_wmi_hwmon_write(struct device *d,enum hwmon_sensor_types type,u32 attr,int chan,long val){
	if(type==hwmon_pwm&&attr==hwmon_pwm_enable){
		if(is_victus_s_thermal_profile())
			hp_wmi_get_fan_count_userdefine_trigger();
		if(val==2)
			return is_victus_s_thermal_profile()?hp_wmi_fan_speed_max_reset():hp_wmi_fan_speed_max_set(0);
		if(val==0)
			return hp_wmi_fan_speed_max_set(1);
		return -EINVAL;
	}
	return -EOPNOTSUPP;
}
static const struct hwmon_channel_info * const hp_wmi_hwmon_info[]={HWMON_CHANNEL_INFO(fan,HWMON_F_INPUT,HWMON_F_INPUT),HWMON_CHANNEL_INFO(pwm,HWMON_PWM_ENABLE),NULL};
static const struct hwmon_ops hp_wmi_hwmon_ops={.is_visible=hp_wmi_hwmon_is_visible,.read=hp_wmi_hwmon_read,.write=hp_wmi_hwmon_write};
static const struct hwmon_chip_info hp_wmi_hwmon_chip_info={.ops=&hp_wmi_hwmon_ops,.info=hp_wmi_hwmon_info};
static int hp_wmi_hwmon_init(void){
	struct device *dev,*hwmon_dev; if(!hp_wmi_platform_dev){pr_err("HWMON: pdev NULL\n");return -ENODEV;}
	dev=&hp_wmi_platform_dev->dev;
	hwmon_dev=devm_hwmon_device_register_with_info(dev,"hp_wmi",NULL,&hp_wmi_hwmon_chip_info,NULL);
	if(IS_ERR(hwmon_dev)){pr_err("Fail register hwmon:%ld\n",PTR_ERR(hwmon_dev));return PTR_ERR(hwmon_dev);}
	pr_info("HP WMI HWMON registered.\n");return 0;
}

static int __init hp_wmi_init(void)
{
	int err=0,test_val=0; bool ec=wmi_has_guid(HPWMI_EVENT_GUID),bc=wmi_has_guid(HPWMI_BIOS_GUID);
	if(!bc&&!ec){pr_info("No HP WMI interface found.\n");return -ENODEV;}

	if(bc){ /* Check zero_insize_support only if bios_capable */
		if(hp_wmi_perform_query(HPWMI_HARDWARE_QUERY,HPWMI_READ,&test_val,sizeof(test_val),sizeof(test_val))==HPWMI_RET_INVALID_PARAMETERS)
			zero_insize_support=true;
		else
			zero_insize_support=false;
	} else {
		zero_insize_support=false; /* Default if not bios_capable */
	}

	if(ec){err=hp_wmi_input_setup();if(err){pr_err("HP WMI input_setup fail:%d\n",err);return err;}}

	if(bc){
		hp_wmi_platform_dev=platform_device_register_simple("hp-wmi",PLATFORM_DEVID_NONE,NULL,0);
		if(IS_ERR(hp_wmi_platform_dev)){
			err=PTR_ERR(hp_wmi_platform_dev);
			pr_err("Fail register pdev:%d\n",err);
			goto err_destroy_input;
		}
		/* Using platform_driver_probe as in the initial upstream code */
		/* hp_wmi_driver.probe is hp_wmi_bios_setup */
		err = platform_driver_probe(&hp_wmi_driver, hp_wmi_bios_setup);
		if(err){
			pr_err("Fail probe pdrv:%d\n",err);
			goto err_unregister_pdev;
		}
	}

	if(bc && hp_wmi_platform_dev){ /* Register power source handlers only if platform device exists */
		if(is_omen_thermal_profile()||is_victus_thermal_profile()){
			err=omen_register_powersource_event_handler();
			if(err)pr_warn("Fail Omen/Victus ACPI power notify:%d\n",err);
		} else if(is_victus_s_thermal_profile()){
			err=victus_s_register_powersource_event_handler();
			if(err)pr_warn("Fail VictusS ACPI power notify:%d\n",err);
		}
	}
	pr_info("HP WMI driver init (Evt:%d,BIOS:%d,RGB:exp)\n",ec,bc);return 0;

err_unregister_pdev:
	platform_device_unregister(hp_wmi_platform_dev);
	hp_wmi_platform_dev=NULL;
err_destroy_input:
	if(ec)hp_wmi_input_destroy();
	if(camera_shutter_input_dev){input_unregister_device(camera_shutter_input_dev);camera_shutter_input_dev=NULL;}
	return err;
}
module_init(hp_wmi_init);

static void __exit hp_wmi_exit(void)
{
	if(hp_wmi_platform_dev) { /* Unregister only if platform_dev was created */
		if(is_omen_thermal_profile()||is_victus_thermal_profile())
			omen_unregister_powersource_event_handler();
		else if(is_victus_s_thermal_profile())
			victus_s_unregister_powersource_event_handler();
	}

	if(wmi_has_guid(HPWMI_BIOS_GUID)&&hp_wmi_platform_dev){
		/* If platform_driver_probe was used, unregistering device first might be safer
		   before unregistering driver, or just unregister driver.
		   The original code unregisters device then driver.
		   Let's stick to platform_driver_unregister if platform_driver_probe was used
		   and platform_device_unregister separately.
		*/
		platform_driver_unregister(&hp_wmi_driver); // If probe was used, this unbinds and calls remove
		platform_device_unregister(hp_wmi_platform_dev);
		hp_wmi_platform_dev=NULL;
	}
	if(wmi_has_guid(HPWMI_EVENT_GUID)) {
		hp_wmi_input_destroy();
	}
	if(camera_shutter_input_dev){
		input_unregister_device(camera_shutter_input_dev);
		camera_shutter_input_dev=NULL;
	}
	pr_info("HP WMI driver unloaded.\n");
}
module_exit(hp_wmi_exit);