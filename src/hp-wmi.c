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
#include <linux/platform_profile.h> // Keep this include
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

/* DMI board names of devices that should use the omen specific path for
 * thermal profiles.
 */
static const char * const omen_thermal_profile_boards[] = {
	"84DA", "84DB", "84DC", "8574", "8575", "860A", "87B5", "8572", "8573",
	"8600", "8601", "8602", "8605", "8606", "8607", "8746", "8747", "8749",
	"874A", "8603", "8604", "8748", "886B", "886C", "878A", "878B", "878C",
	"88C8", "88CB", "8786", "8787", "8788", "88D1", "88D2", "88F4", "88FD",
	"88F5", "88F6", "88F7", "88FE", "88FF", "8900", "8901", "8902", "8912",
	"8917", "8918", "8949", "894A", "89EB"
};

enum hp_wmi_radio {
	HPWMI_WIFI	= 0x0,
	HPWMI_BLUETOOTH	= 0x1,
	HPWMI_WWAN	= 0x2,
	HPWMI_GPS	= 0x3,
};

enum hp_wmi_event_ids {
	HPWMI_DOCK_EVENT		= 0x01,
	HPWMI_PARK_HDD			= 0x02,
	HPWMI_SMART_ADAPTER		= 0x03,
	HPWMI_BEZEL_BUTTON		= 0x04,
	HPWMI_WIRELESS			= 0x05,
	HPWMI_CPU_BATTERY_THROTTLE	= 0x06,
	HPWMI_LOCK_SWITCH		= 0x07,
	HPWMI_LID_SWITCH		= 0x08,
	HPWMI_SCREEN_ROTATION		= 0x09,
	HPWMI_COOLSENSE_SYSTEM_MOBILE	= 0x0A,
	HPWMI_COOLSENSE_SYSTEM_HOT	= 0x0B,
	HPWMI_PROXIMITY_SENSOR		= 0x0C,
	HPWMI_BACKLIT_KB_BRIGHTNESS	= 0x0D,
	HPWMI_PEAKSHIFT_PERIOD		= 0x0F,
	HPWMI_BATTERY_CHARGE_PERIOD	= 0x10,
	HPWMI_SANITIZATION_MODE		= 0x17,
	HPWMI_CAMERA_TOGGLE		= 0x1A,

	HPWMI_OMEN_KEY      	= 0x1D,
	HPWMI_SMART_EXPERIENCE_APP	= 0x21,
};

struct bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[128];
};

enum hp_wmi_commandtype {
	HPWMI_DISPLAY_QUERY		= 0x01,
	HPWMI_HDDTEMP_QUERY		= 0x02,
	HPWMI_ALS_QUERY			= 0x03,
	HPWMI_HARDWARE_QUERY		= 0x04,
	HPWMI_WIRELESS_QUERY		= 0x05,
	HPWMI_BATTERY_QUERY		= 0x07,
	HPWMI_BIOS_QUERY		= 0x09,
	HPWMI_FEATURE_QUERY		= 0x0b,
	HPWMI_HOTKEY_QUERY		= 0x0c,
	HPWMI_FEATURE2_QUERY		= 0x0d,
	HPWMI_WIRELESS2_QUERY		= 0x1b,
	HPWMI_POSTCODEERROR_QUERY	= 0x2a,
	HPWMI_THERMAL_PROFILE_QUERY	= 0x4c,
	HPWMI_SYSTEM_DEVICE_MODE	= 0x40,

	HPWMI_FOURZONE_COLOR_GET = 2,
	HPWMI_FOURZONE_COLOR_SET = 3,
	HPWMI_FOURZONE_BRIGHT_GET = 4,
	HPWMI_FOURZONE_BRIGHT_SET = 5,
	HPWMI_FOURZONE_ANIM_GET = 6,
	HPWMI_FOURZONE_ANIM_SET = 7,
};

enum hp_wmi_gm_commandtype {
	HPWMI_FAN_SPEED_GET_QUERY = 0x11,
	HPWMI_SET_PERFORMANCE_MODE = 0x1A,
	HPWMI_FAN_SPEED_MAX_GET_QUERY = 0x26,
	HPWMI_FAN_SPEED_MAX_SET_QUERY = 0x27,
};

enum hp_wmi_command {
	HPWMI_READ	= 0x01,
	HPWMI_WRITE	= 0x02,
	HPWMI_ODM	= 0x03,
	HPWMI_GM	= 0x20008,

	HPWMI_FOURZONE = 0x20009,
};

enum hp_wmi_hardware_mask {
	HPWMI_DOCK_MASK		= 0x01,
	HPWMI_TABLET_MASK	= 0x04,
};

struct bios_return {
	u32 sigpass;
	u32 return_code;
};

enum hp_return_value {
	HPWMI_RET_WRONG_SIGNATURE	= 0x02,
	HPWMI_RET_UNKNOWN_COMMAND	= 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE	= 0x04,
	HPWMI_RET_INVALID_PARAMETERS	= 0x05,
};

enum hp_wireless2_bits {
	HPWMI_POWER_STATE	= 0x01,
	HPWMI_POWER_SOFT	= 0x02,
	HPWMI_POWER_BIOS	= 0x04,
	HPWMI_POWER_HARD	= 0x08,
	HPWMI_POWER_FW_OR_HW	= HPWMI_POWER_BIOS | HPWMI_POWER_HARD,
};

// Moved thermal profile enums here for broader use
enum hp_thermal_profile_omen {
	HP_OMEN_THERMAL_PROFILE_DEFAULT     = 0x00,
	HP_OMEN_THERMAL_PROFILE_PERFORMANCE = 0x01,
	HP_OMEN_THERMAL_PROFILE_COOL        = 0x02,
};

enum hp_thermal_profile {
	HP_THERMAL_PROFILE_PERFORMANCE	= 0x00,
	HP_THERMAL_PROFILE_DEFAULT		= 0x01,
	HP_THERMAL_PROFILE_COOL			= 0x02
};

#define IS_HWBLOCKED(x) ((x & HPWMI_POWER_FW_OR_HW) != HPWMI_POWER_FW_OR_HW)
#define IS_SWBLOCKED(x) !(x & HPWMI_POWER_SOFT)

struct bios_rfkill2_device_state {
	u8 radio_type;
	u8 bus_type;
	u16 vendor_id;
	u16 product_id;
	u16 subsys_vendor_id;
	u16 subsys_product_id;
	u8 rfkill_id;
	u8 power;
	u8 unknown[4];
};

/* 7 devices fit into the 128 byte buffer */
#define HPWMI_MAX_RFKILL2_DEVICES	7

struct bios_rfkill2_state {
	u8 unknown[7];
	u8 count;
	u8 pad[8];
	struct bios_rfkill2_device_state device[HPWMI_MAX_RFKILL2_DEVICES];
};

// Set if the keycode is a key release
#define HPWMI_HOTKEY_RELEASE_FLAG (1<<16)

