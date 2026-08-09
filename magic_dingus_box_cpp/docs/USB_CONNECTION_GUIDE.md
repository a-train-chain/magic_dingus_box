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
3. **Open** **`http://dingus.box`** in your browser

That's it. Over the cable the box answers **any** address you type —
`http://dingus.box` is just the advertised name. If your browser insists
on searching instead of navigating, use the numeric fallback:
`http://10.55.0.1:5000`.

Note: your box's USB-C data port is separate from its power connection on
standard units — if plugging in a laptop powers the box off, use Wi-Fi
instead and contact us.

---

## Platform Notes

### macOS (one-time step)

The Mac usually needs one-time confirmation that the new network
interface uses DHCP:

1. Plug in the cable
2. Open **System Settings** → **Network**
3. Select **"RNDIS/Ethernet Gadget"** (or similarly named USB interface)
   in the sidebar
4. Make sure **Configure IPv4** is **"Using DHCP"**, then Apply
5. Open `http://dingus.box`

Terminal equivalent of steps 2-4:

```bash
sudo networksetup -setdhcp "RNDIS/Ethernet Gadget"
```

After the first time, the Mac remembers — plugging in just works.

### Windows

Windows 10/11 typically recognizes the box automatically as an RNDIS
network device — plug in, wait for the "new network" notification, open
`http://dingus.box`.

If no network device appears:

1. Open Device Manager
2. Find "Other devices" → right-click the unknown device
3. Update driver → Browse → "Let me pick" → Network adapters
4. Select "Microsoft" → "Remote NDIS Compatible Device"

### Linux

Just works: the `usb0` interface auto-configures via DHCP on any modern
distribution. Open `http://dingus.box`.

If your distribution doesn't run a DHCP client on hotplugged interfaces:

```bash
sudo dhclient usb0
```

---

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| No network device appears | Charge-only cable | Use a data-capable USB-C cable |
| `dingus.box` won't load | OS hasn't picked up DHCP yet | Wait ~10 s; on macOS do the one-time DHCP step above; try `http://10.55.0.1:5000` |
| Browser searches instead of navigating | Address typed without `http://` | Type the full `http://dingus.box` |
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
  scoped to `usb0` only)
- **Why any address works:** a catch-all DNS rule on `usb0`
  (`address=/#/10.55.0.1`) resolves every name to the box, and a
  port-80→5000 redirect makes plain `http://` URLs land on the Content
  Manager
- **`.local` caveat:** the box's `<hostname>.local` address resolves via
  your existing Wi-Fi/mDNS path, not the cable — use `dingus.box` or
  `10.55.0.1:5000` to guarantee USB speed
- **USB protocol:** CDC-ECM (macOS/Linux), RNDIS (Windows)
- **Speed:** ~30-40 MB/s (USB 2.0)
