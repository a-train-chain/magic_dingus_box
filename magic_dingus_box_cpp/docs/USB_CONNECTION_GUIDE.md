# USB Connection Guide

Connect a laptop directly to the Magic Dingus Box with a USB-C cable for
the fastest uploads — no Wi-Fi or network settings needed.

<!-- TODO(alex): confirm enclosure port wiring — which USB-C port(s) are
     exposed on production units, and whether the data port is distinct
     from the power input. The neutral wording below must not promise
     either way until that's settled. -->

## Quick Start

1. **Plug** a USB-C cable from your computer into the box
2. **Wait** about 10 seconds for the connection to appear
3. **Open** **`http://10.55.0.1`** in your browser

That's it. The box is always `10.55.0.1` on the cable, and it works even
while your computer is also on Wi-Fi. The TV shows the same address under
**Settings → Connect a Device**.

`http://dingus.box` also reaches the box, but only when your computer's
Wi-Fi is off: with Wi-Fi on, the computer asks the Wi-Fi network to look
names up, and that network has never heard of `dingus.box`.

Note: your box's USB-C data port is separate from its power connection on
standard units — if plugging in a laptop powers the box off, use Wi-Fi
instead and contact us.

---

## Platform Notes

### macOS

Plug in and open `http://10.55.0.1` — a Mac picks up the cable's address
on its own.

If the page doesn't load after ~10 seconds, check that the new interface
uses DHCP (it does by default):

1. Open **System Settings** → **Network**
2. Select **"RNDIS/Ethernet Gadget"** (or similarly named USB interface)
   in the sidebar
3. Make sure **Configure IPv4** is **"Using DHCP"**, then Apply
4. Open `http://10.55.0.1`

Terminal equivalent of steps 1-3:

```bash
sudo networksetup -setdhcp "RNDIS/Ethernet Gadget"
```

### Windows

Windows 10/11 typically recognizes the box automatically as an RNDIS
network device — plug in, wait for the "new network" notification, open
`http://10.55.0.1`.

If no network device appears:

1. Open Device Manager
2. Find "Other devices" → right-click the unknown device
3. Update driver → Browse → "Let me pick" → Network adapters
4. Select "Microsoft" → "Remote NDIS Compatible Device"

### Linux

Just works: the `usb0` interface auto-configures via DHCP on any modern
distribution. Open `http://10.55.0.1`.

If your distribution doesn't run a DHCP client on hotplugged interfaces:

```bash
sudo dhclient usb0
```

---

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| No network device appears | Charge-only cable | Use a data-capable USB-C cable |
| `http://10.55.0.1` won't load | Computer hasn't picked up the cable's address yet | Wait ~10 s; on macOS check the DHCP setting above; try `http://10.55.0.1:5000` |
| `dingus.box` won't load | Computer is also on Wi-Fi | Use `http://10.55.0.1` (or turn Wi-Fi off) |
| Browser searches instead of navigating | Address typed without `http://` | Type the full `http://10.55.0.1` |
| Page loads but slowly | USB 1.1 fallback | Re-seat both ends; try another cable |
| Box turns off when cable plugged in | Port wiring on your unit | Unplug, power back on, use Wi-Fi instead — and contact us |

### Check from the box (SSH)

```bash
/opt/magic_dingus_box/scripts/usb_gadget_status.sh
```

---

## Technical Details

- **Box IP on the cable:** `10.55.0.1` (static on `usb0`)
- **Your computer gets:** `10.55.0.10` – `10.55.0.100` via DHCP (dnsmasq,
  scoped to `usb0` only), with `10.55.0.1` as its DNS server
- **Why `10.55.0.1` and not a name:** a numeric address needs no name
  lookup, so it works whatever else the computer is connected to. The
  catch-all DNS rule on `usb0` (`address=/#/10.55.0.1`) still resolves
  every name — `dingus.box` included — to the box, but a computer that is
  also on Wi-Fi sends its lookups to the Wi-Fi network's DNS first
  (tested on macOS, 2026-10-27), so names only work with Wi-Fi off
- **Port 80:** a redirect sends plain `http://10.55.0.1` (and any other
  name) to the Content Manager on port 5000
- **`.local` caveat:** the box's `<hostname>.local` address resolves via
  your existing Wi-Fi/mDNS path, not the cable — use `10.55.0.1` to
  guarantee USB speed
- **USB protocol:** CDC-ECM (macOS/Linux), RNDIS (Windows)
- **Speed:** ~30-40 MB/s (USB 2.0)