static const struct key_entry hp_wmi_keymap[] = {
	{ KE_KEY, 0x02,   { KEY_BRIGHTNESSUP } },
	{ KE_KEY, 0x03,   { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x20e6, { KEY_PROG1 } },
	{ KE_KEY, 0x20e8, { KEY_MEDIA } },
	{ KE_KEY, 0x2142, { KEY_MEDIA } },
	{ KE_KEY, 0x213b, { KEY_INFO } },
	{ KE_KEY, 0x2169, { KEY_ROTATE_DISPLAY } },
	{ KE_KEY, 0x216a, { KEY_SETUP } },
	{ KE_KEY, 0x231b, { KEY_HELP } },

	{ KE_KEY, 0x21A4, { KEY_F14 } }, // Winlock hotkey
	{ KE_KEY, 0x21A5, { KEY_F15 } }, // Omen key
	{ KE_KEY, 0x21A7, { KEY_F16 } }, // Fn+Esc
	{ KE_KEY, 0x21A9, { KEY_F17 } }, // Disable touchpad hotkey

	{ KE_END, 0 }
};

static struct input_dev *hp_wmi_input_dev;
static struct platform_device *hp_wmi_platform_dev;
// static struct platform_profile_handler platform_profile_handler; // No longer needed
static struct input_dev *camera_shutter_input_dev;

static bool platform_profile_support;

static struct rfkill *wifi_rfkill;
static struct rfkill *bluetooth_rfkill;
static struct rfkill *wwan_rfkill;

struct rfkill2_device {
	u8 id;
	int num;
	struct rfkill *rfkill;
};

static int rfkill2_count;
static struct rfkill2_device rfkill2[HPWMI_MAX_RFKILL2_DEVICES];

/*
 * Chassis Types values were obtained from SMBIOS reference
 * specification version 3.00. A complete list of system enclosures
 * and chassis types is available on Table 17.
 */
static const char * const tablet_chassis_types[] = {
	"30", /* Tablet*/
	"31", /* Convertible */
	"32"  /* Detachable */
};

#define DEVICE_MODE_TABLET	0x06

// Removed: static bool zero_insize_support;

/* Determine featureset for specific models */
struct quirk_entry {
	bool fourzone;
};

static struct quirk_entry temp_omen = {
	.fourzone = true,
};

static struct quirk_entry *quirks = &temp_omen;

/* map output size to the corresponding WMI method id */
static inline int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

static int hp_wmi_perform_query(int query, enum hp_wmi_command command,
				void *buffer, int insize, int outsize)
{
	int mid;
	struct bios_return *bios_return;
	int actual_outsize;
	union acpi_object *obj;
	struct bios_args args = {
		.signature = 0x55434553,
		.command = command,
		.commandtype = query,
		.datasize = insize,
		.data = { 0 },
	};
	struct acpi_buffer input = { sizeof(struct bios_args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int ret = 0;

	mid = encode_outsize_for_pvsz(outsize);
	if (WARN_ON(mid < 0))
		return mid;

	if (WARN_ON(insize > sizeof(args.data)))
		return -EINVAL;
	if (insize > 0 && buffer) // Ensure buffer is not NULL if insize > 0
		memcpy(&args.data[0], buffer, insize);

	// Use wmi_evaluate_method for consistency, older kernels might not have wmi_query_method
	// status = wmi_query_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	// if (ACPI_FAILURE(status)) {
	//  pr_warn("hp_wmi_perform_query failed to evaluate method (0x%x)\n", status);
	//  return -EIO;
	// }
	// For simplicity and matching original code, stick to wmi_evaluate_method
	// but note that wmi_query_method is often preferred for its error handling.
	// The original code used wmi_evaluate_method directly.
	wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);


	obj = output.pointer;

	if (!obj)
		return -EINVAL;

	if (obj->type != ACPI_TYPE_BUFFER) {
		ret = -EINVAL;
		goto out_free;
	}

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;

	if (ret) {
		if (ret != HPWMI_RET_UNKNOWN_COMMAND &&
		    ret != HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_warn("query 0x%x command 0x%x returned error 0x%x\n", query, command, ret);
		goto out_free;
	}

	if (!outsize)
		goto out_free;

	actual_outsize = min_t(int, outsize, obj->buffer.length - sizeof(*bios_return));
	if (buffer) { // Ensure buffer is not NULL for output
		memcpy(buffer, obj->buffer.pointer + sizeof(*bios_return), actual_outsize);
		if (outsize > actual_outsize)
			memset(buffer + actual_outsize, 0, outsize - actual_outsize);
	}


out_free:
	kfree(obj);
	return ret;
}

static int hp_wmi_get_fan_speed(int fan)
{
	u8 fsh, fsl;
	char fan_data[4] = { fan, 0, 0, 0 };

	int ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_GET_QUERY, HPWMI_GM,
				       &fan_data, sizeof(fan_data),
				       sizeof(fan_data));

	if (ret != 0)
		return -EINVAL;

	fsh = fan_data[2];
	fsl = fan_data[3];

	return (fsh << 8) | fsl;
}

static int hp_wmi_read_int(int query)
{
	int val = 0, ret;

	ret = hp_wmi_perform_query(query, HPWMI_READ, &val,
				   sizeof(val), sizeof(val));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return val;
}

static int hp_wmi_hw_state(int mask)
{
	int state = hp_wmi_read_int(HPWMI_HARDWARE_QUERY);

	if (state < 0)
		return state;

	return !!(state & mask);
}

static int omen_thermal_profile_set(int mode)
{
	char buffer[2] = {0, mode}; // First byte often command/sub-command, second is value
	int ret;

	if (mode < HP_OMEN_THERMAL_PROFILE_DEFAULT || mode > HP_OMEN_THERMAL_PROFILE_COOL)
		return -EINVAL;

	// According to other similar drivers, HPWMI_SET_PERFORMANCE_MODE might take 2 bytes input
	// buffer[0] might be a sub-command or fixed value.
	// If buffer[0] should be something else, this needs adjustment.
	// Assuming buffer[0]=0 is fine as per the existing code.
	ret = hp_wmi_perform_query(HPWMI_SET_PERFORMANCE_MODE, HPWMI_GM,
				   &buffer, sizeof(buffer), 0); // Output size 0 for write

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return 0; // Success
}

static bool is_omen_thermal_profile(void)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board_name)
		return false;

	return match_string(omen_thermal_profile_boards,
			    ARRAY_SIZE(omen_thermal_profile_boards),
			    board_name) >= 0;
}

static int omen_thermal_profile_get(void)
{
	u8 data;
	int ret;

	// EC access can be tricky and platform-specific beyond WMI.
	// Ensure acpi_ec driver is loaded and this offset is correct for your device.
	// This is outside WMI, so if it fails, it's an EC issue.
	ret = ec_read(HP_OMEN_EC_THERMAL_PROFILE_OFFSET, &data);
	if (ret < 0) // ec_read returns negative on error, or number of bytes read
		return ret;

	return data;
}

static int hp_wmi_fan_speed_max_set(int enabled)
{
	int val = enabled; // Typically 0 for auto, 1 for max.
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, HPWMI_GM,
				   &val, sizeof(val), 0); // Output size 0 for write

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return 0; // Success
}

static int hp_wmi_fan_speed_max_get(void)
{
	int val = 0, ret;

	ret = hp_wmi_perform_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, HPWMI_GM,
				   &val, sizeof(val), sizeof(val));

	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return val;
}

