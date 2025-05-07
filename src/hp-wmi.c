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
#include <linux/acpi.h>
#include <linux/rfkill.h>
#include <linux/string.h>
#include <linux/dmi.h>
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include <linux/power_supply.h>

MODULE_AUTHOR("Matthew Garrett <mjg59@srcf.ucam.org>");
MODULE_DESCRIPTION("HP laptop WMI hotkeys driver");
MODULE_LICENSE("GPL");

MODULE_ALIAS("wmi:95F24279-4D7B-4334-9387-ACCDC67EF61C");
MODULE_ALIAS("wmi:5FB7F034-2C63-45e9-BE91-3D44E2C707E4");

static int enable_tablet_mode_sw = -1;
module_param(enable_tablet_mode_sw, int, 0444);
MODULE_PARM_DESC(enable_tablet_mode_sw, "Enable SW_TABLET_MODE reporting (-1=auto, 0=no, 1=yes)");

#define HPWMI_EVENT_GUID "95F24279-4D7B-4334-9387-ACCDC67EF61C"
#define HPWMI_BIOS_GUID "5FB7F034-2C63-45e9-BE91-3D44E2C707E4"
#define HP_OMEN_EC_THERMAL_PROFILE_OFFSET 0x95

static const char * const omen_thermal_profile_boards[] = {
	"84DA", "84DB", "84DC", "8574", "8575", "860A", "87B5", "8572", "8573",
	"8600", "8601", "8602", "8605", "8606", "8607", "8746", "8747", "8749",
	"874A", "8603", "8604", "8748", "886B", "886C", "878A", "878B", "878C",
	"88C8", "88CB", "8786", "8787", "8788", "88D1", "88D2", "88F4", "88FD",
	"88F5", "88F6", "88F7", "88FE", "88FF", "8900", "8901", "8902", "8912",
	"8917", "8918", "8949", "894A", "89EB"
};

enum hp_wmi_radio { HPWMI_WIFI = 0x0, HPWMI_BLUETOOTH = 0x1, HPWMI_WWAN = 0x2, HPWMI_GPS = 0x3 };
enum hp_wmi_event_ids {
	HPWMI_DOCK_EVENT = 0x01, HPWMI_PARK_HDD = 0x02, HPWMI_SMART_ADAPTER = 0x03,
	HPWMI_BEZEL_BUTTON = 0x04, HPWMI_WIRELESS = 0x05, HPWMI_CPU_BATTERY_THROTTLE = 0x06,
	HPWMI_LOCK_SWITCH = 0x07, HPWMI_LID_SWITCH = 0x08, HPWMI_SCREEN_ROTATION = 0x09,
	HPWMI_COOLSENSE_SYSTEM_MOBILE = 0x0A, HPWMI_COOLSENSE_SYSTEM_HOT = 0x0B,
	HPWMI_PROXIMITY_SENSOR = 0x0C, HPWMI_BACKLIT_KB_BRIGHTNESS = 0x0D,
	HPWMI_PEAKSHIFT_PERIOD = 0x0F, HPWMI_BATTERY_CHARGE_PERIOD = 0x10,
	HPWMI_SANITIZATION_MODE = 0x17, HPWMI_CAMERA_TOGGLE = 0x1A, HPWMI_OMEN_KEY = 0x1D,
	HPWMI_SMART_EXPERIENCE_APP = 0x21,
};
struct bios_args { u32 signature; u32 command; u32 commandtype; u32 datasize; u8 data[128]; };
enum hp_wmi_commandtype {
	HPWMI_DISPLAY_QUERY = 0x01, HPWMI_HDDTEMP_QUERY = 0x02, HPWMI_ALS_QUERY = 0x03,
	HPWMI_HARDWARE_QUERY = 0x04, HPWMI_WIRELESS_QUERY = 0x05, HPWMI_BATTERY_QUERY = 0x07,
	HPWMI_BIOS_QUERY = 0x09, HPWMI_FEATURE_QUERY = 0x0b, HPWMI_HOTKEY_QUERY = 0x0c,
	HPWMI_FEATURE2_QUERY = 0x0d, HPWMI_WIRELESS2_QUERY = 0x1b, HPWMI_POSTCODEERROR_QUERY = 0x2a,
	HPWMI_THERMAL_PROFILE_QUERY = 0x4c, HPWMI_SYSTEM_DEVICE_MODE = 0x40,
	HPWMI_FOURZONE_COLOR_GET = 2, HPWMI_FOURZONE_COLOR_SET = 3, HPWMI_FOURZONE_BRIGHT_GET = 4,
	HPWMI_FOURZONE_BRIGHT_SET = 5, HPWMI_FOURZONE_ANIM_GET = 6, HPWMI_FOURZONE_ANIM_SET = 7,
};
enum hp_wmi_gm_commandtype {
	HPWMI_FAN_SPEED_GET_QUERY = 0x11, HPWMI_SET_PERFORMANCE_MODE = 0x1A,
	HPWMI_FAN_SPEED_MAX_GET_QUERY = 0x26, HPWMI_FAN_SPEED_MAX_SET_QUERY = 0x27,
};
enum hp_wmi_command { HPWMI_READ = 0x01, HPWMI_WRITE = 0x02, HPWMI_ODM = 0x03, HPWMI_GM = 0x20008, HPWMI_FOURZONE = 0x20009 };
enum hp_wmi_hardware_mask { HPWMI_DOCK_MASK = 0x01, HPWMI_TABLET_MASK = 0x04 };
struct bios_return { u32 sigpass; u32 return_code; };
enum hp_return_value {
	HPWMI_RET_WRONG_SIGNATURE = 0x02, HPWMI_RET_UNKNOWN_COMMAND = 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE = 0x04, HPWMI_RET_INVALID_PARAMETERS = 0x05,
};
enum hp_wireless2_bits {
	HPWMI_POWER_STATE = 0x01, HPWMI_POWER_SOFT = 0x02, HPWMI_POWER_BIOS = 0x04,
	HPWMI_POWER_HARD = 0x08, HPWMI_POWER_FW_OR_HW = HPWMI_POWER_BIOS | HPWMI_POWER_HARD,
};
enum hp_thermal_profile_omen { HP_OMEN_THERMAL_PROFILE_DEFAULT=0x00, HP_OMEN_THERMAL_PROFILE_PERFORMANCE=0x01, HP_OMEN_THERMAL_PROFILE_COOL=0x02 };
enum hp_thermal_profile { HP_THERMAL_PROFILE_PERFORMANCE=0x00, HP_THERMAL_PROFILE_DEFAULT=0x01, HP_THERMAL_PROFILE_COOL=0x02 };
#define IS_HWBLOCKED(x) ((x & HPWMI_POWER_FW_OR_HW) != HPWMI_POWER_FW_OR_HW)
#define IS_SWBLOCKED(x) !(x & HPWMI_POWER_SOFT)
struct bios_rfkill2_device_state { u8 radio_type; u8 bus_type; u16 vendor_id; u16 product_id; u16 subsys_vendor_id; u16 subsys_product_id; u8 rfkill_id; u8 power; u8 unknown[4]; };
#define HPWMI_MAX_RFKILL2_DEVICES 7
struct bios_rfkill2_state { u8 unknown[7]; u8 count; u8 pad[8]; struct bios_rfkill2_device_state device[HPWMI_MAX_RFKILL2_DEVICES]; };
#define HPWMI_HOTKEY_RELEASE_FLAG (1<<16)
static const struct key_entry hp_wmi_keymap[] = {
	{ KE_KEY, 0x02,   { KEY_BRIGHTNESSUP } }, { KE_KEY, 0x03,   { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x20e6, { KEY_PROG1 } }, { KE_KEY, 0x20e8, { KEY_MEDIA } },
	{ KE_KEY, 0x2142, { KEY_MEDIA } }, { KE_KEY, 0x213b, { KEY_INFO } },
	{ KE_KEY, 0x2169, { KEY_ROTATE_DISPLAY } }, { KE_KEY, 0x216a, { KEY_SETUP } },
	{ KE_KEY, 0x231b, { KEY_HELP } }, { KE_KEY, 0x21A4, { KEY_F14 } },
	{ KE_KEY, 0x21A5, { KEY_F15 } }, { KE_KEY, 0x21A7, { KEY_F16 } },
	{ KE_KEY, 0x21A9, { KEY_F17 } }, { KE_END, 0 }
};

static struct input_dev *hp_wmi_input_dev;
static struct platform_device *hp_wmi_platform_dev;
static struct input_dev *camera_shutter_input_dev;

static bool platform_profile_support;
static struct device *platform_profile_dev;

static struct platform_profile_ops hp_wmi_profile_ops;
static struct platform_profile_ops hp_wmi_omen_profile_ops;

static struct rfkill *wifi_rfkill;
static struct rfkill *bluetooth_rfkill;
static struct rfkill *wwan_rfkill;
struct rfkill2_device { u8 id; int num; struct rfkill *rfkill; };
static int rfkill2_count;
static struct rfkill2_device rfkill2[HPWMI_MAX_RFKILL2_DEVICES];
static const char * const tablet_chassis_types[] = { "30", "31", "32" };
#define DEVICE_MODE_TABLET	0x06
struct quirk_entry { bool fourzone; };
static struct quirk_entry temp_omen = { .fourzone = true, };
static struct quirk_entry *quirks = &temp_omen;

static inline int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096) return -EINVAL;
	if (outsize > 1024) return 5;
	if (outsize > 128) return 4;
	if (outsize > 4) return 3;
	if (outsize > 0) return 2;
	return 1;
}

