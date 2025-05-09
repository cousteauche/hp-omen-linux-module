HP Omen/Platform Special Feature Control for Linux
-------------------------------------------------

This is an enhanced version of the standard `hp-wmi` kernel module, integrating features commonly found in HP's Omen Command Centre and other HP utilities, primarily focused on providing RGB keyboard control and maintaining compatibility with newer kernels.

It aims to be a more comprehensive solution by merging community-developed RGB support with the ongoing development of the upstream `hp-wmi` driver.

**DISCLAIMER:** While this module is based on existing kernel code and community efforts, any kernel module carries a small risk. This version has been tested, but use it at your own discretion.

**Current Features:**

*   **RGB Keyboard Control (Four-Zone):**
    *   Allows setting static colors for up to four keyboard lighting zones.
    *   Accessible via sysfs: `/sys/devices/platform/hp-wmi/rgb_zones/zone0[0-3]_rgb`
    *   *Note: Keyboard backlighting must first be enabled using the laptop's Fn-key combination for RGB color changes to take effect.*
*   **Standard `hp-wmi` Features:**
    *   **Hotkey Support:** Omen key, brightness, volume, and other special keys are mapped to standard X11 keysyms for use with your desktop environment's hotkey manager.
    *   **Platform Profile Support:** Integration with the kernel's platform profile sysfs interface (`/sys/firmware/acpi/platform_profile`) for switching between performance, balanced, and cool/quiet modes (if supported by the specific HP model's WMI/EC).
    *   **RFKill:** Control for wireless devices (WiFi, Bluetooth, WWAN if present).
    *   **Hardware Monitoring (HWMON):** Basic fan speed reporting and control over "max fan" mode, if supported by the BIOS.
    *   **Other WMI-exposed Features:** Ambient Light Sensor (ALS) control, dock/tablet state detection, camera shutter events, etc., as provided by the underlying `hp-wmi` driver.

**Rationale for this Version:**

This module was developed by integrating RGB four-zone lighting support (originally from a community fork designed for older kernels) into a more recent version of the `hp-wmi` driver (based on Linux kernel 6.14+). The goal was to:
1.  Provide a working RGB keyboard solution for HP Omen and similar laptops on modern Linux kernels.
2.  Retain and benefit from the improvements, bug fixes, and broader hardware support present in newer upstream `hp-wmi` versions.
3.  Offer a single, more complete module instead of maintaining separate, aging forks.

## Installation

This module is best installed using DKMS (Dynamic Kernel Module Support) to ensure it is automatically rebuilt after kernel updates.

1.  **Prerequisites:**
    *   Ensure you have `dkms` installed.
        *   On Arch Linux: `sudo pacman -S dkms`
        *   On Debian/Ubuntu: `sudo apt install dkms`
    *   Install the kernel headers for your currently running kernel.
        *   On Arch Linux (e.g., for `linux-zen`): `sudo pacman -S linux-zen-headers` (replace `linux-zen` with your kernel, e.g., `linux-headers` for the standard kernel).
        *   On Debian/Ubuntu: `sudo apt install linux-headers-$(uname -r)`
    *   You'll also need `git` (to clone this repository) and a build environment (`base-devel` on Arch, `build-essential` on Debian/Ubuntu).

2.  **Clone the Repository:**
    ```bash
    git clone <URL_of_this_repository>
    cd <repository_directory_name>
    ```

3.  **Install using DKMS:**
    Run the provided DKMS installation script (or adapt `Makefile` targets if preferred):
    ```bash
    sudo make dkms-install 
    ```
    (Assuming your `Makefile` has a `dkms-install` target similar to common DKMS module setups. If not, you might need to copy files to `/usr/src/hp-wmi-<version>/` and run `sudo dkms add ...`, `sudo dkms build ...`, `sudo dkms install ...` manually, or use a `dkms.conf` file.)

    Alternatively, for a one-time build and install (not recommended for long-term use due to kernel updates):
    ```bash
    make
    sudo make install
    ```

The module will be built and installed. If using DKMS, it should automatically manage rebuilding it on kernel updates. You may need to manually load the module the first time (`sudo modprobe hp_wmi`) or reboot.

## Usage

### RGB Keyboard Color Control

The module creates files for each lighting zone in `/sys/devices/platform/hp-wmi/rgb_zones/`.
Typically, these are named `zone00_rgb`, `zone01_rgb`, `zone02_rgb`, and `zone03_rgb`.

**Important:** Ensure your keyboard backlighting is **turned on** using the laptop's Fn-key shortcut before attempting to change colors.

To change a zone's highlight color, write a hex color value in RRGGBB format to the respective file. For example:

*   Set Zone 0 to Red:
    ```bash
    echo "FF0000" | sudo tee /sys/devices/platform/hp-wmi/rgb_zones/zone00_rgb
    ```
*   Set Zone 1 to Green:
    ```bash
    sudo sh -c 'echo "00FF00" > /sys/devices/platform/hp-wmi/rgb_zones/zone01_rgb'
    ```
*   Set all zones to White (using a helper script or a loop):
    ```bash
    for i in {00..03}; do echo "FFFFFF" | sudo tee "/sys/devices/platform/hp-wmi/rgb_zones/zone${i}_rgb"; done
    ```

You can read the current color of a zone using `cat`:
```bash
cat /sys/devices/platform/hp-wmi/rgb_zones/zone00_rgb

Hotkeys
Omen-specific and other special hotkeys (brightness, volume, etc.) are mapped to regular X11 keysyms. You can use your desktop environment's (KDE, GNOME, XFCE, etc.) built-in hotkey or keyboard shortcut manager to assign these keysyms to desired functions, just like any other key.
Other Features
Refer to the standard documentation for the platform_profile and hwmon sysfs interfaces for controlling performance modes and monitoring hardware, if supported by your laptop model.
To Do / Future Enhancements:
RGB Keyboard:
FourZone brightness control (if WMI interface supports it).
Support for keyboard lighting animations (if WMI interface supports it).
More robust detection of RGB keyboard capabilities at module load time.
Fan Control:
Explore more granular fan control options beyond the basic "max fan" mode, if exposed by WMI/EC.
General:
Continuously sync with upstream hp-wmi module for bug fixes and new features.
Investigate and document any model-specific WMI commands or behaviors.
Troubleshooting
"Permission denied" when writing to sysfs files: Ensure you are using sudo correctly, for example: echo "RRGGBB" | sudo tee /path/to/sysfs/file.
Module does not load / acpi_status_to_errno error during build (older issue): This specific build issue related to acpi_status_to_errno should be resolved in this version by adapting the error handling. If you encounter build issues, ensure your kernel headers are correctly installed and match your running kernel.
RGB colors don't change: Make sure the keyboard backlight is first enabled using the laptop's Fn-key shortcut. Check dmesg for any errors from the hp_wmi module.
Contributions and feedback are welcome!