static int __init hp_wmi_bios_2008_later(void)
{
	int state = 0;
	int ret = hp_wmi_perform_query(HPWMI_FEATURE_QUERY, HPWMI_READ, &state,
				       sizeof(state), sizeof(state));
	if (!ret)
		return 1;

	return (ret == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO;
}

static int __init hp_wmi_bios_2009_later(void)
{
	u8 state[128] = {0}; // Initialize buffer
	int ret = hp_wmi_perform_query(HPWMI_FEATURE2_QUERY, HPWMI_READ, &state,
				       sizeof(state), sizeof(state)); // Pass full buffer for potential input needs
	if (!ret)
		return 1;

	return (ret == HPWMI_RET_UNKNOWN_CMDTYPE) ? 0 : -ENXIO;
}

static int __init hp_wmi_enable_hotkeys(void)
{
	int value = 0x6e;
	int ret = hp_wmi_perform_query(HPWMI_BIOS_QUERY, HPWMI_WRITE, &value,
				       sizeof(value), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static int hp_wmi_set_block(void *data, bool blocked)
{
	enum hp_wmi_radio r = (enum hp_wmi_radio)(uintptr_t)data; // Cast data appropriately
	int query_val = BIT(r + 8) | ((!blocked) << r); // query_val not query
	int ret;

	ret = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE,
				   &query_val, sizeof(query_val), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static const struct rfkill_ops hp_wmi_rfkill_ops = {
	.set_block = hp_wmi_set_block,
};

static bool hp_wmi_get_sw_state(enum hp_wmi_radio r)
{
	int mask = 0x200 << (r * 8);
	int wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);

	if (wireless < 0) {
		WARN_ONCE(1, "error executing HPWMI_WIRELESS_QUERY for SW state");
		return true; // Default to blocked on error
	}
	return !(wireless & mask);
}

static int camera_shutter_input_setup(void)
{
	int err;

	camera_shutter_input_dev = input_allocate_device();
	if (!camera_shutter_input_dev)
		return -ENOMEM;

	camera_shutter_input_dev->name = "HP WMI camera shutter";
	camera_shutter_input_dev->phys = "wmi/input1";
	camera_shutter_input_dev->id.bustype = BUS_HOST;

	__set_bit(EV_SW, camera_shutter_input_dev->evbit);
	__set_bit(SW_CAMERA_LENS_COVER, camera_shutter_input_dev->swbit);

	err = input_register_device(camera_shutter_input_dev);
	if (err)
		goto err_free_dev;

	return 0;

 err_free_dev:
	input_free_device(camera_shutter_input_dev);
	camera_shutter_input_dev = NULL;
	return err;
}

static bool hp_wmi_get_hw_state(enum hp_wmi_radio r)
{
	int mask = 0x800 << (r * 8);
	int wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);

	if (wireless < 0) {
		WARN_ONCE(1, "error executing HPWMI_WIRELESS_QUERY for HW state");
		return true; // Default to blocked on error
	}
	return !(wireless & mask);
}

static int hp_wmi_rfkill2_set_block(void *data, bool blocked)
{
	int rfkill_id = (int)(uintptr_t)data; // Cast data appropriately
	char buffer[4] = { 0x01, 0x00, rfkill_id, !blocked };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_WRITE,
				   buffer, sizeof(buffer), 0);

	return ret <= 0 ? ret : -EINVAL;
}

static const struct rfkill_ops hp_wmi_rfkill2_ops = {
	.set_block = hp_wmi_rfkill2_set_block,
};

static int hp_wmi_rfkill2_refresh(void)
{
	struct bios_rfkill2_state state = {0}; // Initialize
	int err, i;

	err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &state,
				   sizeof(state), sizeof(state)); // Pass full struct for input needs if any
	if (err)
		return err < 0 ? err : -EINVAL; // Ensure negative error code

	for (i = 0; i < rfkill2_count; i++) {
		int num = rfkill2[i].num;
		struct bios_rfkill2_device_state *devstate;

		if (num >= state.count) { // Check bounds
			pr_warn("rfkill device index %d out of bounds (count %d)\n", num, state.count);
			continue;
		}
		devstate = &state.device[num];

		if (devstate->rfkill_id != rfkill2[i].id) {
			pr_warn("power configuration of the wireless devices unexpectedly changed (id mismatch)\n");
			continue;
		}
		if (rfkill2[i].rfkill) // Check if rfkill device exists
			rfkill_set_states(rfkill2[i].rfkill,
					  IS_SWBLOCKED(devstate->power),
					  IS_HWBLOCKED(devstate->power));
	}

	return 0;
}

static ssize_t display_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int value = hp_wmi_read_int(HPWMI_DISPLAY_QUERY);
	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t hddtemp_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int value = hp_wmi_read_int(HPWMI_HDDTEMP_QUERY);
	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t als_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int value = hp_wmi_read_int(HPWMI_ALS_QUERY);
	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t dock_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	int value = hp_wmi_hw_state(HPWMI_DOCK_MASK);
	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t tablet_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	int value = hp_wmi_hw_state(HPWMI_TABLET_MASK);
	if (value < 0)
		return value;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t postcode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	int value = hp_wmi_read_int(HPWMI_POSTCODEERROR_QUERY);
	if (value < 0)
		return value;
	return sysfs_emit(buf, "0x%x\n", value);
}

static ssize_t als_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	u32 tmp;
	int ret;

	ret = kstrtou32(buf, 10, &tmp);
	if (ret)
		return ret;

	ret = hp_wmi_perform_query(HPWMI_ALS_QUERY, HPWMI_WRITE, &tmp,
				       sizeof(tmp), 0); // Output size 0 for write
	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return count;
}

static ssize_t postcode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	u32 tmp = 1;
	bool clear;
	int ret;

	ret = kstrtobool(buf, &clear);
	if (ret)
		return ret;

	if (clear == false)
		return -EINVAL;

	ret = hp_wmi_perform_query(HPWMI_POSTCODEERROR_QUERY, HPWMI_WRITE, &tmp,
				       sizeof(tmp), 0); // Output size 0 for write
	if (ret)
		return ret < 0 ? ret : -EINVAL;

	return count;
}

static DEVICE_ATTR_RO(display);
static DEVICE_ATTR_RO(hddtemp);
static DEVICE_ATTR_RW(als);
static DEVICE_ATTR_RO(dock);
static DEVICE_ATTR_RO(tablet);
static DEVICE_ATTR_RW(postcode);

static struct attribute *hp_wmi_attrs[] = {
	&dev_attr_display.attr,
	&dev_attr_hddtemp.attr,
	&dev_attr_als.attr,
	&dev_attr_dock.attr,
	&dev_attr_tablet.attr,
	&dev_attr_postcode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hp_wmi);

static int hp_wmi_get_dock_state(void)
{
	int state = hp_wmi_read_int(HPWMI_HARDWARE_QUERY);

	if (state < 0)
		return state;

	return !!(state & HPWMI_DOCK_MASK);
}

static int hp_wmi_get_tablet_mode(void)
{
	char system_device_mode[4] = { 0 }; // Initialize buffer
	const char *chassis_type;
	bool tablet_found;
	int ret;

	chassis_type = dmi_get_system_info(DMI_CHASSIS_TYPE);
	if (!chassis_type)
		return -ENODEV;

	tablet_found = match_string(tablet_chassis_types,
				    ARRAY_SIZE(tablet_chassis_types),
				    chassis_type) >= 0;
	if (!tablet_found)
		return -ENODEV;

	// For HPWMI_SYSTEM_DEVICE_MODE read, insize should be 0 as per mainline kernel
	ret = hp_wmi_perform_query(HPWMI_SYSTEM_DEVICE_MODE, HPWMI_READ,
				   system_device_mode, 0, /* insize = 0 */
				   sizeof(system_device_mode));
	if (ret < 0) // hp_wmi_perform_query returns negative on error, or WMI status code
		return ret; // Propagate error

	return system_device_mode[0] == DEVICE_MODE_TABLET;
}

