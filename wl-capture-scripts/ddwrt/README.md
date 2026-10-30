# wl-capture-scripts/ddwrt — running the capture scripts on DD-WRT

DD-WRT's `wl` (`src/router/utils/wl.modern.arm` for Northstar) is cut down: it
has no `up`, `isup`, `revinfo`, `bss` or `ap`, and the capture scripts need
all five. There is no full `wl` in the DD-WRT tree.

- `wlx.c` sends those ioctls directly (`WLC_UP`, `WLC_DOWN`, `WLC_GET_UP`,
  `WLC_GET_REVINFO`, `WLC_SET_AP`, `WLC_SET_SSID`, the `bss` iovar, and any
  integer iovar). Its output mimics `wl` where the scripts parse it
  (`revinfo` prints `deviceid 0x....`, `isup` prints `0`/`1`).
- `wl-shim.sh` goes in front of the real `wl`: the missing commands go to
  `wlx`, everything else to `/usr/sbin/wl`. It also sends `ap 1` before
  `ssid`: after `insmod wl` the driver is a STA (DD-WRT sets AP mode in its
  own `wlconf`), and `ssid` + `bss up` on a STA bring no BSS up.

With both, the scripts ran unchanged on a Netgear R7000
([`../../router-data/r7000/`](../../router-data/r7000/)).

## Build and install

Static, with any ARM musl toolchain (the static libc may drop `printf`
output, so `wlx` writes with `write(2)`):

```sh
arm-openwrt-linux-muslgnueabi-gcc -static -Os -Wall -o wlx wlx.c
```

On the router:

```sh
cp wlx /tmp/wlx; chmod +x /tmp/wlx
mkdir -p /tmp/wlbin; cp wl-shim.sh /tmp/wlbin/wl; chmod +x /tmp/wlbin/wl
PATH=/tmp/wlbin:$PATH WL_KO=/lib/modules/$(uname -r)/wl.ko KILL='nas wland' \
    IF=eth1 DEVID=0x43a1 SSID=test-ap BR=br0 sh capture_cold_init.sh 2g20
```

`wlx IF revinfo` hangs while the radios are disabled in the DD-WRT web
interface; enable them first.

## Other DD-WRT traps

- **No `printk` in out-of-tree modules.** DD-WRT's top Makefile builds modules
  with `KBUILD_CFLAGS_MODULE := -DMODULE -DCONFIG_NOPRINTK`, which compiles
  out every `printk`: `wl_diag` and `wl-mmio-trap` load but log nothing.
  Override it on the command line:

  ```sh
  make -C <dd-wrt>/src/linux/universal/linux-4.4 M=$PWD ARCH=arm \
      CROSS_COMPILE=arm-openwrt-linux-muslgnueabi- KBUILD_CFLAGS_MODULE=-DMODULE modules
  ```

- **ssh brute-force guard.** DD-WRT blocks a client after more than four
  connections without a login (a host-key mismatch or `ssh-keyscan` counts),
  and every further try multiplies the block by five. When the host key
  changes (after a reflash, or at a boot without JFFS), take the new
  fingerprint over telnet first, then log in once.
