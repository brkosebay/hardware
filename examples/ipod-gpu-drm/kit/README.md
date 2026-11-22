# A8 GPU tester kit

Test the Apple A8 GPU (PowerVR GX6450) with the Linux `powervr` driver and Mesa's Vulkan driver, and the A8's
display pipe with the `adbe` KMS driver, on your device, without building a kernel or Mesa. The kit is the
`examples/ipod-gpu-drm` payload of OrchardLinux/orchard with its kernel, loader and Vulkan bundle prebuilt.

The kit runs only Vulkan. Its Mesa build has no OpenGL, GLES or EGL (no Zink), so no step runs GL and there is
no GL renderer to check.

| Board | Device | Panel link | GPU | Display (`adbe`) |
|---|---|---|---|---|
| `n102` | iPod touch (6th generation) | MIPI DSI | tested | tested: the KMS test program passed with the driver built in; loading it as a module not yet |
| `j96` | iPad mini 4 (Wi-Fi) | eDP | untested: please test | untested: please test |
| `j97` | iPad mini 4 (Wi-Fi + Cellular) | eDP | untested: please test | untested: please test |
| `n61` | iPhone 6 | MIPI DSI | untested: please test | untested: please test |
| `n56` | iPhone 6 Plus | MIPI DSI | untested: please test | untested: please test |

## Safety

Everything runs from RAM: checkm8, PongoOS, m1n1, then Linux. Nothing is written to the device's storage.
A reboot (hold the power and home buttons) returns to iOS. Every test step runs under a 30 s hardware watchdog,
so a frozen SoC resets by itself. The payload boots on the boot loader's framebuffer (simpledrm); the display
driver is loaded only by the display step.

## Requirements

- A Mac or a Linux PC, a USB cable, and the device. Any iOS version.
- [uv](https://docs.astral.sh/uv/getting-started/installation/). It provides the Python that orchard runs on.
- What orchard needs to boot a device in DFU mode: libusb, palera1n (orchard downloads it), and on Linux USB
  permissions. `orchard/bin/orchard setup-host` installs what is missing and does not touch a device. Building
  tools such as podman are not needed.
- Internet access. `setup.sh` fetches the GPU firmware (about 15 MB) from Apple's public iPod touch 6
  iOS 12.4 IPSW. The kit does not include any Apple files.

## Steps

`BOARD` is your board from the table above.

1. Unpack the kit and set up the payload. Keep the directory where it is afterwards, because the payload
   records its paths:

   ```sh
   tar xzf a8-gpu-tester-kit-4.tar.gz
   cd a8-gpu-tester-kit-4
   ./setup.sh BOARD
   ```

   `setup.sh` ends with `kit: the payload is ready`.
2. Put the device in DFU mode, then run `./run.sh BOARD` and answer `y` to `Boot this device?`.
3. Open the device's new USB serial port. You get a root shell:

   ```sh
   screen /dev/cu.usbmodem* 115200   # macOS
   screen /dev/ttyACM0 115200        # Linux
   ```

4. Run the tests in this order. After a step fails, nothing more runs until the next boot.

   ```sh
   a8-gpu-drm-test probe        # loads powervr, powers the GPU, boots its firmware
   cat /run/a8-gpu-drm/probe.txt
   a8-gpu-drm-test vk           # Vulkan probe, compute and a triangle through Mesa
   for r in $(/opt/a8vk/a8-vk-ladder.sh list | awk 'NR>1 && /^  /{print $1}'); do /opt/a8vk/a8-vk-ladder.sh $r; done
   a8-gpu-drm-test display      # loads the display driver: modeset, page flips, a GPU buffer on screen
   cat /run/a8-gpu-drm/display.txt
   ```

   If a GPU step fails, reboot and run `a8-gpu-drm-test display` alone: it needs no GPU step (it then skips
   only the GPU buffer check). If those pass, vkmark is optional: `/opt/a8vk/vkmark.sh --size 256x256`.

5. What the display step does and shows. It prints one `RESULT display-...` line per check, then
   `RESULT display PASS` or `FAIL`:

   | Line | Checks |
   |---|---|
   | `display-bind` | the `adbe` driver takes the panel over from simpledrm (`[drm] Initialized adbe`) |
   | `display-dart` | its self-check of scanout through the display DART passes (`PASS`), or it stays in bypass (`BYPASS`, not a failure) |
   | `display-crc` | the self-check's blend CRCs: the test pattern reads back the same through the DART |
   | `display-mode` | the panel mode matches your panel: 640x1136 (iPod), 750x1334 (iPhone 6), 1080x1920 (iPhone 6 Plus), 1536x2048 (iPad mini 4) |
   | `display-link` | the connector is `eDP` on the iPad mini 4 and `DSI` on the phones and the iPod |
   | `display-modeset` | a modeset to a buffer of colour bars in a white border |
   | `display-flip` | 30 page flips at the panel's 60 Hz, each acknowledged by the vblank interrupt |
   | `display-import` | a `powervr` buffer (an orange and blue checkerboard) scanned out through the display DART |
   | `display-kernel` | no flip timeout, pipe error, DART fault or kernel warning while it ran |

   While it runs, the screen should flicker three times (the self-check), then show the colour bars, flash
   between two bar orders for half a second, and show the checkerboard for 2 s. Then the text console returns.

## What to report

Post these on OrchardLinux/orchard PR #196:

- your board, e.g. `j96`;
- the output of `a8-gpu-drm-test probe`, plus `/run/a8-gpu-drm/probe.txt`. If the device froze, paste the
  serial log instead: its last kernel lines show how far the probe got;
- the `RESULT` lines of `a8-gpu-drm-test vk` and of the ladder;
- the output of `a8-gpu-drm-test display`, plus `/run/a8-gpu-drm/display.txt`, and what the screen showed
  (bars, checkerboard, or nothing). If the device froze or reset, paste the serial log: the step streams
  every kernel line as it runs.

## Sources

`SOURCES.txt` names the commits everything prebuilt was built from: the kernel and the kit come from
OrchardLinux/orchard (GPL-2.0 kernel, orchard's own licences); the Vulkan driver comes from OrchardLinux/mesa
(MIT). `orchard/examples/ipod-gpu-drm/kit/make-kit.sh` packed the kit.