static void hp_wmi_notify(union acpi_object *obj, void *context)
{
	u32 event_id, event_data;
	u32 *location;
	int key_code;
	int tablet_mode; // For SW_TABLET_MODE

	if (!obj)
		return;
	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_info("Unknown WMI event type %d\n", obj->type);
		return;
	}

	location = (u32 *)obj->buffer.pointer;
	if (obj->buffer.length == 8) {
		event_id = location[0];
		event_data = location[1];
	} else if (obj->buffer.length == 16) {
		event_id = location[0];
		event_data = location[2]; // Index 2 for 16-byte struct
	} else {
		pr_info("Unknown WMI event buffer length %d\n", obj->buffer.length);
		return;
	}

	switch (event_id) {
	case HPWMI_DOCK_EVENT:
		if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit))
			input_report_switch(hp_wmi_input_dev, SW_DOCK,
					    hp_wmi_get_dock_state());

		// SW_TABLET_MODE handling for convertibles/tablets
		// Check enable_tablet_mode_sw if it should be auto or always enabled
		if (enable_tablet_mode_sw != 0) { // if -1 (auto) or 1 (yes)
			tablet_mode = hp_wmi_get_tablet_mode();
			if (tablet_mode >= 0) { // Success
				if (!test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit))
					__set_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit);
				input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, tablet_mode);
			} else if (enable_tablet_mode_sw == 1) { // Forced but failed
				pr_warn("Failed to get tablet mode for SW_TABLET_MODE reporting: %d\n", tablet_mode);
			}
		}
		input_sync(hp_wmi_input_dev);
		break;
	case HPWMI_PARK_HDD:
		break;
	case HPWMI_SMART_ADAPTER:
		// This could potentially trigger power_supply_changed() for AC adapter
		// For now, just log
		pr_debug("Smart Adapter event: 0x%x\n", event_data);
		break;
	case HPWMI_BEZEL_BUTTON:
		key_code = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		if (key_code < 0 || (key_code & HPWMI_HOTKEY_RELEASE_FLAG))
			break;

		if (!sparse_keymap_report_event(hp_wmi_input_dev,
						key_code, 1, true))
			pr_info("Unknown Bezel Button key_code: 0x%x\n", key_code);
		break;
	case HPWMI_OMEN_KEY:
		// Omen key might pass key_code in event_data or require query
		if (event_data != 0 && event_data != 0xFFFFFFFF) { // Check if event_data seems valid
			key_code = event_data;
		} else {
			key_code = hp_wmi_read_int(HPWMI_HOTKEY_QUERY);
		}

		if (key_code < 0 || (key_code & HPWMI_HOTKEY_RELEASE_FLAG))
			break;

		if (!sparse_keymap_report_event(hp_wmi_input_dev,
						key_code, 1, true))
			pr_info("Unknown Omen key_code: 0x%x (event_data: 0x%x)\n", key_code, event_data);
		break;
	case HPWMI_WIRELESS:
		if (rfkill2_count) {
			hp_wmi_rfkill2_refresh();
			break;
		}

		if (wifi_rfkill)
			rfkill_set_states(wifi_rfkill,
					  hp_wmi_get_sw_state(HPWMI_WIFI),
					  hp_wmi_get_hw_state(HPWMI_WIFI));
		if (bluetooth_rfkill)
			rfkill_set_states(bluetooth_rfkill,
					  hp_wmi_get_sw_state(HPWMI_BLUETOOTH),
					  hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		if (wwan_rfkill)
			rfkill_set_states(wwan_rfkill,
					  hp_wmi_get_sw_state(HPWMI_WWAN),
					  hp_wmi_get_hw_state(HPWMI_WWAN));
		break;
	case HPWMI_CPU_BATTERY_THROTTLE:
		pr_info("CPU throttle due to battery event detected (data: 0x%x)\n", event_data);
		break;
	case HPWMI_LOCK_SWITCH: // Kensington lock? Or screen lock?
		pr_debug("Lock switch event: 0x%x\n", event_data);
		break;
	case HPWMI_LID_SWITCH: // Should be handled by ACPI lid driver
		pr_debug("Lid switch event: 0x%x\n", event_data);
		break;
	case HPWMI_SCREEN_ROTATION: // Could be used for tablet auto-rotation
		pr_debug("Screen rotation event: 0x%x\n", event_data);
		break;
	case HPWMI_COOLSENSE_SYSTEM_MOBILE:
	case HPWMI_COOLSENSE_SYSTEM_HOT:
		pr_debug("Coolsense event: ID 0x%x, Data 0x%x\n", event_id, event_data);
		break;
	case HPWMI_PROXIMITY_SENSOR:
		pr_debug("Proximity sensor event: 0x%x\n", event_data);
		break;
	case HPWMI_BACKLIT_KB_BRIGHTNESS: // Usually handled by brightness keys
		pr_debug("Keyboard backlight brightness event: 0x%x\n", event_data);
		break;
	case HPWMI_PEAKSHIFT_PERIOD:
	case HPWMI_BATTERY_CHARGE_PERIOD:
		pr_debug("Battery charging event: ID 0x%x, Data 0x%x\n", event_id, event_data);
		// This could potentially trigger power_supply_changed()
		// power_supply_changed_late(hp_wmi_power_supply); // If we had one
		break;
	case HPWMI_SANITIZATION_MODE:
		pr_info("Sanitization mode event: 0x%x\n", event_data);
		break;
	case HPWMI_CAMERA_TOGGLE:
		if (!camera_shutter_input_dev) {
			if (camera_shutter_input_setup()) {
				pr_err("Failed to setup camera shutter input device\n");
				break;
			}
		}
		// Event data: 0xFF for shutter closed (lens covered), 0xFE for open
		if (event_data == 0xff)
			input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 1);
		else if (event_data == 0xfe)
			input_report_switch(camera_shutter_input_dev, SW_CAMERA_LENS_COVER, 0);
		else
			pr_warn("Unknown camera shutter state - event_id 0x%x, data 0x%x\n", event_id, event_data);
		input_sync(camera_shutter_input_dev);
		break;
	case HPWMI_SMART_EXPERIENCE_APP:
		pr_info("Smart Experience App event: 0x%x\n", event_data);
		break;
	default:
		pr_info("Unknown WMI event_id: 0x%x, event_data: 0x%x\n", event_id, event_data);
		break;
	}
}


static int __init hp_wmi_input_setup(void)
{
	acpi_status status;
	int err, val;
	int tablet_mode;

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

	val = hp_wmi_get_dock_state(); // Use new helper
	if (val >= 0) {
		__set_bit(SW_DOCK, hp_wmi_input_dev->swbit);
		input_report_switch(hp_wmi_input_dev, SW_DOCK, val);
	} else {
		pr_warn("Failed to get initial dock state: %d\n", val);
	}


	if (enable_tablet_mode_sw != 0) { // If auto (-1) or yes (1)
		tablet_mode = hp_wmi_get_tablet_mode();
		if (tablet_mode >= 0) {
			__set_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit);
			input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, tablet_mode);
		} else if (enable_tablet_mode_sw == 1) { // Forced but failed
			pr_warn("Failed to get initial tablet mode for SW_TABLET_MODE: %d\n", tablet_mode);
		}
		// If auto and failed, it will try again on event
	}


	input_sync(hp_wmi_input_dev);

	if (!hp_wmi_bios_2009_later() && hp_wmi_bios_2008_later()) {
		err = hp_wmi_enable_hotkeys();
		if (err)
			pr_warn("Failed to enable hotkeys: %d\n", err);
	}

	status = wmi_install_notify_handler(HPWMI_EVENT_GUID, hp_wmi_notify, NULL);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to install WMI notify handler: 0x%x\n", status);
		err = -EIO;
		goto err_free_keymap; // Keymap is setup, need to free it too
	}

	err = input_register_device(hp_wmi_input_dev);
	if (err)
		goto err_uninstall_notifier;

	return 0;

 err_uninstall_notifier:
	wmi_remove_notify_handler(HPWMI_EVENT_GUID);
 err_free_keymap:
	sparse_keymap_free(hp_wmi_input_dev); // Free sparse keymap if setup
 err_free_dev:
	input_free_device(hp_wmi_input_dev);
	hp_wmi_input_dev = NULL;
	return err;
}

static void hp_wmi_input_destroy(void)
{
	if (hp_wmi_input_dev) {
		wmi_remove_notify_handler(HPWMI_EVENT_GUID);
		input_unregister_device(hp_wmi_input_dev); // This also calls input_free_device
		// sparse_keymap_free(hp_wmi_input_dev); // No, unregister handles it
		hp_wmi_input_dev = NULL;
	}
}