static int hp_wmi_perform_query(int query, enum hp_wmi_command command, void *buffer, int insize, int outsize) {
	int mid, actual_outsize, ret = 0;
	struct bios_return *bios_return;
	union acpi_object *obj;
	struct bios_args args = { .signature = 0x55434553, .command = command, .commandtype = query, .datasize = insize, .data = {0} };
	struct acpi_buffer input = { sizeof(struct bios_args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	mid = encode_outsize_for_pvsz(outsize);
	if (WARN_ON(mid < 0)) return mid;
	if (WARN_ON(insize > sizeof(args.data))) return -EINVAL;
	if (insize > 0 && buffer) memcpy(&args.data[0], buffer, insize);

	wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	obj = output.pointer;
	if (!obj) return -EINVAL;
	if (obj->type != ACPI_TYPE_BUFFER) { ret = -EINVAL; goto out_free; }

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;
	if (ret) {
		if (ret != HPWMI_RET_UNKNOWN_COMMAND && ret != HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_warn("query 0x%x cmd 0x%x err 0x%x\n", query, command, ret);
		goto out_free;
	}
	if (!outsize) goto out_free;
	actual_outsize = min_t(int, outsize, obj->buffer.length - sizeof(*bios_return));
	if (buffer) {
		memcpy(buffer, obj->buffer.pointer + sizeof(*bios_return), actual_outsize);
		if (outsize > actual_outsize) memset(buffer + actual_outsize, 0, outsize - actual_outsize);
	}
out_free:
	kfree(obj);
	return ret;
}

static int hp_wmi_get_fan_speed(int fan) {
	u8 fsh, fsl; char fan_data[4] = { fan, 0, 0, 0 };
	if (hp_wmi_perform_query(HPWMI_FAN_SPEED_GET_QUERY, HPWMI_GM, &fan_data, sizeof(fan_data), sizeof(fan_data)) != 0) return -EINVAL;
	fsh = fan_data[2]; fsl = fan_data[3]; return (fsh << 8) | fsl;
}
static int hp_wmi_read_int(int query) {
	int val = 0, ret = hp_wmi_perform_query(query, HPWMI_READ, &val, sizeof(val), sizeof(val));
	return ret ? (ret < 0 ? ret : -EINVAL) : val;
}
static int hp_wmi_hw_state(int mask) { int s = hp_wmi_read_int(HPWMI_HARDWARE_QUERY); return (s < 0) ? s : !!(s & mask); }
static int omen_thermal_profile_set(int mode) {
	char b[2] = {0, mode}; int r; if (mode < 0 || mode > 2) return -EINVAL;
	r = hp_wmi_perform_query(HPWMI_SET_PERFORMANCE_MODE, HPWMI_GM, &b, sizeof(b), 0); return r ? (r < 0 ? r : -EINVAL) : 0;
}
static bool is_omen_thermal_profile(void) {
	const char *bn = dmi_get_system_info(DMI_BOARD_NAME); if (!bn) return false;
	return match_string(omen_thermal_profile_boards, ARRAY_SIZE(omen_thermal_profile_boards), bn) >= 0;
}
static int omen_thermal_profile_get(void) { u8 d; int r = ec_read(HP_OMEN_EC_THERMAL_PROFILE_OFFSET, &d); return r < 0 ? r : d; }
static int hp_wmi_fan_speed_max_set(int en) { int v = en, r = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, HPWMI_GM, &v, sizeof(v), 0); return r ? (r < 0 ? r : -EINVAL) : 0; }
static int hp_wmi_fan_speed_max_get(void) {
	int v = 0, r = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, HPWMI_GM, &v, sizeof(v), sizeof(v)); return r ? (r < 0 ? r : -EINVAL) : v;
}
static int __init hp_wmi_bios_2008_later(void) {
	int s = 0, r = hp_wmi_perform_query(HPWMI_FEATURE_QUERY, HPWMI_READ, &s, sizeof(s), sizeof(s));
	return !r ? 1 : ((r == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO);
}
static int __init hp_wmi_bios_2009_later(void) {
	u8 s[128] = {0}; int r = hp_wmi_perform_query(HPWMI_FEATURE2_QUERY, HPWMI_READ, &s, sizeof(s), sizeof(s));
	return !r ? 1 : ((r == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO);
}
static int __init hp_wmi_enable_hotkeys(void) { int v = 0x6e, r = hp_wmi_perform_query(HPWMI_BIOS_QUERY, HPWMI_WRITE, &v, sizeof(v), 0); return r <= 0 ? r : -EINVAL; }
static int hp_wmi_set_block(void *data, bool blocked) {
	enum hp_wmi_radio r = (enum hp_wmi_radio)(uintptr_t)data; int q = BIT(r + 8) | ((!blocked) << r);
	int ret = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE, &q, sizeof(q), 0); return ret <= 0 ? ret : -EINVAL;
}
static const struct rfkill_ops hp_wmi_rfkill_ops = { .set_block = hp_wmi_set_block };
static bool hp_wmi_get_sw_state(enum hp_wmi_radio r) {
	int m = 0x200 << (r * 8), w = hp_wmi_read_int(HPWMI_WIRELESS_QUERY); if (w < 0) { WARN_ONCE(1, "err HPWMI_WIRELESS_QUERY SW"); return true; } return !(w & m);
}
static int camera_shutter_input_setup(void) {
	int err; camera_shutter_input_dev = input_allocate_device(); if (!camera_shutter_input_dev) return -ENOMEM;
	camera_shutter_input_dev->name = "HP WMI camera shutter"; camera_shutter_input_dev->phys = "wmi/input1"; camera_shutter_input_dev->id.bustype = BUS_HOST;
	__set_bit(EV_SW, camera_shutter_input_dev->evbit); __set_bit(SW_CAMERA_LENS_COVER, camera_shutter_input_dev->swbit);
	err = input_register_device(camera_shutter_input_dev); if (err) goto err_free_dev; return 0;
err_free_dev: input_free_device(camera_shutter_input_dev); camera_shutter_input_dev = NULL; return err;
}
static bool hp_wmi_get_hw_state(enum hp_wmi_radio r) {
	int m = 0x800 << (r * 8), w = hp_wmi_read_int(HPWMI_WIRELESS_QUERY); if (w < 0) { WARN_ONCE(1, "err HPWMI_WIRELESS_QUERY HW"); return true; } return !(w & m);
}
static int hp_wmi_rfkill2_set_block(void *data, bool blocked) {
	int id = (int)(uintptr_t)data; char b[4] = {0x01, 0x00, id, !blocked};
	int ret = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_WRITE, b, sizeof(b), 0); return ret <= 0 ? ret : -EINVAL;
}
static const struct rfkill_ops hp_wmi_rfkill2_ops = { .set_block = hp_wmi_rfkill2_set_block };
static int hp_wmi_rfkill2_refresh(void) {
	struct bios_rfkill2_state s = {0}; int err, i; err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &s, sizeof(s), sizeof(s));
	if (err) return err < 0 ? err : -EINVAL;
	for (i = 0; i < rfkill2_count; i++) { int num = rfkill2[i].num; struct bios_rfkill2_device_state *ds;
		if (num >= s.count) { pr_warn("rfkill idx %d OOB (%d)\n", num, s.count); continue; } ds = &s.device[num];
		if (ds->rfkill_id != rfkill2[i].id) { pr_warn("rfkill config changed\n"); continue; }
		if (rfkill2[i].rfkill) rfkill_set_states(rfkill2[i].rfkill, IS_SWBLOCKED(ds->power), IS_HWBLOCKED(ds->power));
	} return 0;
}
static ssize_t display_show(struct device *d, struct device_attribute *a, char *b) { int v = hp_wmi_read_int(HPWMI_DISPLAY_QUERY); return v < 0 ? v : sysfs_emit(b, "%d\n", v); }
static ssize_t hddtemp_show(struct device *d, struct device_attribute *a, char *b) { int v = hp_wmi_read_int(HPWMI_HDDTEMP_QUERY); return v < 0 ? v : sysfs_emit(b, "%d\n", v); }
static ssize_t als_show(struct device *d, struct device_attribute *a, char *b) { int v = hp_wmi_read_int(HPWMI_ALS_QUERY); return v < 0 ? v : sysfs_emit(b, "%d\n", v); }
static ssize_t dock_show(struct device *d, struct device_attribute *a, char *b) { int v = hp_wmi_hw_state(HPWMI_DOCK_MASK); return v < 0 ? v : sysfs_emit(b, "%d\n", v); }
static ssize_t tablet_show(struct device *d, struct device_attribute *a, char *b) { int v = hp_wmi_hw_state(HPWMI_TABLET_MASK); return v < 0 ? v : sysfs_emit(b, "%d\n", v); }
static ssize_t postcode_show(struct device *d, struct device_attribute *a, char *b) { int v = hp_wmi_read_int(HPWMI_POSTCODEERROR_QUERY); return v < 0 ? v : sysfs_emit(b, "0x%x\n", v); }
static ssize_t als_store(struct device *d, struct device_attribute *a, const char *b, size_t c) { u32 t; int r = kstrtou32(b, 10, &t); if (r) return r; r = hp_wmi_perform_query(HPWMI_ALS_QUERY, HPWMI_WRITE, &t, sizeof(t), 0); return r ? (r < 0 ? r : -EINVAL) : c; }
static ssize_t postcode_store(struct device *d, struct device_attribute *a, const char *b, size_t c) { u32 t = 1; bool cl; int r = kstrtobool(b, &cl); if (r) return r; if (!cl) return -EINVAL; r = hp_wmi_perform_query(HPWMI_POSTCODEERROR_QUERY, HPWMI_WRITE, &t, sizeof(t), 0); return r ? (r < 0 ? r : -EINVAL) : c; }
static DEVICE_ATTR_RO(display); static DEVICE_ATTR_RO(hddtemp); static DEVICE_ATTR_RW(als);
static DEVICE_ATTR_RO(dock); static DEVICE_ATTR_RO(tablet); static DEVICE_ATTR_RW(postcode);
static struct attribute *hp_wmi_attrs[] = {&dev_attr_display.attr, &dev_attr_hddtemp.attr, &dev_attr_als.attr, &dev_attr_dock.attr, &dev_attr_tablet.attr, &dev_attr_postcode.attr, NULL};
ATTRIBUTE_GROUPS(hp_wmi);
static int hp_wmi_get_dock_state(void) { int s = hp_wmi_read_int(HPWMI_HARDWARE_QUERY); return (s < 0) ? s : !!(s & HPWMI_DOCK_MASK); }
static int hp_wmi_get_tablet_mode(void) {
	char sdm[4] = {0}; const char *ct = dmi_get_system_info(DMI_CHASSIS_TYPE); int r;
	if (!ct) return -ENODEV;
	if (match_string(tablet_chassis_types, ARRAY_SIZE(tablet_chassis_types), ct) < 0) return -ENODEV;
	r = hp_wmi_perform_query(HPWMI_SYSTEM_DEVICE_MODE, HPWMI_READ, sdm, 0, sizeof(sdm)); return r < 0 ? r : (sdm[0] == DEVICE_MODE_TABLET);
}
static void hp_wmi_notify(union acpi_object *obj, void *context) {
	u32 eid, edata, *loc; int kc, tm;
	if (!obj || obj->type != ACPI_TYPE_BUFFER) { pr_info("Unk WMI evt type %d\n", obj ? obj->type : -1); return; }
	loc = (u32 *)obj->buffer.pointer;
	if (obj->buffer.length == 8) { eid = loc[0]; edata = loc[1]; }
	else if (obj->buffer.length == 16) { eid = loc[0]; edata = loc[2]; }
	else { pr_info("Unk WMI evt len %d\n", obj->buffer.length); return; }

	switch (eid) {
	case HPWMI_DOCK_EVENT:
		if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_DOCK, hp_wmi_get_dock_state());
		if (enable_tablet_mode_sw != 0) {
			tm = hp_wmi_get_tablet_mode();
			if (tm >= 0) {
				if (!test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit))
					__set_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit);
				input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, tm);
			} else if (enable_tablet_mode_sw == 1) {
				pr_warn("Fail tablet mode SW_TABLET_MODE: %d\n", tm);
			}
		}
		input_sync(hp_wmi_input_dev);
		break;
	case HPWMI_PARK_HDD: break;
	case HPWMI_SMART_ADAPTER: pr_debug("Smart Adapter evt:0x%x\n", edata); break;
	case HPWMI_BEZEL_BUTTON:
		kc = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		if (kc < 0 || (kc & HPWMI_HOTKEY_RELEASE_FLAG)) break;
		if (!sparse_keymap_report_event(hp_wmi_input_dev, kc, 1, true))
			pr_info("Unk Bezel key:0x%x\n", kc);
		break;
	case HPWMI_OMEN_KEY:
		if (edata != 0 && edata != 0xFFFFFFFF) kc = edata;
		else kc = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		if (kc < 0 || (kc & HPWMI_HOTKEY_RELEASE_FLAG)) break;
		if (!sparse_keymap_report_event(hp_wmi_input_dev, kc, 1, true))
			pr_info("Unk Omen key:0x%x (edata:0x%x)\n", kc, edata);
		break;
	case HPWMI_WIRELESS:
		if (rfkill2_count) { hp_wmi_rfkill2_refresh(); break; }
		if (wifi_rfkill) rfkill_set_states(wifi_rfkill, hp_wmi_get_sw_state(HPWMI_WIFI), hp_wmi_get_hw_state(HPWMI_WIFI));
		if (bluetooth_rfkill) rfkill_set_states(bluetooth_rfkill, hp_wmi_get_sw_state(HPWMI_BLUETOOTH), hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		if (wwan_rfkill) rfkill_set_states(wwan_rfkill, hp_wmi_get_sw_state(HPWMI_WWAN), hp_wmi_get_hw_state(HPWMI_WWAN));
		break;
	case HPWMI_CPU_BATTERY_THROTTLE: pr_info("CPU throttle evt (data:0x%x)\n", edata); break;
	case HPWMI_LOCK_SWITCH: pr_debug("Lock switch evt:0x%x\n", edata); break;
	case HPWMI_LID_SWITCH: pr_debug("Lid evt:0x%x\n", edata); break;
	case HPWMI_SCREEN_ROTATION: pr_debug("Screen rotation evt:0x%x\n", edata); break;
	case HPWMI_COOLSENSE_SYSTEM_MOBILE: case HPWMI_COOLSENSE_SYSTEM_HOT: pr_debug("Coolsense evt:ID 0x%x,Data 0x%x\n", eid, edata); break;
	case HPWMI_PROXIMITY_SENSOR: pr_debug("Proximity evt:0x%x\n", edata); break;
	case HPWMI_BACKLIT_KB_BRIGHTNESS: pr_debug("KB backlight evt:0x%x\n", edata); break;
	case HPWMI_PEAKSHIFT_PERIOD: case HPWMI_BATTERY_CHARGE_PERIOD: pr_debug("Battery evt:ID 0x%x,Data 0x%x\n", eid, edata); break;
	case HPWMI_SANITIZATION_MODE: pr_info("Sanitization evt:0x%x\n", edata); break;
	case HPWMI_CAMERA_TOGGLE:
		if (!camera_shutter_input_dev) {
			if (camera_shutter_input_setup())
				pr_err("Fail cam shutter setup\n");
		}
		if (camera_shutter_input_dev) {
			if (edata == 0xff) input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 1);
			else if (edata == 0xfe) input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 0);
			else pr_warn("Unk cam shutter state:0x%x\n", edata);
			input_sync(camera_shutter_input_dev);
		}
		break;
	case HPWMI_SMART_EXPERIENCE_APP: pr_info("Smart Exp App evt:0x%x\n", edata); break;
	default: pr_info("Unk WMI evt_id:0x%x,data:0x%x\n", eid, edata); break;
	}
}
static int __init hp_wmi_input_setup(void) {
	acpi_status status; int err, val, tablet_mode;
	hp_wmi_input_dev = input_allocate_device(); if (!hp_wmi_input_dev) return -ENOMEM;
	hp_wmi_input_dev->name = "HP WMI hotkeys"; hp_wmi_input_dev->phys = "wmi/input0"; hp_wmi_input_dev->id.bustype = BUS_HOST;
	err = sparse_keymap_setup(hp_wmi_input_dev, hp_wmi_keymap, NULL); if (err) goto err_free_dev;
	__set_bit(EV_SW, hp_wmi_input_dev->evbit);
	val = hp_wmi_get_dock_state(); if (val >= 0) { __set_bit(SW_DOCK, hp_wmi_input_dev->swbit); input_report_switch(hp_wmi_input_dev, SW_DOCK, val); } else pr_warn("Fail init dock state:%d\n", val);
	if (enable_tablet_mode_sw != 0) { tablet_mode = hp_wmi_get_tablet_mode(); if (tablet_mode >= 0) { __set_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit); input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, tablet_mode); } else if (enable_tablet_mode_sw == 1) pr_warn("Fail init tablet SW_TABLET_MODE:%d\n", tablet_mode); }
	input_sync(hp_wmi_input_dev);
	if (!hp_wmi_bios_2009_later() && hp_wmi_bios_2008_later()) { err = hp_wmi_enable_hotkeys(); if (err) pr_warn("Fail enable hotkeys:%d\n", err); }
	status = wmi_install_notify_handler(HPWMI_EVENT_GUID, hp_wmi_notify, NULL); if (ACPI_FAILURE(status)) { pr_err("Fail WMI notify handler:0x%x\n", status); err = -EIO; goto err_free_keymap_and_dev; } // Zmieniona etykieta
	err = input_register_device(hp_wmi_input_dev); if (err) goto err_uninstall_notifier;
	return 0; // Sukces
