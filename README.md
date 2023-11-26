# evdevkm tool

## Introduction
This tool provides a software solution for switching input devices between the host operating system and a guest operating system. An arbitrary number number of devices can be specified. In short the tool works be grabbing the specified devices and routing them to either of two "virtual" devices.

## Usage
```
Usage: evdevkm [OPTION...] [Device...]
Virtual keyboard and mouse switch.

The switch capabilities extend to any device under '/dev/input' and the switch
key can be specified as an option. The intended use is with the '-g' option as
this will grab original devices and then route the input events to either the
'host' virtual devices or the 'guest' virtual devices

  -c, --code=KEY_OR_CODE     Key name or key code to be used as switch
  -g, --grab                 Grab device
  -n, --no-symlink           Create no symlinks
  -p, --print-key-codes      Print key codes
  -u, --user=UID_OR_USER     Uid or user name to assign to guest device
  -v, --verbose              Verbose output
  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version

Mandatory or optional arguments to long options are also mandatory or optional
for any corresponding short options.

Report bugs to /dev/null.
```

## Conceptually
The tool works by creating two uinput devices per device argument and then routing the device events to one or the other. The uinput devices are constructed from the original device and therefor carry the same capabilities. A hotkey is used to flip the routing of events between the two uinput devices. In order to act as a software kvm (without the 'v') switch it this program grabs the devices provided as device arguments which means that the input events are intercepted and not reaching the host os.

Before any other program grabs one of the devices both device sets are available to the host and switching between uninput device sets will effectively do nothing since the host listens to both by default. Invoking qemu with arguments to grab one of the device sets enables the kvm (without the 'v') functionality.

## A note on permissions
It is the users responsibility to ensure correct permssions. In general this tools will need read permission for the devices it is given as arguments. Furthermore, read & write permissions for `/dev/uinput` is needed to create the `host` and `guest` devices.

## Identify devices
List devices
```bash
ls -alh /dev/input/{.,by-id,by-path}
```
In case it is not obivous which device is which from the symbolic links in `/dev/input/by-id` or `/dev/input/by-path` then `cat` can be used to identify which device. For example, `cat /dev/input/event2` and then move/type attached devices one by one until there is output in the terminal.

## Building
The binary can be built with make using the standard c build toolchain plus the libevdev libray. On ubuntu the dependencies can be installed with `apt install build-essential libevdev-dev`. To build the binary:
```bash
make build
```

## Examples

### Example: qemu mouse and keyboard
In this example the mouse and keyboard are available as `/dev/input/event2` and `/dev/input/event3`, respectively. Note, that the order when listing devices to `evdevkm` is not important. 
```bash
./evdevkm -v -g /dev/input/event2 /dev/input/event3
```

Then qemu can be invoked as seen below using virtio mouse and keyboard and specifying the paths to the virtual devices to be grabbed.
```bash
qemu-system-x86_64 -enable-kvm -smp 2 -m 4096 -nic user -drive file=disk001.qcow2 \
	-monitor stdio -vnc :0 -vga qxl \
	-device virtio-mouse-pci -device virtio-keyboard-pci \
	-object input-linux,id=mouse,evdev=/dev/input/by-path/event2-guest \
	-object input-linux,id=kbd,evdev=/dev/input/by-path/event3-guest
```