static int __init hp_wmi_rfkill_setup(struct platform_device *device)
{
	int err = 0, wireless; // Initialize err

	wireless = hp_wmi_read_int(HPWMI_WIRELESS_QUERY);
	if (wireless < 0) {
		pr_warn("Failed to read wireless query for rfkill setup: %d\n", wireless);
		return wireless;
	}

	// Some BIOS might not support writing this if it's read-only info
	// err = hp_wmi_perform_query(HPWMI_WIRELESS_QUERY, HPWMI_WRITE, &wireless,
	//			   sizeof(wireless), 0);
	// if (err) {
	//	pr_warn("Failed to write wireless query (0x%x), proceeding with read-only info\n", err);
	//	// Don't return error, try to setup rfkill with read state
	// }


	if (wireless & 0x1) { // WiFi
		wifi_rfkill = rfkill_alloc("hp-wifi", &device->dev,
					   RFKILL_TYPE_WLAN,
					   &hp_wmi_rfkill_ops,
					   (void *) (uintptr_t)HPWMI_WIFI);
		if (!wifi_rfkill) {
			err = -ENOMEM; goto error_cleanup;
		}
		rfkill_set_states(wifi_rfkill, hp_wmi_get_sw_state(HPWMI_WIFI),
				  hp_wmi_get_hw_state(HPWMI_WIFI));
		err = rfkill_register(wifi_rfkill);
		if (err)
			goto error_cleanup;
	}

	if (wireless & 0x2) { // Bluetooth
		bluetooth_rfkill = rfkill_alloc("hp-bluetooth", &device->dev,
						RFKILL_TYPE_BLUETOOTH,
						&hp_wmi_rfkill_ops,
						(void *) (uintptr_t)HPWMI_BLUETOOTH);
		if (!bluetooth_rfkill) {
			err = -ENOMEM; goto error_cleanup;
		}
		rfkill_set_states(bluetooth_rfkill, hp_wmi_get_sw_state(HPWMI_BLUETOOTH),
				  hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		err = rfkill_register(bluetooth_rfkill);
		if (err)
			goto error_cleanup;
	}

	if (wireless & 0x4) { // WWAN
		wwan_rfkill = rfkill_alloc("hp-wwan", &device->dev,
					   RFKILL_TYPE_WWAN,
					   &hp_wmi_rfkill_ops,
					   (void *) (uintptr_t)HPWMI_WWAN);
		if (!wwan_rfkill) {
			err = -ENOMEM; goto error_cleanup;
		}
		rfkill_set_states(wwan_rfkill, hp_wmi_get_sw_state(HPWMI_WWAN),
				  hp_wmi_get_hw_state(HPWMI_WWAN));
		err = rfkill_register(wwan_rfkill);
		if (err)
			goto error_cleanup;
	}

	return 0;

error_cleanup:
	if (wwan_rfkill) {
		rfkill_unregister(wwan_rfkill);
		rfkill_destroy(wwan_rfkill);
		wwan_rfkill = NULL;
	}
	if (bluetooth_rfkill) {
		rfkill_unregister(bluetooth_rfkill);
		rfkill_destroy(bluetooth_rfkill);
		bluetooth_rfkill = NULL;
	}
	if (wifi_rfkill) {
		rfkill_unregister(wifi_rfkill);
		rfkill_destroy(wifi_rfkill);
		wifi_rfkill = NULL;
	}
	return err;
}

static int __init hp_wmi_rfkill2_setup(struct platform_device *device)
{
	struct bios_rfkill2_state state = {0}; // Initialize
	int err = 0, i; // Initialize err

	err = hp_wmi_perform_query(HPWMI_WIRELESS2_QUERY, HPWMI_READ, &state,
				   sizeof(state), sizeof(state)); // Pass full struct for input if needed
	if (err)
		return err < 0 ? err : -EINVAL;

	if (state.count > HPWMI_MAX_RFKILL2_DEVICES) {
		pr_warn("rfkill2 count %d exceeds max %d\n", state.count, HPWMI_MAX_RFKILL2_DEVICES);
		return -EINVAL;
	}

	for (i = 0; i < state.count; i++) {
		struct rfkill *rfkill_dev; // Local var for clarity
		enum rfkill_type type;
		const char *name; // Use const char *

		switch (state.device[i].radio_type) {
		case HPWMI_WIFI: type = RFKILL_TYPE_WLAN; name = "hp-wifi2"; break;
		case HPWMI_BLUETOOTH: type = RFKILL_TYPE_BLUETOOTH; name = "hp-bluetooth2"; break;
		case HPWMI_WWAN: type = RFKILL_TYPE_WWAN; name = "hp-wwan2"; break;
		case HPWMI_GPS: type = RFKILL_TYPE_GPS; name = "hp-gps2"; break;
		default:
			pr_warn("unknown rfkill2 device type 0x%x\n", state.device[i].radio_type);
			continue;
		}

		if (!state.device[i].vendor_id && !state.device[i].product_id) { // Check both
			pr_warn("rfkill2 device %d has zero vendor/product ID (type 0x%x)\n", i, state.device[i].radio_type);
			continue;
		}

		rfkill_dev = rfkill_alloc(name, &device->dev, type,
				      &hp_wmi_rfkill2_ops, (void *)(uintptr_t)i); // Pass index i
		if (!rfkill_dev) {
			err = -ENOMEM;
			goto fail;
		}

		rfkill2[rfkill2_count].id = state.device[i].rfkill_id;
		rfkill2[rfkill2_count].num = i; // Store original index from state.device
		rfkill2[rfkill2_count].rfkill = rfkill_dev;

		rfkill_set_states(rfkill_dev, IS_SWBLOCKED(state.device[i].power),
				  IS_HWBLOCKED(state.device[i].power));

		if (!(state.device[i].power & HPWMI_POWER_BIOS))
			pr_info("rfkill2 device %s (type 0x%x) blocked by BIOS\n", name, state.device[i].radio_type);

		err = rfkill_register(rfkill_dev);
		if (err) {
			rfkill_destroy(rfkill_dev); // rfkill2[rfkill2_count].rfkill is not yet assigned if register fails early
			rfkill2[rfkill2_count].rfkill = NULL;
			goto fail;
		}
		rfkill2_count++;
	}
	return 0;
fail:
	while (--rfkill2_count >= 0) { // Iterate down to 0
		if (rfkill2[rfkill2_count].rfkill) {
			rfkill_unregister(rfkill2[rfkill2_count].rfkill);
			rfkill_destroy(rfkill2[rfkill2_count].rfkill);
			rfkill2[rfkill2_count].rfkill = NULL;
		}
	}
	rfkill2_count = 0; // Ensure count is 0 on failure
	return err;
}


// --- Platform Profile Functions (New API) ---
static int platform_profile_omen_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	int tp;

	tp = omen_thermal_profile_get();
	if (tp < 0)
		return tp;

	switch (tp) {
	case HP_OMEN_THERMAL_PROFILE_PERFORMANCE: *profile = PLATFORM_PROFILE_PERFORMANCE; break;
	case HP_OMEN_THERMAL_PROFILE_DEFAULT:     *profile = PLATFORM_PROFILE_BALANCED; break;
	case HP_OMEN_THERMAL_PROFILE_COOL:        *profile = PLATFORM_PROFILE_COOL; break;
	default:
		pr_warn("Unknown Omen thermal profile value from EC: %d\n", tp);
		return -EINVAL;
	}
	return 0;
}

static int platform_profile_omen_set(struct device *dev,
				     enum platform_profile_option profile)
{
	int tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE: tp = HP_OMEN_THERMAL_PROFILE_PERFORMANCE; break;
	case PLATFORM_PROFILE_BALANCED:    tp = HP_OMEN_THERMAL_PROFILE_DEFAULT; break;
	case PLATFORM_PROFILE_COOL:        tp = HP_OMEN_THERMAL_PROFILE_COOL; break;
	default: return -EINVAL; // Use EINVAL for unsupported options
	}

	return omen_thermal_profile_set(tp); // Returns 0 on success
}

static int generic_thermal_profile_get_wmi(void) // Renamed from thermal_profile_get to avoid conflict
{
	return hp_wmi_read_int(HPWMI_THERMAL_PROFILE_QUERY);
}

static int generic_thermal_profile_set_wmi(int thermal_profile) // Renamed
{
	// Input check
	if (thermal_profile < HP_THERMAL_PROFILE_PERFORMANCE || thermal_profile > HP_THERMAL_PROFILE_COOL)
		return -EINVAL;

	return hp_wmi_perform_query(HPWMI_THERMAL_PROFILE_QUERY, HPWMI_WRITE, &thermal_profile,
				    sizeof(thermal_profile), 0); // Output size 0 for write
}

static int hp_wmi_platform_profile_get(struct device *dev,
					enum platform_profile_option *profile)
{
	int tp;

	tp = generic_thermal_profile_get_wmi();
	if (tp < 0)
		return tp;

	switch (tp) {
	case HP_THERMAL_PROFILE_PERFORMANCE: *profile =  PLATFORM_PROFILE_PERFORMANCE; break;
	case HP_THERMAL_PROFILE_DEFAULT:     *profile =  PLATFORM_PROFILE_BALANCED; break;
	case HP_THERMAL_PROFILE_COOL:        *profile =  PLATFORM_PROFILE_COOL; break;
	default:
		pr_warn("Unknown generic thermal profile value from WMI: %d\n", tp);
		return -EINVAL;
	}
	return 0;
}