err_uninstall_notifier: wmi_remove_notify_handler(HPWMI_EVENT_GUID);
err_free_keymap_and_dev: // Etykieta dla czyszczenia mapy klawiszy (jeśli sparse_keymap_setup się powiodło) i urządzenia
	// sparse_keymap_free(hp_wmi_input_dev); // Usuwamy, bo nie istnieje
err_free_dev: input_free_device(hp_wmi_input_dev); hp_wmi_input_dev = NULL; return err;
}
static void hp_wmi_input_destroy(void) {
	if (hp_wmi_input_dev) { wmi_remove_notify_handler(HPWMI_EVENT_GUID); input_unregister_device(hp_wmi_input_dev); hp_wmi_input_dev = NULL; }
}
static int __init hp_wmi_rfkill_setup(struct platform_device *device) {
	int err = 0, wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY); if (wireless < 0) { pr_warn("Fail read wireless query rfkill:%d\n", wireless); return wireless; }
	if (wireless & 1) { wifi_rfkill = rfkill_alloc("hp-wifi", &device->dev, RFKILL_TYPE_WLAN, &hp_wmi_rfkill_ops, (void *)(uintptr_t)HPWMI_WIFI); if (!wifi_rfkill) { err = -ENOMEM; goto err_cleanup; } rfkill_set_states(wifi_rfkill, hp_wmi_get_sw_state(HPWMI_WIFI), hp_wmi_get_hw_state(HPWMI_WIFI)); err = rfkill_register(wifi_rfkill); if (err) goto err_cleanup; }
	if (wireless & 2) { bluetooth_rfkill = rfkill_alloc("hp-bluetooth", &device->dev, RFKILL_TYPE_BLUETOOTH, &hp_wmi_rfkill_ops, (void *)(uintptr_t)HPWMI_BLUETOOTH); if (!bluetooth_rfkill) { err = -ENOMEM; goto err_cleanup; } rfkill_set_states(bluetooth_rfkill, hp_wmi_get_sw_state(HPWMI_BLUETOOTH), hp_wmi_get_hw_state(HPWMI_BLUETOOTH)); err = rfkill_register(bluetooth_rfkill); if (err) goto err_cleanup; }
	if (wireless & 4) { wwan_rfkill = rfkill_alloc("hp-wwan", &device->dev, RFKILL_TYPE_WWAN, &hp_wmi_rfkill_ops, (void *)(uintptr_t)HPWMI_WWAN); if (!wwan_rfkill) { err = -ENOMEM; goto err_cleanup; } rfkill_set_states(wwan_rfkill, hp_wmi_get_sw_state(HPWMI_WWAN), hp_wmi_get_hw_state(HPWMI_WWAN)); err = rfkill_register(wwan_rfkill); if (err) goto err_cleanup; }
	return 0;
