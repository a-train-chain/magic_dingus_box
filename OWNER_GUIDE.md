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
   switched to the right input. Either of the box's two HDMI ports
   works — picture and sound follow whichever one the TV is on.
2. Flip the power switch on the box. The first boot takes about two
   minutes while the box sets itself up (later boots are ~20 seconds).
3. You'll see the intro video, then the main menu. Plug in a controller
   and play — every game playlist on the main menu works right now.

Rather flip channels than pick from a list? Turn on the **Channel Guide**
(section 12).

**The two physical controls on the box:**

- **Power switch** — flip it off to put the box in standby (the lights
  sweep down, ~5 seconds); flip it on to wake it (~10 seconds).
- **Restart button** — restarts the kiosk software (~10 seconds back to
  the menu). If a game ever freezes, press this: your game progress is
  saved automatically first.

## 2. Playing games

- **Browse** playlists with the D-pad; **A** selects.
- **Exit a game** (from the player-1 controller):
  - N64-style pad: **press Z and Start together**
  - PlayStation-style pad: **hold Select, then press Start**
- Your progress saves automatically when you exit, and picks up where
  you left off next time you launch the game.
- **Settings menu**: press **B** (the Circle button on a
  PlayStation-style pad). Everything else in this guide starts there.

**Using your own wireless controller** (PlayStation 4 or 5, Xbox,
Switch Pro, 8BitDo and most other Bluetooth controllers):

1. Open Settings and choose **Pair Wireless Controller**. You can use
   the knob and buttons on the front of the box if you have no
   controller working yet.
2. Put the controller in pairing mode — the screen shows how for each
   kind (on a PlayStation 5 controller, hold **Create** and the **PS**
   button until the light flashes).
3. The box finds it and connects. Press any button on it when asked.
4. PlayStation, Xbox, Switch Pro and 8BitDo controllers are ready to
   play straight away. For anything else the box walks you through
   setting up its buttons.

After that, just press the controller's PS / Home / Xbox button to
connect it again. On these controllers the bottom face button selects
and the right one opens Settings, and you **exit a game by holding
Select (Share, View or −) and pressing Start (Options, Menu or +)**.
If a wireless controller ever feels laggy, connect the box to the
internet with a network cable or a 5 GHz Wi-Fi network — Bluetooth
shares a radio with 2.4 GHz Wi-Fi.

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

1. Press **B** → **Connect a Device**. The screen shows a QR code, a
   6-digit code, and the box's address (it looks like
   `http://magicpi-a3f2.local:5000` — every box has its own name).
2. Scan the QR code with your phone, or type the address into any
   browser on the same Wi-Fi. A **Connect a Device** page opens — pick
   **Manage movies & playlists** to reach the **Content Manager**.

Opening the Content Manager also sets the box's clock to your phone's or
computer's time zone, so the Channel Guide's times are your local times.

**No Wi-Fi, or want faster uploads?** Plug a USB-C cable from the box
into your computer, then open **`http://10.55.0.1`** in a browser. No
network settings to configure, and it works even while your computer is
on Wi-Fi.

## 5. Pair your phone as a remote (optional, recommended)

1. Press **B** → **Connect a Device** — the same screen as section 4.
2. Scan the QR code with your phone and pick **Use this phone as a
   remote**. The remote page opens and stays paired — you can use it as
   a D-pad, and whenever the box shows a text field (like a Wi-Fi
   password), your phone's keyboard types straight into it. Once paired,
   tap **Share → Add to Home Screen** on whichever page you're on — the
   installed app opens already paired.

## 6. Unlock the Movies section

The Movies section is hidden until you, the owner, unlock it with this
sequence on the box's front panel, from the main menu:

1. **Press and hold BTN1 + BTN3 together** (the two outer buttons) for
   about half a second.
2. **Tap BTN2 three times** within two seconds.
3. **Press the rotary knob in** (one click).

You'll see "Movie section unlocked" on the TV, and a **Media Browser**
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

**When a VPN server stops working (ProtonVPN only),** the box notices
within a minute or two and switches to another server in the same
country by itself, without you doing anything. You can also allow a
**backup country** for when several servers in your country fail in a
row: Content Manager → **Media Browser** tab → **Advanced** → **Allowed
fallback countries** → pick one or more → **Apply**. None are allowed
unless you choose them.