static int hp_wmi_platform_profile_set(struct device *dev,
					enum platform_profile_option profile)
{
	int tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE: tp =  HP_THERMAL_PROFILE_PERFORMANCE; break;
	case PLATFORM_PROFILE_BALANCED:    tp =  HP_THERMAL_PROFILE_DEFAULT; break;
	case PLATFORM_PROFILE_COOL:        tp =  HP_THERMAL_PROFILE_COOL; break;
	default: return -EINVAL; // Use EINVAL for unsupported options
	}

	return generic_thermal_profile_set_wmi(tp); // Returns WMI status or negative error
}


/* Support for the HP Omen FourZone keyboard lighting */
#define FOURZONE_COUNT 4

struct color_platform {
	u8 blue;
	u8 green;
	u8 red;
} __packed;

struct platform_zone {
	u8 offset;
	struct device_attribute *attr; // Points to an entry in zone_dev_attrs
	struct color_platform colors;
	char *name_ptr; // Store kstrdup'd name for freeing
};

static struct device_attribute *zone_dev_attrs; // Array of device_attributes
static struct attribute **zone_attrs; // Array of pointers to attributes for sysfs group
static struct platform_zone *zone_data; // Array of platform_zone structs

static struct attribute_group zone_attribute_group = {
	.name = "rgb_zones",
	/* .attrs is set dynamically */
};

static int parse_rgb(const char *buf, struct color_platform *colors) // Pass colors directly
{
	unsigned long rgb_val; // Use a different name
	int ret;

	union color_union {
		struct color_platform cp;
		u32 package; // Use u32 for up to 0xFFFFFF
	} repackager = { {0} };


	ret = kstroul(buf, 16, &rgb_val); // Use kstroul
	if (ret)
		return ret;

	if (rgb_val > 0xFFFFFF)
		return -EINVAL;

	repackager.package = cpu_to_be32(rgb_val); // Assuming R is MSB in 0xRRGGBB
	// Example: 0x123456 -> BE: 12 34 56 00 -> if struct is B G R, then B=12, G=34, R=56
	// If struct is R G B, then R=12, G=34, B=56.
	// The struct is B G R. So buf "RRGGBB" -> 0xRR, 0xGG, 0xBB
	// colors->red = (rgb_val >> 16) & 0xFF;
	// colors->green = (rgb_val >> 8) & 0xFF;
	// colors->blue = rgb_val & 0xFF;
	// Simpler:
	sscanf(buf, "%2hhx%2hhx%2hhx", &colors->red, &colors->green, &colors->blue);
    // Check sscanf return? For simplicity, assume valid hex string if kstroul passed.

	pr_debug("hp-wmi: parsed r:%d g:%d b:%d\n", colors->red, colors->green, colors->blue);
	return 0;
}


static struct platform_zone *match_zone_by_attr(struct device_attribute *attr)
{
	int i;
	if (!zone_data || !zone_dev_attrs) return NULL;
	for (i = 0; i < FOURZONE_COUNT; i++) {
		if (&zone_dev_attrs[i] == attr) { // Compare addresses of device_attribute structs
			return &zone_data[i];
		}
	}
	return NULL;
}

static int fourzone_update_led(struct platform_zone *zone, bool read_op) // Changed bool
{
	u8 state[128] = {0}; // Initialize
	int ret;

	// Always read current state first, even for write, as we modify parts of it
	ret = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_GET, HPWMI_FOURZONE, state,
			0, sizeof(state)); // insize 0 for read, or sizeof(state) if it expects it

	if (ret) {
		pr_warn("fourzone_color_get returned error 0x%x\n", ret);
		return ret < 0 ? ret : -EIO; // Return negative error
	}

	if (read_op) {
		zone->colors.red   = state[zone->offset + 0];
		zone->colors.green = state[zone->offset + 1];
		zone->colors.blue  = state[zone->offset + 2];
	} else { // Write operation
		state[zone->offset + 0] = zone->colors.red;
		state[zone->offset + 1] = zone->colors.green;
		state[zone->offset + 2] = zone->colors.blue;

		ret = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_SET, HPWMI_FOURZONE, state,
				sizeof(state), 0); // insize is full buffer, outsize 0 for write

		if (ret) {
			pr_warn("fourzone_color_set returned error 0x%x\n", ret);
			return ret < 0 ? ret : -EIO; // Return negative error
		}
	}
	return 0;
}

static ssize_t zone_show(struct device *dev, struct device_attribute *attr,
       char *buf)
{
	struct platform_zone *target_zone;
	int ret;

	target_zone = match_zone_by_attr(attr);
	if (!target_zone) {
		pr_err("hp-wmi: zone_show: could not match attribute\n");
		return -EINVAL;
	}

	ret = fourzone_update_led(target_zone, true); // Read operation
	if (ret)
		return sysfs_emit(buf, "Error reading zone: %d\n", ret);

	return sysfs_emit(buf, "RGB: %02x%02x%02x (R:%d G:%d B:%d)\n",
			target_zone->colors.red, target_zone->colors.green, target_zone->colors.blue,
			target_zone->colors.red, target_zone->colors.green, target_zone->colors.blue);
}

static ssize_t zone_set(struct device *dev, struct device_attribute *attr,
      const char *buf, size_t count)
{
	struct platform_zone *target_zone;
	struct color_platform new_colors; // Temporary to parse into
	int ret;

	target_zone = match_zone_by_attr(attr);
	if (!target_zone) {
		pr_err("hp-wmi: zone_set: could not match attribute\n");
		return -EINVAL;
	}

	ret = parse_rgb(buf, &new_colors); // Parse into temporary
	if (ret)
		return ret;

	target_zone->colors = new_colors; // Assign if parse successful

	ret = fourzone_update_led(target_zone, false); // Write operation
	return ret ? ret : count;
}

static int fourzone_setup(struct platform_device *pdev) // Changed arg name
{
	int zone_idx; // Use int for loop iterators
	char name_buf[16]; // Buffer for sysfs name
	int err = 0;

	if (!quirks || !quirks->fourzone)
		return 0;

	zone_dev_attrs = kcalloc(FOURZONE_COUNT, sizeof(struct device_attribute), GFP_KERNEL);
	if (!zone_dev_attrs) return -ENOMEM;

	zone_attrs = kcalloc(FOURZONE_COUNT + 1, sizeof(struct attribute *), GFP_KERNEL);
	if (!zone_attrs) { kfree(zone_dev_attrs); zone_dev_attrs = NULL; return -ENOMEM; }

	zone_data = kcalloc(FOURZONE_COUNT, sizeof(struct platform_zone), GFP_KERNEL);
	if (!zone_data) {
		kfree(zone_attrs); zone_attrs = NULL;
		kfree(zone_dev_attrs); zone_dev_attrs = NULL;
		return -ENOMEM;
	}

	for (zone_idx = 0; zone_idx < FOURZONE_COUNT; zone_idx++) {
		snprintf(name_buf, sizeof(name_buf), "zone%02X_rgb", zone_idx); // Changed name format
		zone_data[zone_idx].name_ptr = kstrdup(name_buf, GFP_KERNEL);
		if (!zone_data[zone_idx].name_ptr) {
			err = -ENOMEM; goto error_cleanup_fourzone;
		}

		sysfs_attr_init(&zone_dev_attrs[zone_idx].attr);
		zone_dev_attrs[zone_idx].attr.name = zone_data[zone_idx].name_ptr;
		zone_dev_attrs[zone_idx].attr.mode = 0644; // S_IWUGO | S_IRUGO would be more standard
		zone_dev_attrs[zone_idx].show = zone_show;
		zone_dev_attrs[zone_idx].store = zone_set;

		zone_data[zone_idx].offset = 25 + (zone_idx * 3);
		zone_data[zone_idx].attr = &zone_dev_attrs[zone_idx]; // Link back
		zone_attrs[zone_idx] = &zone_dev_attrs[zone_idx].attr;
	}
	zone_attrs[FOURZONE_COUNT] = NULL; // Null terminate the list

	zone_attribute_group.attrs = zone_attrs;
	err = sysfs_create_group(&pdev->dev.kobj, &zone_attribute_group);
	if (err)
		goto error_cleanup_fourzone;

	return 0;

error_cleanup_fourzone:
	for (zone_idx--; zone_idx >= 0; zone_idx--) { // Free allocated names
		kfree(zone_data[zone_idx].name_ptr);
	}
	kfree(zone_data); zone_data = NULL;
	kfree(zone_attrs); zone_attrs = NULL;
	kfree(zone_dev_attrs); zone_dev_attrs = NULL;
	return err;
}