err_cleanup: if (wwan_rfkill) { rfkill_unregister(wwan_rfkill); rfkill_destroy(wwan_rfkill); wwan_rfkill = NULL; } if (bluetooth_rfkill) { rfkill_unregister(bluetooth_rfkill); rfkill_destroy(bluetooth_rfkill); bluetooth_rfkill = NULL; } if (wifi_rfkill) { rfkill_unregister(wifi_rfkill); rfkill_destroy(wifi_rfkill); wifi_rfkill = NULL; } return err;
}
static int __init hp_wmi_rfkill2_setup(struct platform_device *device) {
	struct bios_rfkill2_state s = {0}; int err = 0, i; err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &s, sizeof(s), sizeof(s)); if (err) return err < 0 ? err : -EINVAL;
	if (s.count > HPWMI_MAX_RFKILL2_DEVICES) { pr_warn("rfkill2 count %d > max %d\n", s.count, HPWMI_MAX_RFKILL2_DEVICES); return -EINVAL; }
	for (i = 0; i < s.count; i++) { struct rfkill *rd; enum rfkill_type t; const char *n;
		switch (s.device[i].radio_type) { case HPWMI_WIFI:t = RFKILL_TYPE_WLAN; n = "hp-wifi2"; break; case HPWMI_BLUETOOTH:t = RFKILL_TYPE_BLUETOOTH; n = "hp-bluetooth2"; break; case HPWMI_WWAN:t = RFKILL_TYPE_WWAN; n = "hp-wwan2"; break; case HPWMI_GPS:t = RFKILL_TYPE_GPS; n = "hp-gps2"; break; default:pr_warn("unk rfkill2 type 0x%x\n", s.device[i].radio_type); continue; }
		if (!s.device[i].vendor_id && !s.device[i].product_id) { pr_warn("rfkill2 dev %d zero vid/pid (type 0x%x)\n", i, s.device[i].radio_type); continue; }
		rd = rfkill_alloc(n, &device->dev, t, &hp_wmi_rfkill2_ops, (void *)(uintptr_t)i); if (!rd) { err = -ENOMEM; goto fail; }
		rfkill2[rfkill2_count].id = s.device[i].rfkill_id; rfkill2[rfkill2_count].num = i; rfkill2[rfkill2_count].rfkill = rd;
		rfkill_set_states(rd, IS_SWBLOCKED(s.device[i].power), IS_HWBLOCKED(s.device[i].power));
		if (!(s.device[i].power & HPWMI_POWER_BIOS)) pr_info("rfkill2 dev %s (type 0x%x) blocked by BIOS\n", n, s.device[i].radio_type);
		err = rfkill_register(rd); if (err) { rfkill_destroy(rd); rfkill2[rfkill2_count].rfkill = NULL; goto fail; } rfkill2_count++;
	} return 0;