**Changing the VPN country (ProtonVPN only).** The box uses servers in
the Netherlands unless you choose otherwise. If downloads keep stalling
because the VPN keeps dropping (the Box Health check tells you — see
section 11), a different country often helps: Content Manager → **Media
Browser** tab → **Advanced** → **VPN server country** → pick one →
**Apply** (tap twice to confirm). The VPN reconnects and downloads pause
for about a minute. Your choice is kept if you later re-upload a config
with **Reconfigure**. With a non-ProtonVPN config the country comes from
the config file itself, so this setting is shown but can't be changed —
download a config for a server in the country you want instead.

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
- With the Channel Guide turned on (section 12), your downloaded films
  also become channels: MOVIE MIX plus genre channels such as KIDS,
  COMEDY and SCI-FI. A channel appears once it has at least three
  films, and KIDS only shows G and PG family and animated films. Movie
  channels play from 6 AM to 1 AM; overnight the box parks on channel 00
  and comes back at 6 AM. A film you pick yourself always plays to the
  end.
- After a few episodes without input, the box asks whether to keep
  going — a fresh press keeps it rolling; otherwise it stops and saves
  your place.
- Don't want a season anymore — wrong language, wrong cut, whatever?
  Open that show's episode list, scroll past the last episode, and
  select **Delete Season…** twice to confirm. It clears that season's
  downloads without touching the rest of the series or anything you've
  already watched. Changed your mind later? Pick that season in the
  show's season list — a season with nothing downloaded starts
  downloading when you select it.

**What the messages mean:**

| On screen | Meaning |
|---|---|
| "Movies (needs Modern TV display)" | Movies only runs in the Modern TV display mode — switch it in Settings → Display (the CRT Native screen is too small for the movie menus) |
| "Movies (configure VPN)" | The VPN step (section 7) hasn't been done yet |
| "Movies (drive not connected)" | Plug the movie drive back in |
| "Media Browser unavailable — VPN tunnel down" | The VPN dropped; the box reconnects on its own, usually within a couple of minutes |

## 11. If something's stuck

- **Not sure what's wrong?** → Content Manager → **Settings** tab →
  **Box Health** → **Run Health Check**. In about half a minute it checks
  sound, picture, games, storage and (if you use Movies) the download
  services, and says "Everything looks good" or lists what needs
  attention in plain English. If you use Movies it also says whether the
  VPN is working right now. With ProtonVPN the box checks the connection
  itself and switches servers when one fails, so "recovering" for a
  minute or two is normal. If it keeps struggling, try another VPN
  country or allow a backup country (section 7). With other VPN
  providers it shows how often the VPN dropped in the last 24 hours
  instead.
- **Asked for logs by support?** → same card → **Download Diagnostics**.
  It saves one small .zip you can email. Passwords, keys and your Wi-Fi
  details are removed before it's created.
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
  Connect a Device.

## 12. The Channel Guide (watch like TV)

Turn it on in **Settings → Display → Menu Style → Channel Guide** (the
default, **List**, keeps the familiar playlist menu).

- Every video playlist becomes a **TV channel** that plays on a schedule,
  and **channel 00** is a mix of everything. Tuning in joins whatever is
  already on, like real TV.
- **Change channels** with the yellow and green buttons, or open the
  guide: turn the knob to move through the channels and press to watch
  the one you've highlighted.
- **Settings while you watch:** a channel is always playing in this
  style, so open the guide first (press the knob), then press **B**.
- The box remembers the last channel you watched and goes back to it
  after you turn it on, finish a game or leave Movies.
- Each day at 4 AM the channels start from a new point and the mixes
  reshuffle, so the guide isn't the same every day.

**Pick Ahead.** In **Settings → Display → Guide Mode**, choose **Pick
Ahead** to look up to 24 hours ahead on a channel and start a later
programme from the beginning. While a channel plays, hold the knob for
half a second to browse what's coming up on it, then press to start the
programme you want. When it ends, the channel goes back to live TV.

## 13. Picture shape: Crop or the whole picture

Videos keep their own shape when you upload them, even phone videos
shot upright. Any video that isn't the old 4:3 TV shape has a **Crop**
setting:

- **On** (the default) fills the screen by trimming the sides.
- **Off** shows the whole picture, with borders where it doesn't fill
  the screen.

Change it on the TV while the video plays (turn the knob past the last
playlist to reach **CROP** and press; in the Channel Guide it sits above
channel 00), or tick and untick **Crop** on the
video's row in the Content Manager. The two stay in sync. A 4:3 video
always fills the screen, so it has no Crop setting.

Enjoy the box!