static void fourzone_remove(struct platform_device *pdev)
{
	int i;
	if (quirks && quirks->fourzone && zone_attribute_group.attrs) {
		sysfs_remove_group(&pdev->dev.kobj, &zone_attribute_group);
		if (zone_data) {
			for (i = 0; i < FOURZONE_COUNT; i++) {
				kfree(zone_data[i].name_ptr); // Free kstrdup'd names
			}
		}
		kfree(zone_data); zone_data = NULL;
		kfree(zone_attrs); zone_attrs = NULL; // zone_attrs only holds pointers
		kfree(zone_dev_attrs); zone_dev_attrs = NULL;
		zone_attribute_group.attrs = NULL; // Clear pointer
	}
}


static int thermal_profile_setup(void)
{
	struct device *dev = &hp_wmi_platform_dev->dev;
	int err, tp_val; // Renamed tp to tp_val
	unsigned long choices_mask = 0; // Use unsigned long for set_bit

	// Pointers to the chosen get/set functions
	int (*profile_get_func)(struct device *, enum platform_profile_option *);
	int (*profile_set_func)(struct device *, enum platform_profile_option);

	if (is_omen_thermal_profile()) {
		tp_val = omen_thermal_profile_get();
		if (tp_val < 0) {
			pr_warn("Failed to get Omen thermal profile: %d\n", tp_val);
			return tp_val;
		}
		// Ensure firmware variables are correctly set by writing current profile back
		err = omen_thermal_profile_set(tp_val);
		if (err < 0) { // omen_thermal_profile_set returns 0 on success
			pr_warn("Failed to set initial Omen thermal profile: %d\n", err);
			return err;
		}
		profile_get_func = platform_profile_omen_get;
		profile_set_func = platform_profile_omen_set;
	} else {
		tp_val = generic_thermal_profile_get_wmi();
		if (tp_val < 0) {
			pr_warn("Failed to get generic thermal profile: %d\n", tp_val);
			return tp_val;
		}
		// Ensure firmware variables are correctly set
		err = generic_thermal_profile_set_wmi(tp_val);
		if (err) { // generic_thermal_profile_set_wmi returns WMI status or negative error
			pr_warn("Failed to set initial generic thermal profile: %d\n", err);
			return err < 0 ? err : -EIO;
		}
		profile_get_func = hp_wmi_platform_profile_get;
		profile_set_func = hp_wmi_platform_profile_set;
	}

	set_bit(PLATFORM_PROFILE_COOL, &choices_mask);
	set_bit(PLATFORM_PROFILE_BALANCED, &choices_mask);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, &choices_mask);

	err = platform_profile_register(dev, "hp-wmi", &choices_mask,
					profile_get_func, profile_set_func);
	if (err) {
		pr_err("Failed to register platform profile: %d\n", err);
		return err;
	}

	platform_profile_support = true;
	return 0;
}

static int hp_wmi_hwmon_init(void); // Forward declaration

static int __init hp_wmi_bios_setup(struct platform_device *device)
{
	int err;

	wifi_rfkill = NULL;
	bluetooth_rfkill = NULL;
	wwan_rfkill = NULL;
	rfkill2_count = 0;

	// Try rfkill_setup first, if it fails or returns specific code, try rfkill2_setup
	err = hp_wmi_rfkill_setup(device);
	if (err && err != -ENODEV) { // -ENODEV might mean no legacy rfkill interface
		pr_warn("hp_wmi_rfkill_setup failed (%d), attempting rfkill2_setup\n", err);
		// Fall through to try rfkill2, or handle error more specifically
	}
	// Always try rfkill2_setup if rfkill_setup didn't find much or failed softly
	// Or, more simply, if rfkill_setup found nothing or failed, try rfkill2.
	// A common pattern: if (hp_wmi_rfkill_setup(device) != 0) { ... }
	// For now, let's assume one might partially succeed or we want to try both if one is weak.
	// The original logic was: if (hp_wmi_rfkill_setup(device)) hp_wmi_rfkill2_setup(device);
	// This means rfkill2 is only tried if rfkill_setup *succeeds* but returns non-zero (WMI error codes).
	// Let's refine: try legacy, if it reports "no devices" or fails, try newer.
	if (hp_wmi_rfkill_setup(device) != 0) { // If legacy setup returns non-zero (error or WMI specific code)
		pr_info("Legacy rfkill setup indicated issues or no devices, trying rfkill2.\n");
		err = hp_wmi_rfkill2_setup(device);
		if (err)
			pr_warn("hp_wmi_rfkill2_setup also failed: %d\n", err);
			// Decide if this is a fatal error for the module part.
	}


	err = fourzone_setup(device);
	if (err) {
		pr_err("Failed to setup FourZone keyboard lighting: %d\n", err);
		// Non-fatal, continue
	}

	err = hp_wmi_hwmon_init();
	if (err) {
		pr_err("Failed to initialize HWMON interface: %d\n", err);
		// Potentially non-fatal, depending on requirements
		// return err; // If HWMON is critical
	}

	err = thermal_profile_setup();
	if (err) {
		pr_err("Failed to setup thermal profile: %d\n", err);
		// Potentially non-fatal
		// return err; // If thermal profiles are critical
	}

	return 0; // Success for this part of setup
}

static void __exit hp_wmi_bios_remove(struct platform_device *device)
{
	int i;

	// Cleanup fourzone first as it uses device->dev.kobj
	fourzone_remove(device);

	if (platform_profile_support) {
		platform_profile_remove(&device->dev);
		platform_profile_support = false;
	}

	// Unregister rfkill devices
	for (i = 0; i < rfkill2_count; i++) {
		if (rfkill2[i].rfkill) {
			rfkill_unregister(rfkill2[i].rfkill);
			rfkill_destroy(rfkill2[i].rfkill);
			rfkill2[i].rfkill = NULL;
		}
	}
	rfkill2_count = 0;

	if (wifi_rfkill) {
		rfkill_unregister(wifi_rfkill);
		rfkill_destroy(wifi_rfkill);
		wifi_rfkill = NULL;
	}
	if (bluetooth_rfkill) {
		rfkill_unregister(bluetooth_rfkill);
		rfkill_destroy(bluetooth_rfkill);
		bluetooth_rfkill = NULL;
	}
	if (wwan_rfkill) {
		rfkill_unregister(wwan_rfkill);
		rfkill_destroy(wwan_rfkill);
		wwan_rfkill = NULL;
	}
	// hwmon registered with devm_, so auto-cleanup
}

static int hp_wmi_resume_handler(struct device *device)
{
	int dock_state, tablet_state;

	if (hp_wmi_input_dev) {
		if (test_bit(SW_DOCK, hp_wmi_input_dev->swbit)) {
			dock_state = hp_wmi_get_dock_state();
			if (dock_state >= 0)
				input_report_switch(hp_wmi_input_dev, SW_DOCK, dock_state);
		}
		if (test_bit(SW_TABLET_MODE, hp_wmi_input_dev->swbit) && enable_tablet_mode_sw != 0) {
			tablet_state = hp_wmi_get_tablet_mode();
			if (tablet_state >= 0)
				input_report_switch(hp_wmi_input_dev, SW_TABLET_MODE, tablet_state);
		}
		input_sync(hp_wmi_input_dev);
	}

	if (rfkill2_count)
		hp_wmi_rfkill2_refresh();
	else { // Only refresh legacy if rfkill2 is not active
		if (wifi_rfkill)
			rfkill_set_states(wifi_rfkill, hp_wmi_get_sw_state(HPWMI_WIFI), hp_wmi_get_hw_state(HPWMI_WIFI));
		if (bluetooth_rfkill)
			rfkill_set_states(bluetooth_rfkill, hp_wmi_get_sw_state(HPWMI_BLUETOOTH), hp_wmi_get_hw_state(HPWMI_BLUETOOTH));
		if (wwan_rfkill)
			rfkill_set_states(wwan_rfkill, hp_wmi_get_sw_state(HPWMI_WWAN), hp_wmi_get_hw_state(HPWMI_WWAN));
	}
	
	// Refresh thermal profile state? Some systems might change it during sleep.
	// For now, assume it's persistent or re-applied by userspace if needed.

	return 0;
}