fail: while (--rfkill2_count >= 0) { if (rfkill2[rfkill2_count].rfkill) { rfkill_unregister(rfkill2[rfkill2_count].rfkill); rfkill_destroy(rfkill2[rfkill2_count].rfkill); rfkill2[rfkill2_count].rfkill = NULL; } } rfkill2_count = 0; return err;
}
static int platform_profile_omen_get(struct device *d, enum platform_profile_option *p) { int t = omen_thermal_profile_get(); if (t < 0)return t; switch (t) { case HP_OMEN_THERMAL_PROFILE_PERFORMANCE:*p = PLATFORM_PROFILE_PERFORMANCE; break; case HP_OMEN_THERMAL_PROFILE_DEFAULT:*p = PLATFORM_PROFILE_BALANCED; break; case HP_OMEN_THERMAL_PROFILE_COOL:*p = PLATFORM_PROFILE_COOL; break; default:pr_warn("Unk Omen EC profile:%d\n", t); return -EINVAL; } return 0; }
static int platform_profile_omen_set(struct device *d, enum platform_profile_option p) { int t; switch (p) { case PLATFORM_PROFILE_PERFORMANCE:t = HP_OMEN_THERMAL_PROFILE_PERFORMANCE; break; case PLATFORM_PROFILE_BALANCED:t = HP_OMEN_THERMAL_PROFILE_DEFAULT; break; case PLATFORM_PROFILE_COOL:t = HP_OMEN_THERMAL_PROFILE_COOL; break; default:return -EINVAL; } return omen_thermal_profile_set(t); }
static int generic_thermal_profile_get_wmi(void) { return hp_wmi_read_int(HPWMI_THERMAL_PROFILE_QUERY); }
static int generic_thermal_profile_set_wmi(int tp) { if (tp < 0 || tp > 2)return -EINVAL; return hp_wmi_perform_query(HPWMI_THERMAL_PROFILE_QUERY, HPWMI_WRITE, &tp, sizeof(tp), 0); }
static int hp_wmi_platform_profile_get(struct device *d, enum platform_profile_option *p) { int t = generic_thermal_profile_get_wmi(); if (t < 0)return t; switch (t) { case HP_THERMAL_PROFILE_PERFORMANCE:*p = PLATFORM_PROFILE_PERFORMANCE; break; case HP_THERMAL_PROFILE_DEFAULT:*p = PLATFORM_PROFILE_BALANCED; break; case HP_THERMAL_PROFILE_COOL:*p = PLATFORM_PROFILE_COOL; break; default:pr_warn("Unk generic WMI profile:%d\n", t); return -EINVAL; } return 0; }
static int hp_wmi_platform_profile_set(struct device *d, enum platform_profile_option p) { int t; switch (p) { case PLATFORM_PROFILE_PERFORMANCE:t = HP_THERMAL_PROFILE_PERFORMANCE; break; case PLATFORM_PROFILE_BALANCED:t = HP_THERMAL_PROFILE_DEFAULT; break; case PLATFORM_PROFILE_COOL:t = HP_THERMAL_PROFILE_COOL; break; default:return -EINVAL; } return generic_thermal_profile_set_wmi(t); }

