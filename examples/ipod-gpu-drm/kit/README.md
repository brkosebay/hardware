# A8 GPU tester kit

Test the Apple A8 GPU (PowerVR GX6450) with the Linux `powervr` driver and Mesa's Vulkan driver on your device,
without building a kernel or Mesa. The kit is the `examples/ipod-gpu-drm` payload of OrchardLinux/orchard with
its kernel, loader and Vulkan bundle prebuilt.

| Board | Device | GPU status |
|---|---|---|
| `n102` | iPod touch (6th generation) | tested |
| `j96` | iPad mini 4 (Wi-Fi) | untested: please test |
| `j97` | iPad mini 4 (Wi-Fi + Cellular) | untested: please test |
| `n61` | iPhone 6 | untested: please test |
| `n56` | iPhone 6 Plus | untested: please test |

## Safety

Everything runs from RAM: checkm8, PongoOS, m1n1, then Linux. Nothing is written to the device's storage.
A reboot (hold the power and home buttons) returns to iOS. Every test step runs under a 30 s hardware watchdog,
so a frozen SoC resets by itself.

## Requirements

- A Mac or a Linux PC, a USB cable, and the device. Any iOS version.
- `python3` 3.11 or newer, and [uv](https://docs.astral.sh/uv/getting-started/installation/).
- What orchard needs to boot a device in DFU mode. `orchard/bin/orchard doctor` lists anything missing.
- Internet access. `setup.sh` fetches the GPU firmware (about 15 MB) from Apple's public iPod touch 6
  iOS 12.4 IPSW. The kit does not include any Apple files.

## Steps

`BOARD` is your board from the table above.

1. Unpack the kit and set up the payload. Keep the directory where it is afterwards, because the payload
   records its paths:

   ```sh
   tar xzf a8-gpu-tester-kit-1.tar.gz
   cd a8-gpu-tester-kit-1
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
   ```

   If those pass, vkmark is optional: `/opt/a8vk/vkmark.sh --size 256x256`.

## What to report

Post these on OrchardLinux/orchard PR #196:

- your board, e.g. `j96`;
- the output of `a8-gpu-drm-test probe`, plus `/run/a8-gpu-drm/probe.txt`. If the device froze, paste the
  serial log instead: its last kernel lines show how far the probe got;
- the `RESULT` lines of `a8-gpu-drm-test vk` and of the ladder.

## Sources

`SOURCES.txt` names the commits everything prebuilt was built from: the kernel and the kit come from
OrchardLinux/orchard (GPL-2.0 kernel, orchard's own licences); the Vulkan driver comes from OrchardLinux/mesa
(MIT). `orchard/examples/ipod-gpu-drm/kit/make-kit.sh` packed the kit.