static const struct dev_pm_ops hp_wmi_pm_ops = {
	.resume  = hp_wmi_resume_handler,
	.restore  = hp_wmi_resume_handler, // restore is often same as resume
};

static struct platform_driver hp_wmi_driver = {
	.driver = {
		.name = "hp-wmi",
		.pm = &hp_wmi_pm_ops,
		.dev_groups = hp_wmi_groups,
	},
	.probe = hp_wmi_bios_setup, // Ensure probe is assigned if not done via platform_driver_probe
	.remove = __exit_p(hp_wmi_bios_remove), // Use __exit_p for .remove
};

static umode_t hp_wmi_hwmon_is_visible(const void *drvdata, // drvdata usually *this* driver struct
				       enum hwmon_sensor_types type,
				       u32 attr, int channel)
{
	// drvdata is unused here, but good practice to have it if needed later.
	switch (type) {
	case hwmon_pwm: // Fan control mode (auto/max)
		if (attr == hwmon_pwm_enable) return 0644; // Read-write
		break;
	case hwmon_fan: // Fan speed reading
		if (attr == hwmon_fan_input) {
			if (hp_wmi_get_fan_speed(channel) >= 0) // Channel might map to fan index
				return 0444; // Read-only
		}
		break;
	default:
		break;
	}
	return 0;
}

static int hp_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	int ret;

	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_input) {
			ret = hp_wmi_get_fan_speed(channel); // Channel is fan index (0, 1, ..)
			if (ret < 0) return ret;
			*val = ret;
			return 0;
		}
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_enable) { // pwmX_enable: 0=off, 1=manual, 2=auto
			ret = hp_wmi_fan_speed_max_get();
			if (ret < 0) return ret;
			if (ret == 0) *val = 2; // Our 'auto' (0) is hwmon 'auto' (2)
			else if (ret == 1) *val = 0; // Our 'max' (1) is hwmon 'no control / full speed' (0)
			else return -ENODATA; // Unknown state
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static int hp_wmi_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long val)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_enable) {
		if (val == 2) return hp_wmi_fan_speed_max_set(0); // hwmon 'auto' (2) to our 'auto' (0)
		if (val == 0) return hp_wmi_fan_speed_max_set(1); // hwmon 'no control/max' (0) to our 'max' (1)
		// hwmon 'manual' (1) is not supported by this WMI interface directly.
		return -EINVAL; // Unsupported value for pwmX_enable
	}
	return -EOPNOTSUPP;
}

// Define HWMON structures
static const struct hwmon_channel_info * const hp_wmi_hwmon_info[] = { // const correctness
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT), // Fan 1 input (speed)
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_ENABLE), // PWM 1 enable (control mode)
	// Add more channels if multiple fans/PWMs are supported by WMI calls
	NULL
};

static const struct hwmon_ops hp_wmi_hwmon_ops = { // const correctness
	.is_visible = hp_wmi_hwmon_is_visible,
	.read = hp_wmi_hwmon_read,
	.write = hp_wmi_hwmon_write,
};

static const struct hwmon_chip_info hp_wmi_hwmon_chip_info = { // const correctness
	.ops = &hp_wmi_hwmon_ops,
	.info = hp_wmi_hwmon_info,
};

static int hp_wmi_hwmon_init(void)
{
	struct device *hwmon_dev; // Use different name than dev passed to callbacks
	// Pass hp_wmi_platform_dev as parent for devm_
	if (!hp_wmi_platform_dev) {
		pr_err("HWMON init: hp_wmi_platform_dev is NULL\n");
		return -ENODEV;
	}

	// Drvdata for hwmon can be NULL if not needed, or point to specific context.
	// Here, it's not explicitly used by callbacks, so NULL is fine.
	hwmon_dev = devm_hwmon_device_register_with_info(&hp_wmi_platform_dev->dev,
							 "hp_wmi", // Name for hwmon device
							 NULL, // drvdata (private data for hwmon ops)
							 &hp_wmi_hwmon_chip_info,
							 NULL); // groups (extra sysfs attributes)

	if (IS_ERR(hwmon_dev)) {
		pr_err("Could not register hp_wmi hwmon device: %ld\n", PTR_ERR(hwmon_dev));
		return PTR_ERR(hwmon_dev);
	}
	// pr_info("HP WMI HWMON interface registered.\n");
	return 0;
}

static int __init hp_wmi_init(void)
{
	int err;
	bool event_capable = wmi_has_guid(HPWMI_EVENT_GUID);
	bool bios_capable = wmi_has_guid(HPWMI_BIOS_GUID);

	if (!event_capable && !bios_capable) {
		pr_info("No HP WMI interface detected\n");
		return -ENODEV;
	}

	if (event_capable) {
		err = hp_wmi_input_setup();
		if (err) {
			pr_err("HP WMI input setup failed: %d\n", err);
			// Don't necessarily fail all if one part fails, depends on desired behavior
			// For now, if input fails, we might still want BIOS features.
			// However, if input is critical, return err. Let's assume it's critical.
			return err;
		}
	}

	if (bios_capable) {
		hp_wmi_platform_dev = platform_device_register_simple("hp-wmi", -1, NULL, 0);
		if (IS_ERR(hp_wmi_platform_dev)) {
			err = PTR_ERR(hp_wmi_platform_dev);
			pr_err("Failed to register hp-wmi platform device: %d\n", err);
			goto err_destroy_input;
		}
		// platform_driver_probe is for manual probing. Registering driver and device
		// separately and letting bus matching handle it is more common.
		// Here, we register the driver, and it should probe the device.
		err = platform_driver_register(&hp_wmi_driver);
		if (err) {
			pr_err("Failed to register hp-wmi platform driver: %d\n", err);
			goto err_unregister_pdev;
		}
		// The .probe = hp_wmi_bios_setup in hp_wmi_driver will be called now.
	}
	pr_info("HP WMI driver initialized (event: %d, bios: %d)\n", event_capable, bios_capable);
	return 0;

err_unregister_pdev:
	platform_device_unregister(hp_wmi_platform_dev);
	hp_wmi_platform_dev = NULL;
err_destroy_input:
	if (event_capable)
		hp_wmi_input_destroy();
	if (camera_shutter_input_dev) { // Clean up camera shutter if it was created by event handler
		input_unregister_device(camera_shutter_input_dev);
		camera_shutter_input_dev = NULL;
	}
	return err;
}
module_init(hp_wmi_init);

static void __exit hp_wmi_exit(void)
{
	if (wmi_has_guid(HPWMI_BIOS_GUID) && hp_wmi_platform_dev) { // Check both
		platform_driver_unregister(&hp_wmi_driver); // Calls .remove -> hp_wmi_bios_remove
		platform_device_unregister(hp_wmi_platform_dev);
		hp_wmi_platform_dev = NULL;
	}

	if (wmi_has_guid(HPWMI_EVENT_GUID)) { // Check if event handler was ever relevant
		hp_wmi_input_destroy(); // Cleans hp_wmi_input_dev
		if (camera_shutter_input_dev) {
			input_unregister_device(camera_shutter_input_dev);
			camera_shutter_input_dev = NULL;
		}
	}
	pr_info("HP WMI driver unloaded\n");
}
module_exit(hp_wmi_exit);