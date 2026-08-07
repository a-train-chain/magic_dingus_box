# Magic Dingus Box — Owner's Guide

Welcome! Your Magic Dingus Box is a retro game console and video player
that lives under your TV. Games work the moment you plug it in — no
account, no internet, no setup. The optional **Movies** section takes
about 15 minutes to set up and needs three things you provide yourself:
your Wi-Fi, a VPN subscription, and a free movie-info account. This
guide walks through all of it.

---

## 1. First power-on

1. Connect the HDMI cable to your TV and **turn the TV on first**,
   switched to the right input.
2. Flip the power switch on the box. The first boot takes about two
   minutes while the box sets itself up (later boots are ~20 seconds).
3. You'll see the intro video, then the main menu. Plug in a controller
   and play — every game playlist on the main menu works right now.

**The two physical controls on the box:**

- **Power switch** — flip it off to put the box in standby (the lights
  sweep down, ~5 seconds); flip it on to wake it (~10 seconds).
- **Restart button** — restarts the kiosk software (~10 seconds back to
  the menu). If a game ever freezes, press this: your game progress is
  saved automatically first.

## 2. Playing games

- **Browse** playlists with the D-pad; **A** selects.
- **Exit a game** (from the player-1 controller):
  - N64-style pad: **hold Z, then press Start**
  - PlayStation-style pad: **hold Select, then press Start**
- Your progress saves automatically when you exit, and picks up where
  you left off next time you launch the game.
- **Settings menu**: press **B** (the Circle button on a
  PlayStation-style pad). Everything else in this guide starts there.

## 3. Connect to your Wi-Fi

1. Press **B** to open Settings.
2. Choose **Wi-Fi → Scan Networks**.
3. Pick your network and type the password on the on-screen keyboard
   (D-pad to move, A to type). Tip: after you pair your phone
   (section 5), you can type passwords with your phone's keyboard
   instead.
4. Done. The box remembers the network.

Games never need the internet — Wi-Fi is for movies, the phone remote,
and software updates.

## 4. Find your box's web page (the Content Manager)

Your box hosts its own settings website on your home network — that's
where the movie setup happens.

1. Press **B** → **Content Manager**. The screen shows the box's
   address (it looks like `http://magicpi-a3f2.local:5000` — every box
   has its own name) and a QR code.
2. Scan the QR code with your phone, or type the address into any
   browser on the same Wi-Fi. A **Connect a Device** page opens — pick
   **Manage movies & playlists** to reach the **Content Manager**.

**No Wi-Fi, or want faster uploads?** Plug a USB-C cable from the box
into your computer, then open **`http://dingus.box`** in a browser —
over the cable, any address you type reaches the box, with no network
settings to configure.

## 5. Pair your phone as a remote (optional, recommended)

1. Press **B** → **Connect Phone / Computer**. A QR code appears.
2. Scan it with your phone and pick **Use this phone as a remote**. The
   remote page opens and stays paired — you can use it as a D-pad, and
   whenever the box shows a text field (like a Wi-Fi password), your
   phone's keyboard types straight into it.

## 6. Unlock the Movies section

The Movies section is hidden until you, the owner, unlock it with this
sequence on the box's front panel, from the main menu:

1. **Press and hold BTN1 + BTN3 together** (the two outer buttons) for
   about half a second.
2. **Tap BTN2 three times** within two seconds.
3. **Press the rotary knob in** (one click).

You'll see "Media Browser unlocked" on the TV, and a **Media Browser**
tab appears in the Content Manager (refresh the page). This only ever
needs doing once.

## 7. Set up the VPN (movie downloads)

Movie downloads run through a VPN for privacy — and only through it: if
the VPN is off, downloads simply don't happen. You bring your own VPN
subscription. We recommend **ProtonVPN** on the **Plus** plan (the free
plan doesn't support port forwarding, which makes downloads much
slower to start).

**Get your config file from ProtonVPN:**

1. Sign up at **protonvpn.com** (Plus plan).
2. Log in to the ProtonVPN website → **Downloads** → **WireGuard
   configuration**.
3. Give the config any name, choose a **P2P** server in a nearby
   country, and — **important** — switch **NAT-PMP (port forwarding)
   ON** before creating it.
4. Download the file. It ends in `.conf`.

**Give it to the box:**

5. Open the Content Manager → **Media Browser** tab.
6. Drag the `.conf` file into the VPN box on that page (or click to
   browse for it).
7. Wait about 90 seconds while the box builds its download system. The
   page shows progress; when it finishes, **Movies** appears on the TV's
   main menu.

Other WireGuard-based VPN providers also work (the box accepts any
standard WireGuard `.conf`), but ProtonVPN is the one the box's
port-forwarding automation is built for.

## 8. Add your movie-info account (TMDB)

Movie posters, descriptions, and search come from The Movie Database
(TMDB) — free, takes two minutes:

1. Create a free account at **themoviedb.org**.
2. Go to **Settings → API** (that's
   `themoviedb.org/settings/api`) and request an API key. Personal use;
   any details are fine.
3. Copy the value labelled **"API Key"** — the short one, NOT the long
   "API Read Access Token".
4. In the Content Manager → **Media Browser** tab, paste it into the
   **Movie Info (TMDB)** box and save. The page checks it on the spot
   and tells you if the box needs a quick restart (flip the power
   switch off and on).

## 9. Add a movie drive

Movies are stored on a USB drive so they never crowd the games.

1. Plug any USB drive into the box (it will be **completely erased**,
   so use one with nothing you want to keep).
2. Content Manager → **Media Browser** tab → **Movie Storage** →
   choose the drive → confirm.
3. The box formats it and takes it from there. Leave it plugged in.

## 10. Using Movies

- Open **Movies** from the main menu. Browse or search, pick a title,
  and add it — the box finds it, downloads it through the VPN, and it
  appears in your Library, usually within the hour for popular titles.
- TV shows work the same way, season by season and episode by episode,
  with resume, next-episode countdowns, and a watched/unwatched filter.

**What the messages mean:**

| On screen | Meaning |
|---|---|
| "Movies (configure VPN)" | The VPN step (section 7) hasn't been done yet |
| "Movies (drive not connected)" | Plug the movie drive back in |
| "Media Browser unavailable — VPN tunnel down" | The VPN dropped; the box reconnects on its own, usually within a couple of minutes |

## 11. If something's stuck

- **Movies missing their pictures, updates failing, or things "half
  working"?** → Content Manager → **Settings** tab → **Network Doctor**
  → Run Network Test. It checks the box's connection step by step and
  tells you in plain English what's wrong — often something in your
  Wi-Fi router's settings — and what to do about it. Screenshot the
  result if you need help.
- **Game frozen** → press the restart button on the box. ~10 seconds,
  saves intact.
- **Box unresponsive** → flip the power switch off, wait five seconds,
  flip it on.
- **Movies section misbehaving** → most things heal themselves within
  15 minutes (the box retries stuck downloads automatically). A power
  switch off/on fixes nearly everything else.
- **Phone remote stopped working** → re-scan the QR under Settings →
  Connect Phone / Computer.

Enjoy the box!