#define FOURZONE_COUNT 4
struct color_platform { u8 b; u8 g; u8 r; } __packed;
struct platform_zone { u8 offset; struct device_attribute *attr; struct color_platform colors; char *name_ptr; };
static struct device_attribute *zone_dev_attrs; static struct attribute **zone_attrs; static struct platform_zone *zone_data;
static struct attribute_group zone_attribute_group = {.name = "rgb_zones"};
static int parse_rgb(const char *buf, struct color_platform *c) { unsigned long v; int r = kstrtoul(buf, 16, &v); if (r)return r; if (v > 0xFFFFFF)return -EINVAL; sscanf(buf, "%2hhx%2hhx%2hhx", &c->r, &c->g, &c->b); pr_debug("hp-wmi:parsed r:%d g:%d b:%d\n", c->r, c->g, c->b); return 0; }
static struct platform_zone *match_zone_by_attr(struct device_attribute *a) { int i; if (!zone_data || !zone_dev_attrs)return NULL; for (i = 0; i < FOURZONE_COUNT; i++)if (&zone_dev_attrs[i] == a)return &zone_data[i]; return NULL; }
static int fourzone_update_led(struct platform_zone *z, bool read_op) { u8 s[128] = {0}; int r = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_GET, HPWMI_FOURZONE, s, 0, sizeof(s)); if (r) { pr_warn("fourzone_get err 0x%x\n", r); return r < 0 ? r : -EIO; } if (read_op) { z->colors.r = s[z->offset + 0]; z->colors.g = s[z->offset + 1]; z->colors.b = s[z->offset + 2]; } else { s[z->offset + 0] = z->colors.r; s[z->offset + 1] = z->colors.g; s[z->offset + 2] = z->colors.b; r = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_SET, HPWMI_FOURZONE, s, sizeof(s), 0); if (r) { pr_warn("fourzone_set err 0x%x\n", r); return r < 0 ? r : -EIO; } } return 0; }
static ssize_t zone_show(struct device *d, struct device_attribute *a, char *b) { struct platform_zone *tz = match_zone_by_attr(a); int r; if (!tz)return -EINVAL; r = fourzone_update_led(tz, true); if (r)return sysfs_emit(b, "Err read zone:%d\n", r); return sysfs_emit(b, "RGB:%02x%02x%02x (R:%d G:%d B:%d)\n", tz->colors.r, tz->colors.g, tz->colors.b, tz->colors.r, tz->colors.g, tz->colors.b); }
static ssize_t zone_set(struct device *d, struct device_attribute *a, const char *buf, size_t count) { struct platform_zone *tz = match_zone_by_attr(a); struct color_platform nc; int r; if (!tz)return -EINVAL; r = parse_rgb(buf, &nc); if (r)return r; tz->colors = nc; r = fourzone_update_led(tz, false); return r ? r : count; }
static int fourzone_setup(struct platform_device *pdev) {
	int zi, err = 0; char nb[16]; if (!quirks || !quirks->fourzone)return 0;
	zone_dev_attrs = kcalloc(FOURZONE_COUNT, sizeof(*zone_dev_attrs), GFP_KERNEL); if (!zone_dev_attrs)return -ENOMEM;
	zone_attrs = kcalloc(FOURZONE_COUNT + 1, sizeof(*zone_attrs), GFP_KERNEL); if (!zone_attrs) { kfree(zone_dev_attrs); zone_dev_attrs = NULL; return -ENOMEM; }
	zone_data = kcalloc(FOURZONE_COUNT, sizeof(*zone_data), GFP_KERNEL); if (!zone_data) { kfree(zone_attrs); zone_attrs = NULL; kfree(zone_dev_attrs); zone_dev_attrs = NULL; return -ENOMEM; }
	for (zi = 0; zi < FOURZONE_COUNT; zi++) { snprintf(nb, sizeof(nb), "zone%02X_rgb", zi); zone_data[zi].name_ptr = kstrdup(nb, GFP_KERNEL); if (!zone_data[zi].name_ptr) { err = -ENOMEM; goto err_fourzone; } sysfs_attr_init(&zone_dev_attrs[zi].attr); zone_dev_attrs[zi].attr.name = zone_data[zi].name_ptr; zone_dev_attrs[zi].attr.mode = 0644; zone_dev_attrs[zi].show = zone_show; zone_dev_attrs[zi].store = zone_set; zone_data[zi].offset = 25 + (zi * 3); zone_data[zi].attr = &zone_dev_attrs[zi]; zone_attrs[zi] = &zone_dev_attrs[zi].attr; }
	zone_attrs[FOURZONE_COUNT] = NULL; zone_attribute_group.attrs = zone_attrs; err = sysfs_create_group(&pdev->dev.kobj, &zone_attribute_group); if (err)goto err_fourzone; return 0;
err_fourzone: for (zi--; zi >= 0; zi--)kfree(zone_data[zi].name_ptr); kfree(zone_data); zone_data = NULL; kfree(zone_attrs); zone_attrs = NULL; kfree(zone_dev_attrs); zone_dev_attrs = NULL; return err;
}
static void fourzone_remove(struct platform_device *pdev) {
	int i; if (quirks && quirks->fourzone && zone_attribute_group.attrs) { sysfs_remove_group(&pdev->dev.kobj, &zone_attribute_group); if (zone_data)for (i = 0; i < FOURZONE_COUNT; i++)kfree(zone_data[i].name_ptr); kfree(zone_data); zone_data = NULL; kfree(zone_attrs); zone_attrs = NULL; kfree(zone_dev_attrs); zone_dev_attrs = NULL; zone_attribute_group.attrs = NULL; }
}
static int thermal_profile_setup(void) {
	struct device *dev = &hp_wmi_platform_dev->dev;
	int err_check, tp_val;
	unsigned long choices_mask = 0;
	struct platform_profile_ops *selected_ops;

	set_bit(PLATFORM_PROFILE_COOL, &choices_mask);
	set_bit(PLATFORM_PROFILE_BALANCED, &choices_mask);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, &choices_mask);

	if (is_omen_thermal_profile()) {
		tp_val = omen_thermal_profile_get();
		if (tp_val < 0) { pr_warn("Fail get Omen profile:%d\n", tp_val); return tp_val; }
		err_check = omen_thermal_profile_set(tp_val);
		if (err_check < 0) { pr_warn("Fail set init Omen profile:%d\n", err_check); return err_check; }
		hp_wmi_omen_profile_ops.profile_get = platform_profile_omen_get;
		hp_wmi_omen_profile_ops.profile_set = platform_profile_omen_set;
		selected_ops = &hp_wmi_omen_profile_ops;
	} else {
		tp_val = generic_thermal_profile_get_wmi();
		if (tp_val < 0) { pr_warn("Fail get generic profile:%d\n", tp_val); return tp_val; }
		err_check = generic_thermal_profile_set_wmi(tp_val);
		if (err_check < 0) { pr_warn("Fail set init generic profile(sys err):%d\n", err_check); return err_check; }
		else if (err_check > 0) { pr_warn("WMI err %d on generic profile set\n", err_check); return -EIO; }
		hp_wmi_profile_ops.profile_get = hp_wmi_platform_profile_get;
		hp_wmi_profile_ops.profile_set = hp_wmi_platform_profile_set;
		selected_ops = &hp_wmi_profile_ops;
	}

	platform_profile_dev = platform_profile_register(dev, "hp-wmi", &choices_mask, selected_ops);
	if (IS_ERR(platform_profile_dev)) {
		pr_err("Fail register platform profile:%ld\n", PTR_ERR(platform_profile_dev));
		platform_profile_dev = NULL;
		return PTR_ERR(platform_profile_dev);
	}
	platform_profile_support = true;
	return 0;
}
static int hp_wmi_hwmon_init(void);
static int __init hp_wmi_bios_setup(struct platform_device *device) {
	int err; wifi_rfkill = NULL; bluetooth_rfkill = NULL; wwan_rfkill = NULL; rfkill2_count = 0;
	if (hp_wmi_rfkill_setup(device) != 0) { pr_info("Legacy rfkill issues, try rfkill2\n"); err = hp_wmi_rfkill2_setup(device); if (err)pr_warn("rfkill2_setup fail:%d\n", err); }
	err = fourzone_setup(device); if (err)pr_err("Fail FourZone setup:%d\n", err);
	err = hp_wmi_hwmon_init(); if (err)pr_err("Fail HWMON init:%d\n", err);
	err = thermal_profile_setup(); if (err)pr_err("Fail thermal profile setup:%d\n", err);
	return 0;
}
static void __exit hp_wmi_bios_remove(struct platform_device *device) {
	int i; fourzone_remove(device);
	if (platform_profile_support && platform_profile_dev) { platform_profile_remove(platform_profile_dev); platform_profile_support = false; platform_profile_dev = NULL; }
	for (i = 0; i < rfkill2_count; i++) {
		if (rfkill2[i].rfkill) {
			rfkill_unregister(rfkill2[i].rfkill);
			rfkill_destroy(rfkill2[i].rfkill);
			rfkill2[i].rfkill = NULL;
		}
	}
	rfkill2_count = 0; // To jest OK, jeśli for był pusty
	if (wifi_rfkill) { rfkill_unregister(wifi_rfkill); rfkill_destroy(wifi_rfkill); wifi_rfkill = NULL; }
	if (bluetooth_rfkill) { rfkill_unregister(bluetooth_rfkill); rfkill_destroy(bluetooth_rfkill); bluetooth_rfkill = NULL; }
	if (wwan_rfkill) { rfkill_unregister(wwan_rfkill); rfkill_destroy(wwan_rfkill); wwan_rfkill = NULL; }
}
static int hp_wmi_resume_handler(struct device *d) {
	int ds, ts; if (hp_wmi_input_dev) { if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit)) { ds = hp_wmi_get_dock_state(); if (ds >= 0)input_report_switch(hp_wmi_input_dev, SW_DOCK, ds); } if (test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit) && enable_tablet_mode_sw != 0) { ts = hp_wmi_get_tablet_mode(); if (ts >= 0)input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, ts); } input_sync(hp_wmi_input_dev); }
	if (rfkill2_count)hp_wmi_rfkill2_refresh(); else { if (wifi_rfkill)rfkill_set_states(wifi_rfkill, hp_wmi_get_sw_state(HPWMI_WIFI), hp_wmi_get_hw_state(HPWMI_WIFI)); if (bluetooth_rfkill)rfkill_set_states(bluetooth_rfkill, hp_wmi_get_sw_state(HPWMI_BLUETOOTH), hp_wmi_get_hw_state(HPWMI_BLUETOOTH)); if (wwan_rfkill)rfkill_set_states(wwan_rfkill, hp_wmi_get_sw_state(HPWMI_WWAN), hp_wmi_get_hw_state(HPWMI_WWAN)); }
	return 0;
}
static const struct dev_pm_ops hp_wmi_pm_ops = { .resume = hp_wmi_resume_handler, .restore = hp_wmi_resume_handler };
static struct platform_driver hp_wmi_driver = { .driver = {.name = "hp-wmi", .pm = &hp_wmi_pm_ops, .dev_groups = hp_wmi_groups}, .probe = hp_wmi_bios_setup, .remove = __exit_p(hp_wmi_bios_remove) };
static umode_t hp_wmi_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr, int channel) {
	switch (type) { case hwmon_pwm:if (attr == hwmon_pwm_enable)return 0644; break; case hwmon_fan:if (attr == hwmon_fan_input)if (hp_wmi_get_fan_speed(channel) >= 0)return 0444; break; default:break; } return 0;
}
static int hp_wmi_hwmon_read(struct device *d, enum hwmon_sensor_types type, u32 attr, int channel, long *val) {
	int r; switch (type) { case hwmon_fan:if (attr == hwmon_fan_input) { r = hp_wmi_get_fan_speed(channel); if (r < 0)return r; *val = r; return 0; } break; case hwmon_pwm:if (attr == hwmon_pwm_enable) { r = hp_wmi_fan_speed_max_get(); if (r < 0)return r; if (r == 0)*val = 2; else if (r == 1)*val = 0; else return -ENODATA; return 0; } break; default:break; } return -EOPNOTSUPP;
}
static int hp_wmi_hwmon_write(struct device *d, enum hwmon_sensor_types type, u32 attr, int channel, long val) {
	if (type == hwmon_pwm && attr == hwmon_pwm_enable) { if (val == 2)return hp_wmi_fan_speed_max_set(0); if (val == 0)return hp_wmi_fan_speed_max_set(1); return -EINVAL; } return -EOPNOTSUPP;
}
static const struct hwmon_channel_info * const hp_wmi_hwmon_info[] = { HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT), HWMON_CHANNEL_INFO(pwm, HWMON_PWM_ENABLE), NULL};
static const struct hwmon_ops hp_wmi_hwmon_ops = {.is_visible = hp_wmi_hwmon_is_visible, .read = hp_wmi_hwmon_read, .write = hp_wmi_hwmon_write};
static const struct hwmon_chip_info hp_wmi_hwmon_chip_info = {.ops = &hp_wmi_hwmon_ops, .info = hp_wmi_hwmon_info};
static int hp_wmi_hwmon_init(void) {
	struct device *hd; if (!hp_wmi_platform_dev) { pr_err("HWMON:hp_wmi_platform_dev NULL\n"); return -ENODEV; }
	hd = devm_hwmon_device_register_with_info(&hp_wmi_platform_dev->dev, "hp_wmi", NULL, &hp_wmi_hwmon_chip_info, NULL);
	if (IS_ERR(hd)) { pr_err("Fail register hp_wmi hwmon:%ld\n", PTR_ERR(hd)); return PTR_ERR(hd); } return 0;
}
static int __init hp_wmi_init(void) {
	int err; bool ec = wmi_has_guid(HPWMI_EVENT_GUID), bc = wmi_has_guid(HPWMI_BIOS_GUID);
	if (!ec && !bc) { pr_info("No HP WMI interface\n"); return -ENODEV; }
	if (ec) { err = hp_wmi_input_setup(); if (err) { pr_err("HP WMI input setup fail:%d\n", err); return err; } }
	if (bc) { hp_wmi_platform_dev = platform_device_register_simple("hp-wmi", -1, NULL, 0); if (IS_ERR(hp_wmi_platform_dev)) { err = PTR_ERR(hp_wmi_platform_dev); pr_err("Fail register hp-wmi pdev:%d\n", err); goto err_destroy_input; }
		err = platform_driver_register(&hp_wmi_driver); if (err) { pr_err("Fail register hp-wmi pdrv:%d\n", err); goto err_unregister_pdev; }
	} pr_info("HP WMI driver init (evt:%d,bios:%d)\n", ec, bc); return 0;
err_unregister_pdev: platform_device_unregister(hp_wmi_platform_dev); hp_wmi_platform_dev = NULL;
err_destroy_input: if (ec)hp_wmi_input_destroy(); if (camera_shutter_input_dev) { input_unregister_device(camera_shutter_input_dev); camera_shutter_input_dev = NULL; } return err;
}
module_init(hp_wmi_init);
static void __exit hp_wmi_exit(void) {
	if (wmi_has_guid(HPWMI_BIOS_GUID) && hp_wmi_platform_dev) { platform_driver_unregister(&hp_wmi_driver); platform_device_unregister(hp_wmi_platform_dev); hp_wmi_platform_dev = NULL; }
	if (wmi_has_guid(HPWMI_EVENT_GUID)) { hp_wmi_input_destroy(); if (camera_shutter_input_dev) { input_unregister_device(camera_shutter_input_dev); camera_shutter_input_dev = NULL; } }
	pr_info("HP WMI driver unloaded\n");
}
module_exit(hp_wmi_exit);