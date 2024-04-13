# Snake Mini

[![Version](https://img.shields.io/github/v/release/Snake-FW/P32-FW?style=plastic)]()

## Wanted
FW developer, testers, and graphic designers are welcome.

## Unofficial Firmware for the Prusa Mini and Mini+

Alternative FW for the Prusa Mini. There's quite few improvements:

1. ~~**Hotend fan speed**: Adds a menu option to unlock the hotend fan speed~~
  ~~and increase it from the Prusa Firmware's default 38% to anywhere from 50-100%.~~
2. **Skew compensation**: Turns on skew compensation in Marlin and allows it to be configured with `M852`.
3. ~~**OctoPrint screen**: Adds support for `M73` (print progress) and `M117`~~
  ~~(LCD messages).~~
4. ~~**PID tuning**: Automatically writes PID settings to EEPROM after `M303 U1` (autotune),~~
  ~~`M301` (set hotend PID), and `M304` (set bed PID).~~
5. **Max Temps**: Raises the maximum bed temperature from 100C to 110C
  and nozzle temperature from 275C to 285C (use with caution!).
6. ~~**EEPROM upgrade/sidegrade/downgrade**: Saved values (live-z, skew etc.) are kept during upgrade/sidegrade/downgrade.~~
7. **Settings during print**: You can change Snake settings during printing.
8. **Faster nozzle cooling**: If you wait for nozzle cooling before MBL, you can call `M109 R170 C`
  which uses print fan to speed up cooling.
9. **Game**: Instead of printing you can enjoy simple game.
10. ~~**Bigger time**: Printing and remaining time is now bigger.~~
11. **Startup wizard**: Now you can select `Ignore` at the wizard start screen to disable starting of the wizard at the printer startup.
12. ~~**Temperature calibration**: You can calibrate PID temperature control for your hotend/bed directly from the menu. Calibration does 5 cycles.~~
13. ~~**Total time**: Elapsed, Remaining and Total or End time are shown during printing.~~
14. **Change filament**: Change of filament in Tune menu is moved to submenu to avoid unwanted interruption.
15. **Adjust brightness**: You can change brightness of the display. It does not dim the light but draws darker colors.
16. ~~**Cold mode (min.temp.)**: If you enable Cold Mode, temperatures (once set) won't drop below 30°C. For safety reasons cold mode must be enabled after every start of the printer.~~
17. ~~**Show MBL and tilt**: After mesh bed leveling (G29) you can go to `Snake Settings` and see the MBL Z levels at the measured points and check the tilt of the axes. Levels are shifted to avoid negative numbers.~~
18. **Speed up (un)parking**: Parking and unparking is done at the highest speed to shorten maintenance (change filament) time during printing.
19. **Different printers**: Next to a standard version, a coreXY and a long bed version is released. The latter is suited for beds of length 250 mm ([long bed HW](https://www.aliexpress.com/item/1005001632020501.html)).

All settings are automatically saved to EEPROM and loaded on boot.

## Feed the Snake

This FW is developed in spare time. If you like it, please
consider supporting further development and updates by becoming a patron.

[![Feed the Snake on Patreon](https://img.shields.io/endpoint.svg?url=https%3A%2F%2Fshieldsio-patreon.vercel.app%2Fapi%3Fusername%3Despr14%26type%3Dpatrons&style=plastic)](https://patreon.com/espr14)

---

## Installing

### Jailbreak your Mini

You will need to cut out Prusa's appendix to install custom firmware.
Follow the instructions [here](https://help.prusa3d.com/en/article/flashing-custom-firmware-mini_14/).
This is irreversible and voids the warranty, although in the US
you are protected by the [Magnuson-Moss Warranty Act](https://www.ftc.gov/news-events/press-releases/2018/04/ftc-staff-warns-companies-it-illegal-condition-warranty-coverage).

Of course you could always buy a second Buddy board and run it on that one.

Alternatively, if you are good at very fine pitch soldering, you could
lift the BOOT0 pin off the board entirely and make your own jumpers
to connect it directly to 3.3V or GND as you need (the appendix merely
[shorts BOOT0 directly to GND](https://hackaday.com/2019/12/16/prusa-dares-you-to-break-their-latest-printer/)).

Once you have done that, you can live and let live-stock.


### Flashing

Whenever you install new firmware, it's good practice to make a note of your
settings first, particularly your Live Z Offset and your skew coefficients. Not all
FW changes keep settings saved.

Download [the latest release here](https://github.com/Snake-FW/P32-FW/releases).
Copy the `.bbf` file to the root of your USB flash drive.
Follow [the instructions here](https://help.prusa3d.com/en/guide/how-to-update-firmware-mini-mini_128421/)
to install the firmware. The bootloader will warn you the signature is
incorrect - select "Ignore".

### First Run

If you lose your Prusa EEPROM settings during the upgrade process,
when you first run the firmware, Prusa will send you to the initial
calibration wizard. This can be a problem if you need to set custom e-steps
before printing anything. Just skip the initial setup wizard,
use the Snake Settings menu to configure your e-steps, and
then rerun the setup wizard manually.

### Livestock to Stock

Download Prusa's stock firmware [here](https://www.prusa3d.com/drivers/).
Press knob at printer startup to force-install the firmware or go
to the Settings menu, scroll down to "FW Upgrade",
and change the option to "On Restart Older" (this option is only available
in Snake firmware).

To reflash the board in DFU mode, [see below](#flashing-in-dfu-mode).

---

## Configuration

To configure Snake settings, open the Settings menu and select "Snake Settings".

### Configuring E-steps

New in v1.0.7: This can now be done using Prusa's own "Experimental Settings" menu.

### Configuring Skew Compensation

The Prusa Mini+ is inherently prone to skew, by virtue of its cantilever
design. It is normal to see skew on all three axes. This affects the precision
of any parts you print. All three skew compensation coefficients are available
for use - I for XY, J for XZ, and K for YZ.

Note it is always preferable to remove as much skew as possible through physical
adjustments before using firmware skew compensation. For excellent
instructions, read [this post on Prusa's forum](https://forum.prusaprinters.org/forum/hardware-firmware-and-software-help/oh-no-were-skewed-prusa-mini-edition/).

See [the section below](#calibrating-skew) for a guide on how to measure skew
and compute the coefficients. You can use the jog wheel to set
the coefficients in this menu, or use `M852`. Either way, the settings
will automatically be saved to EEPROM - you do not need to use `M500`.
Be sure to set `Skew Correct` to `On` for the settings to be used.

Be careful with large skew correction factors - it is possible to go past
the min or max travel on the X and Y axes while printing or even during
mesh bed leveling. A skew factor of e.g. 0.01 equates to
`0.01 * 180mm = 1.8mm` of movement at the far end of the bed,
so your usable print area will be reduced accordingly.

### Configuring PID Parameters

The stock firmware allows you to run an `M303` PID autotune, but the new
settings are lost on reset. In this FW, PID settings are *automatically* written
to EEPROM after any command that updates Marlin's PID values, which could be
an `M301` (set hotend PID), `M304` (set bed PID), or an `M303 U1` (autotune and
use the PID result). These values will then be restored on reset, too. You do not
need to use `M500`.

If you need to restore the default PID values, they can be reset by running
the following commands:

* Hotend: `M301 P7.00 I0.50 D45.00`
* Bed: `M304 P120.00 I1.50 D600.0`

Note that if you run `M303` (autotune) without the `U1` parameter, Marlin
will just print out the suggested PID values without changing the settings,
and they won't get written to EEPROM.

---

## Calibrating Skew

Measuring skew on all three axes at once can be done by simply printing
[this compact calibration tower](doc/skew/skew.stl):

Use a normal layer height (0.15 or 0.2 mm) and no supports. Do not rotate the model in your slicer -
it must be printed in the same orientation as supplied in the STL.

Open [this spreadsheet](doc/skew/Skew_Calibration_Calculator.ods).
Use [calipers](https://amzn.to/3vVRgOl) to measure the six diagonals,
conveniently labeled A to F, and type the measurements
into the spreadsheet. It will calculate your three skew correction factors.
Add the `M852` command with the factors to the start G-code in your slicer.

If you want to check your calibration is accurate,
print the same tower with skew correction enabled. The diagonals should
then all have the same length (within measurement error of course). If not,
update the table and the factors.

---

## Flashing in DFU Mode

If the bootloader refuses to accept firmware from a USB flash drive,
it's possible to flash the board directly in DFU mode.

Compile the firmware and build a DFU file:

```
$ python3 utils/build.py --generate-dfu --bootloader yes
```

If you built it from another machine, copy it to your Pi:

```
$ scp build/mini_release_boot/firmware.dfu <user>@<pi-host>:~/
```

Put your Buddy board in DFU mode by placing a jumper across the relevant pins
and resetting. If you have a 3-pin header next to the appendix (older versions
of the board), put the jumper between BOOT0 and 3.3V. If you have a 2-pin header,
just add a jumper.

Then flash from your Pi:

```
$ lsusb
Bus 001 Device 010: ID 0483:df11 STMicroelectronics STM Device in DFU Mode
$ sudo apt install dfu-util
$ dfu-util -a 0 -D firmware.dfu
```

Don't forget to remove the jumper before resetting.

---

## Attribution
Snake FW is
- based on [Llama Mini Firmware](https://github.com/matthewlloyd/Llama-Mini-Firmware)
- based on [Prusa Firmware Buddy](https://github.com/prusa3d/Prusa-Firmware-Buddy)
- based on [Marlin Firmware](https://github.com/MarlinFirmware/Marlin)

Precise information of who did what can be obtained by `git blame` command or by `Blame` button in the Github file reader.

## Original Prusa Mini Firmware README
<details>
<summary>Click to expand!</summary>

# Buddy
[![GitHub release](https://img.shields.io/github/release/prusa3d/Prusa-Firmware-Buddy.svg)](https://github.com/prusa3d/Prusa-Firmware-Buddy/releases)
[![Build Status](https://holly.prusa3d.com/buildStatus/icon?job=Prusa-Firmware-Buddy%2FMultibranch%2Fmaster)](https://holly.prusa3d.com/job/Prusa-Firmware-Buddy/job/Multibranch/job/master/)

This repository includes source code and firmware releases for the Original Prusa 3D printers based on the 32-bit ARM microcontrollers.

The currently supported models are:
- Original Prusa MINI
- Original Prusa MK4
- Original Prusa XL

## Getting Started

### Requirements

- Python 3.8 or newer

### Cloning this repository

Run `git clone https://github.com/prusa3d/Prusa-Firmware-Buddy.git`.

### Building (on all platforms, without an IDE)

Run `python utils/build.py`. The binaries are then going to be stored under `./build/products`.

- Without any arguments, it will build a release version of the firmware for all supported printers and bootloader settings.
- To generate `.bbf` versions of the firmware, use: `./utils/build.py --generate-bbf`.
- Use `--build-type` to select build configurations to be built (`debug`, `release`).
- Use `--preset` to select for which printers the firmware should be built.
- By default, it will build the firmware in "prerelease mode" set to `beta`. You can change the prerelease using `--prerelease alpha`, or use `--final` to build a final version of the firmware.
- Use `--host-tools` to include host tools in the build (`bin2cc`, `png2font`, ...)
- Find more options using the `--help` flag!

#### Examples:

Build the firmware for MINI and XL in `debug` mode:

```bash
python utils/build.py --preset mini,xl --build-type debug
```

Build the firmware for MINI using a custom version of gcc-arm-none-eabi (available in `$PATH`) and use `Make` instead of `Ninja` (not recommended):

```bash
python utils/build.py --preset mini --toolchain cmake/AnyGccArmNoneEabi.cmake --generator 'Unix Makefiles'
```

#### Windows 10 troubleshooting

If you have python installed and in your PATH but still getting cmake error `Python3 not found.` Try running python and python3 from cmd. If one of it opens Microsoft Store instead of either opening python interpreter or complaining `'python3' is not recognized as an internal or external command,
operable program or batch file.` Open `manage app execution aliases` and disable `App Installer` association with `python.exe` and `python3.exe`.

### Development

The build process of this project is driven by CMake and `build.py` is just a high-level wrapper around it. As most modern IDEs support some kind of CMake integration, it should be possible to use almost any editor for development. Below are some documents describing how to setup some popular text editors.

- [Visual Studio Code](doc/editor/vscode.md)
- [Vim](doc/editor/vim.md)
- [Eclipse, STM32CubeIDE](doc/editor/stm32cubeide.md)
- [Other LSP-based IDEs (Atom, Sublime Text, ...)](doc/editor/lsp-based-ides.md)

#### Formatting

All the source code in this repository is automatically formatted:

- C/C++ files using [clang-format](https://clang.llvm.org/docs/ClangFormat.html),
- Python files using [yapf](https://github.com/google/yapf),
- and CMake files using [cmake-format](https://github.com/cheshirekow/cmake_format).

If you want to contribute, make sure to install [pre-commit](https://pre-commit.com) and then run `pre-commit install` within the repository. This makes sure that all your future commits will be formatted appropriately. Our build server automatically rejects improperly formatted pull requests.

#### XL and Puppies

With the XL, the situation gets a bit more complex. The firmware of XLBuddy contains firmwares for the puppies (Dwarf and Modularbed) to flash them when necessary. We support several ways of dealing with those firmwares when developing:

1. Build Dwarf/Modularbed firmware automatically and flash it on startup by XLBuddy (the default)
    - The Dwarf & ModularBed firmware will be built from this repo.
    - The puppies are going to be flashed on startup by the XLBuddy. The puppies have to be running the [Puppy Bootloader](http://github.com/prusa3d/Prusa-Bootloader-Puppy).

2. Build Dwarf/Modularbed from a given source directory and flash it on startup by XLBuddy.
    - Specify `DWARF_SOURCE_DIR`/`MODULARBED_SOURCE_DIR` CMake cache variable with the local repo you want to use.
    - Example below would build modularbed's firmware from /Projects/Prusa-Firmware-Buddy-ModularBed and include it in the xlBuddy firmware.
    ```
    cmake .. --preset xl_release_boot -DMODULARBED_SOURCE_DIR=/Projects/Prusa-Firmware-Buddy-ModularBed
    ```
    - You can also specify the build directory you want to use:
    ```
    cmake .. --preset xl_release_boot \
        -DMODULARBED_SOURCE_DIR=/Projects/Prusa-Firmware-Buddy-ModularBed  \
        -DMODULARBED_BINARY_DIR=/Projects/Prusa-Firmware-Buddy-ModularBed/build
    ```
3. Use pre-built Dwarf/Modularbed firmware and flash it on startup by xlBuddy
    - Specify the location of the .bin file with `DWARF_BINARY_PATH`/`MODULARBED_BINARY_PATH`.
    - For example
    ```
    cmake .. --preset xl_release_boot -DDWARF_BINARY_PATH=/Downloads/dwarf-4.4.0-boot.bin
    ```

4. Do not include any puppy firmware, and do not flash the puppies by XLBuddy.
    ```
    -DENABLE_PUPPY_BOOTLOAD=NO
    ```
    - With the `ENABLE_PUPPY_BOOTLOAD` set to false, the project will disable Puppy flashing & interaction with Puppy bootloaders.
    - It is up to you to flash the correct firmware to the puppies (noboot variant).

5. Keep bootloaders but do not write firmware on boot.
    ```
    -DPUPPY_SKIP_FLASH_FW=YES
    ```
    - With the `PUPPY_SKIP_FLASH_FW` set to true, the project will disable Puppy flashing on boot.
    - You can keep other puppies that are not debugged in the same state as before.
    - Use puppy build config with bootloaders (e.g. `xl-dwarf_debug_boot`) on one or more puppies.
    - Recommend breakpoint at the end of `puppy_task_body()` to prevent buddy from resetting the puppy immediately when puppy stops on breakpoint.

See /ProjectOptions.cmake for more information about those cache variables.

#### Running tests

```bash
mkdir build-tests
cd build-tests
cmake ..
make tests
ctest .
```

The simplest way to to debug (step through) a test is to specify CMAKE_BUILD_TYPE when configuring `cmake -DCMAKE_BUILD_TYPE=Debug ..` , build it with `make tests` as previously stated and then run the test with `gdb <path to test binary>` e.g. `gdb tests/unit/configuration_store/eeprom_unit_tests`.

## Flashing Custom Firmware

To install custom firmware, you have to break the appendix on the board. Learn how to in the following article https://help.prusa3d.com/article/zoiw36imrs-flashing-custom-firmware.

## Feedback

- [Feature Requests from Community](https://github.com/prusa3d/Prusa-Firmware-Buddy/labels/feature%20request)

## Credits

- [Marlin](https://marlinfw.org/) - 3D printing core driver
- [Klipper](https://www.klipper3d.org/) - input shaper code based on Klipper

## License

The firmware source code is licensed under the GNU General Public License v3.0 and the graphics and design are licensed under Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0). Fonts are licensed under different license (see [LICENSE](LICENSE.md)).
</details>
