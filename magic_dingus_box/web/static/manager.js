// The Content Manager front end is split into plain classic scripts that
// index.html loads in this fixed order (no bundler, no modules):
//   manager.js                core: AppState, notifications, CSRF + API
//                             helpers, upload/media config, init, shared
//                             utilities (escapeHtml/escapeJs) and modals
//   manager_devices.js        device discovery + connection
//   manager_media.js          video library + uploads
//   manager_roms.js           ROM library + uploads
//   manager_playlists.js      playlist editor, builder, drag/touch reordering
//   manager_import.js         playlist / package import
//   manager_settings.js       backup/restore, health, OTA updates, Network
//                             Doctor, Box health + diagnostics
//   manager_media_browser.js  Media Browser tab
//   vpn_settings.js           Media Browser VPN country (separate feature)
// Classic scripts share ONE global scope, so a function declared in any of
// them is callable from the others and from inline onclick="..." handlers.
// What does NOT cross files is hoisting: code that RUNS while a file loads
// (a top-level const initialiser, a top-level call) may only use names
// from that file or an earlier one. Bump the ?v= of a file you change.

// ===== UNIFIED STATE MANAGEMENT =====

/**
 * Centralized application state with event-based notifications.
 * Provides organized access to all application state with change events.
 */
const AppState = {
    // === Device State ===
    device: {
        _current: null,
        _discovered: [],

        get current() { return this._current; },
        set current(device) {
            this._current = device;
            AppState.emit('deviceChanged', device);
        },

        get discovered() { return this._discovered; },
        set discovered(devices) {
            this._discovered = devices;
            AppState.emit('devicesUpdated', devices);
        },

        get url() { return this._current?.url || null; },

        addDiscovered(device) {
            if (!this._discovered.find(d => d.url === device.url)) {
                this._discovered.push(device);
                AppState.emit('devicesUpdated', this._discovered);
            }
        }
    },

    // === Media State ===
    media: {
        _videos: [],
        _roms: {},

        get videos() { return this._videos; },
        set videos(videos) {
            this._videos = videos;
            AppState.emit('videosUpdated', videos);
        },

        get roms() { return this._roms; },
        set roms(roms) {
            this._roms = roms;
            AppState.emit('romsUpdated', roms);
        }
    },

    // === Playlist State ===
    playlists: {
        _videoItems: [],
        _gameItems: [],
        _editing: { video: null, game: null },
        _dirty: { video: false, game: false },

        getItems(type) {
            return type === 'video' ? this._videoItems : this._gameItems;
        },

        setItems(type, items) {
            if (type === 'video') {
                this._videoItems = items;
            } else {
                this._gameItems = items;
            }
            this._dirty[type] = true;
            AppState.emit('playlistChanged', { type, items });
        },

        getEditing(type) {
            return this._editing[type];
        },

        setEditing(type, filename) {
            this._editing[type] = filename;
        },

        isDirty(type) {
            return this._dirty[type];
        },

        markClean(type) {
            this._dirty[type] = false;
        }
    },

    // === UI/Drag State ===
    ui: {
        drag: {
            item: null,
            playlistIndex: null,
            playlistType: null
        },
        touch: {
            element: null,
            clone: null,
            startX: 0,
            startY: 0,
            isDragging: false,
            timer: null,
            reorderStartIndex: null,
            reorderCurrentIndex: null,
            reorderType: null
        }
    },

    // === CSRF Token ===
    _csrfToken: null,
    get csrfToken() { return this._csrfToken; },
    set csrfToken(token) { this._csrfToken = token; },

    // === Event System ===
    _listeners: {},

    on(event, callback) {
        if (!this._listeners[event]) {
            this._listeners[event] = [];
        }
        this._listeners[event].push(callback);
        return () => this.off(event, callback); // Return unsubscribe function
    },

    off(event, callback) {
        if (this._listeners[event]) {
            this._listeners[event] = this._listeners[event].filter(cb => cb !== callback);
        }
    },

    emit(event, data) {
        if (this._listeners[event]) {
            this._listeners[event].forEach(cb => {
                try {
                    cb(data);
                } catch (e) {
                    console.error(`Error in ${event} listener:`, e);
                }
            });
        }
    },

    // === State Reset ===
    reset() {
        this.device._current = null;
        this.device._discovered = [];
        this.media._videos = [];
        this.media._roms = {};
        this.playlists._videoItems = [];
        this.playlists._gameItems = [];
        this.playlists._editing = { video: null, game: null };
        this.playlists._dirty = { video: false, game: false };
        this._csrfToken = null;
        this.emit('stateReset');
    }
};

// ===== LEGACY GLOBAL STATE (for backwards compatibility) =====
// These will be progressively migrated to use AppState directly

let currentDevice = null;
let discoveredDevices = [];
let availableVideos = [];
let availableROMs = {};
let draggedItem = null;
let csrfToken = null;  // CSRF token for state-changing requests
let globalVideoUsageMap = {};  // Track which videos are in which playlists

// Sync legacy globals with AppState for backwards compatibility
AppState.on('deviceChanged', (device) => {
    currentDevice = device;
    // Re-evaluate Media Browser tab visibility for the newly-connected device.
    // The tab + bottom-nav buttons are hidden by default in index.html and
    // only revealed when /admin/media-browser/visibility returns visible:true,
    // which the kiosk's secret-sequence unlock toggles.
    if (device) {
        checkMediaBrowserVisibility().catch(e => console.warn('MB visibility check failed:', e));
    }
});
AppState.on('devicesUpdated', (devices) => { discoveredDevices = devices; });
AppState.on('videosUpdated', (videos) => { availableVideos = videos; });
AppState.on('romsUpdated', (roms) => { availableROMs = roms; });
AppState.on('playlistChanged', ({ type, items }) => {
    if (type === 'video') { videoPlaylistItems = items; }
    else { gamePlaylistItems = items; }
});

// ===== NOTIFICATION SYSTEM =====

/**
 * Show a toast notification to the user.
 * @param {string} message - The message to display
 * @param {string} type - 'success', 'error', 'warning', or 'info'
 * @param {number} duration - How long to show the notification (ms)
 */
function showNotification(message, type = 'info', duration = 4000) {
    // Create notification container if it doesn't exist
    let container = document.getElementById('notification-container');
    if (!container) {
        container = document.createElement('div');
        container.id = 'notification-container';
        container.style.cssText = `
            position: fixed;
            top: 20px;
            right: 20px;
            z-index: 10000;
            display: flex;
            flex-direction: column;
            gap: 10px;
            pointer-events: none;
        `;
        document.body.appendChild(container);
    }

    // Create notification element
    const notification = document.createElement('div');
    notification.className = `notification notification-${type}`;

    // Color scheme based on type
    const colors = {
        success: { bg: '#d4edda', border: '#28a745', text: '#155724' },
        error: { bg: '#f8d7da', border: '#dc3545', text: '#721c24' },
        warning: { bg: '#fff3cd', border: '#ffc107', text: '#856404' },
        info: { bg: '#d1ecf1', border: '#17a2b8', text: '#0c5460' }
    };
    const color = colors[type] || colors.info;

    notification.style.cssText = `
        background: ${color.bg};
        border: 1px solid ${color.border};
        border-left: 4px solid ${color.border};
        color: ${color.text};
        padding: 12px 16px;
        border-radius: 4px;
        box-shadow: 0 2px 8px rgba(0,0,0,0.15);
        font-size: 14px;
        max-width: 350px;
        pointer-events: auto;
        animation: slideIn 0.3s ease-out;
        display: flex;
        align-items: center;
        gap: 10px;
    `;

    // Add icon based on type
    const icons = {
        success: '✓',
        error: '✕',
        warning: '⚠',
        info: 'ℹ'
    };

    notification.innerHTML = `
        <span style="font-size: 18px; font-weight: bold;">${icons[type] || icons.info}</span>
        <span>${escapeHtml(message)}</span>
    `;

    // Add animation styles if not present
    if (!document.getElementById('notification-styles')) {
        const style = document.createElement('style');
        style.id = 'notification-styles';
        style.textContent = `
            @keyframes slideIn {
                from { transform: translateX(100%); opacity: 0; }
                to { transform: translateX(0); opacity: 1; }
            }
            @keyframes slideOut {
                from { transform: translateX(0); opacity: 1; }
                to { transform: translateX(100%); opacity: 0; }
            }
        `;
        document.head.appendChild(style);
    }

    container.appendChild(notification);

    // Auto-remove after duration
    setTimeout(() => {
        notification.style.animation = 'slideOut 0.3s ease-in';
        setTimeout(() => {
            if (notification.parentNode) {
                notification.parentNode.removeChild(notification);
            }
        }, 300);
    }, duration);

    // Click to dismiss
    notification.addEventListener('click', () => {
        notification.style.animation = 'slideOut 0.3s ease-in';
        setTimeout(() => {
            if (notification.parentNode) {
                notification.parentNode.removeChild(notification);
            }
        }, 300);
    });
}

// ===== CSRF TOKEN MANAGEMENT =====

async function fetchCsrfToken() {
    if (!currentDevice) return;

    try {
        const data = await apiGet(`${currentDevice.url}/admin/csrf-token`);
        csrfToken = data.token;
        console.log('CSRF token obtained');
    } catch (e) {
        console.warn('Failed to fetch CSRF token:', e.message || e);
        csrfToken = null;
    }
}

function getCsrfHeaders(includeContentType = true) {
    const headers = {};
    if (includeContentType) {
        headers['Content-Type'] = 'application/json';
    }
    if (csrfToken) {
        headers['X-CSRF-Token'] = csrfToken;
    }
    return headers;
}

// ===== API RESPONSE HANDLING =====

/**
 * Make an API request with standardized error handling.
 * Handles the new response format: { ok: true/false, data: ..., error: { code, message } }
 *
 * @param {string} url - The API endpoint URL
 * @param {object} options - Fetch options (method, headers, body, etc.)
 * @returns {Promise<any>} - The response data on success
 * @throws {Error} - Error with message and code properties on failure
 */
async function apiRequest(url, options = {}) {
    const response = await fetch(url, options);
    const result = await response.json();

    // Handle standardized response format
    if (result.ok === false) {
        const error = new Error(result.error?.message || 'Unknown error');
        error.code = result.error?.code || 'UNKNOWN_ERROR';
        error.details = result.error?.details;
        throw error;
    }

    // Return the data payload (for new format) or the whole result (for backward compatibility)
    return result.data !== undefined ? result.data : result;
}

/**
 * Make a GET API request.
 * @param {string} url - The API endpoint URL
 * @returns {Promise<any>} - The response data
 */
async function apiGet(url) {
    return apiRequest(url);
}

/**
 * Make a POST API request with JSON body.
 * @param {string} url - The API endpoint URL
 * @param {object} body - The request body
 * @returns {Promise<any>} - The response data
 */
async function apiPost(url, body) {
    return apiRequest(url, {
        method: 'POST',
        headers: getCsrfHeaders(),
        body: JSON.stringify(body)
    });
}

/**
 * Make a DELETE API request.
 * @param {string} url - The API endpoint URL
 * @returns {Promise<any>} - The response data
 */
// ===== UPLOAD CONFIGURATION =====
// Screen Wake Lock — kept while an upload is in progress so the phone
// doesn't lock its screen (which suspends the tab's JS and can stall/kill a
// multi-minute, multi-GB WiFi upload). Supported in iOS Safari 16.4+; a
// no-op where unavailable. The OS auto-releases the lock when the page is
// hidden, so we re-acquire on visibilitychange while an upload is active.
let _wakeLock = null;
let _wakeLockWanted = false;
async function acquireWakeLock() {
    _wakeLockWanted = true;
    try {
        if ('wakeLock' in navigator && !_wakeLock) {
            _wakeLock = await navigator.wakeLock.request('screen');
            _wakeLock.addEventListener('release', () => { _wakeLock = null; });
        }
    } catch (e) {
        console.warn('Wake Lock unavailable:', e);
    }
}
async function releaseWakeLock() {
    _wakeLockWanted = false;
    try { if (_wakeLock) { await _wakeLock.release(); _wakeLock = null; } }
    catch (e) { /* already gone */ }
}
document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && _wakeLockWanted) acquireWakeLock();
});

const UPLOAD_CONFIG = {
    CONCURRENCY_LIMIT: 3,      // Max parallel uploads
    MAX_RETRIES: 2,            // Number of retry attempts per file
    RETRY_DELAY_MS: 2000,      // Delay between retries
    TIMEOUT_MS: 900000,        // 15 minutes per file (supports 1GB+ over WiFi)

    // Server-side transcoding settings (Pi handles conversion via FFmpeg)
    TRANSCODE: {
        ENABLED: true,                    // Enable Pi-side transcoding
        DEFAULT_RESOLUTION: 'crt_hd',     // Default master: 4:3 @ 720p height (see TRANSCODE_RESOLUTIONS in admin_media.py)
        DEFAULT_FIT_MODE: 'crop',         // 'crop' = fill 4:3 (historical); 'fit' = whole frame + borders (see TRANSCODE_FIT_MODES)
    }
};


// ===== MEDIA TYPE CONFIGURATION =====
// Unified configuration for video and game media types
/**
 * System → libretro core. The single source of truth for the web admin.
 *
 * This existed as three byte-identical copies (the ROM upload auto-add, and
 * both "add to playlist" paths). All three had drifted out of step with the
 * cores actually installed: n64 and dreamcast were missing, so adding an N64
 * ROM from the library wrote `emulator_core: 'auto'`.
 *
 * 'auto' is not a safe fallback. playlist_loader.cpp only skips an item when
 * emulator_core is EMPTY, so 'auto' passes validation and reaches
 * app/controller.cpp:515, whose own resolver knows the same seven systems and
 * ends at "Could not resolve auto core for system: n64" — the game fails to
 * launch, after the user has already selected it.
 *
 * Keep in sync with controller.cpp:515. Note the kiosk spells these WITHOUT the
 * _libretro suffix; the suffix belongs in the playlist YAML, which is what this
 * map feeds.
 */
const ROM_CORE_MAP = {
    'nes': 'nestopia_libretro',
    'snes': 'snes9x2010_libretro',
    'genesis': 'genesis_plus_gx_libretro',
    'ps1': 'pcsx_rearmed_libretro',
    'atari7800': 'prosystem_libretro',
    'pcengine': 'mednafen_pce_fast_libretro',
    'arcade': 'fbneo_libretro',
    'n64': 'mupen64plus_next_libretro',
    'dreamcast': 'flycast_libretro'
};

/**
 * Resolve a system to its core, or null when we genuinely do not know.
 *
 * Returning null rather than 'auto' means the caller can refuse to write an
 * item the kiosk would reject at launch time, instead of writing a plausible
 * looking playlist entry that dies on selection.
 */
function coreForSystem(system) {
    return ROM_CORE_MAP[String(system || '').toLowerCase()] || null;
}

/**
 * Display title for a library entry being added to a playlist.
 *
 * The "+" button and drag-and-drop derived this differently: "+" preferred the
 * server-supplied title and otherwise stripped the extension, while drag used
 * the raw filename. So the same video added two ways showed either
 * "A DAY IN THE MALL - 1991" or "A DAY IN THE MALL - 1991 [VLX2_eOKev4].mp4" —
 * and that string is what the TV and the phone remote display.
 *
 * ROM entries carry no .title, so they fall through to filename-minus-extension,
 * which is what addROMToPlaylist already did.
 */
/**
 * Attach dragover/drop to a playlist's row body AND its surrounding container.
 *
 * Two reasons the container matters, not just the tbody:
 *
 *  - An EMPTY playlist has a zero-height tbody. There is nothing to aim at, so
 *    dragging a video into a new playlist appeared to do nothing at all. The
 *    visible affordance is .playlist-empty-state, the dashed "Drag videos here"
 *    pocket — but that is a SIBLING of <table> and is pointer-events: none (so
 *    it never swallows a drop), which means events over it reach neither the
 *    tbody nor anything that bubbles to it. The container sits underneath both
 *    and does have area.
 *  - Even with rows, dragging near the edges — above the first row or below the
 *    last — lands on the container rather than the tbody. The comment at this
 *    call site claimed that was already handled; it was not. `const tbody =
 *    container` bound the same element twice.
 *
 * Assigning to on* properties rather than addEventListener keeps this
 * idempotent, so re-running it on every render rebinds instead of stacking
 * duplicate handlers.
 */
function bindPlaylistDropTargets(container, type) {
    if (!container) return;
    const over = function(event) { handlePlaylistDragOver(event, type); };
    const drop = function(event) { handlePlaylistDrop(event, type); };

    container.ondragover = over;
    container.ondrop = drop;

    const shell = container.closest('.playlist-table-container');
    if (shell && shell !== container) {
        shell.ondragover = over;
        shell.ondrop = drop;
    }
}

function titleForLibraryItem(entry) {
    const given = (entry && entry.title || '').trim();
    if (given) return given;
    return String((entry && entry.filename) || '').replace(/\.[^.]+$/, '');
}

/**
 * Filename for a playlist title, safe as both a path segment and a URL segment.
 *
 * This was `title.toLowerCase().replace(/\s+/g, '_') + '.yaml'`, which only
 * dealt with whitespace. A title containing a URL-significant character
 * produced a filename that could not survive the round trip:
 *   "AC/DC"       -> ac/dc.yaml     -> an extra path segment; the server's own
 *                                      _sanitize_filename rejects separators
 *   "Playlist #1" -> playlist_#1.yaml -> everything from '#' is a fragment and
 *                                      never reaches the server
 *   "Why Not?"    -> why_not?.yaml  -> everything from '?' becomes a query
 * All three surfaced as a bare "Failed to save playlist" with no explanation.
 *
 * Keep the character class conservative: word characters, dot and dash only,
 * which is a subset of what the server accepts, so anything this produces is
 * guaranteed to pass _sanitize_filename.
 */
function playlistFilenameFor(title) {
    const slug = String(title || '')
        .normalize('NFD')             // split accented letters into base + mark
        .replace(/[\u0300-\u036f]/g, '')   // ...and drop the marks, so
                                            // "Ünïcodé" folds to "Unicode"
                                            // rather than being stripped to
                                            // "ncod" by the ASCII-only \w below
        .toLowerCase()
        .replace(/\s+/g, '_')
        .replace(/[^\w.-]/g, '')     // drop anything path- or URL-significant
        .replace(/_{2,}/g, '_')
        .replace(/^[._-]+|[._-]+$/g, '');
    return `${slug || 'untitled'}.yaml`;
}

const MEDIA_CONFIG = {
    video: {
        // Element IDs
        containerScope: null,  // No scope restriction for videos
        viewPrefix: 'video',
        playlistItemsId: 'playlistTableBody',  // New table body
        playlistAvailableId: 'libraryList',    // New library list
        playlistEmptyId: 'playlistEmptyState', // New empty state
        playlistCountId: 'playlistItemCount',  // "N items" under the table
        // Second editable column. index.html:224 heads it "Artist".
        secondaryField: 'artist',
        secondaryLabel: 'Artist',
        editorTitleId: 'videoPlaylistTitle',   // Title is now an input
        titleInputId: 'videoPlaylistTitle',
        curatorInputId: 'videoPlaylistCurator',
        descInputId: 'videoPlaylistDesc',
        loopCheckboxId: 'videoPlaylistLoop',
        saveBtnId: 'saveVideoPlaylist',
        searchInputId: 'librarySearch',        // New search input
        listContainerId: 'videoList',
        countBadgeId: 'videoCount',
        sourceTabPrefix: 'source',
        // Data (using AppState for centralized management)
        getItems: () => AppState.playlists.getItems('video'),
        setItems: (items) => AppState.playlists.setItems('video', items),
        getAvailable: () => AppState.media.videos,
        // API
        endpoint: '/admin/media',
        // Probe-and-transcode, ALWAYS. The raw /admin/upload endpoint must
        // never be wired here: a raw phone video (HEVC/4K) plays as a black
        // screen on the kiosk, which has no hardware decoder.
        uploadEndpoint: '/admin/smart-upload',
        // Display
        icon: spriteIcon('ic-videos'),
        sourceType: 'local',
        emptyMessage: 'No videos available',
        allAddedMessage: 'All videos have been added to the playlist',
        libraryTabText: 'All Videos',
        libraryViewId: 'videoLibraryView'
    },
    game: {
        // Element IDs
        containerScope: '#roms',  // Scope to roms tab
        viewPrefix: 'game',
        playlistItemsId: 'gamePlaylistTableBody',  // New table body
        playlistAvailableId: 'romLibraryList',     // New library list
        playlistEmptyId: 'gamePlaylistEmptyState', // New empty state
        playlistCountId: 'gamePlaylistItemCount',  // "N items" under the table
        // index.html:383 heads this column "System", and game items carry
        // emulator_system (set on add at manager.js:2548/3607/5094 and
        // preserved by cleanPlaylistItems). The shared renderer hardcoded the
        // video field, so the games editor showed an empty box placeholdered
        // "Artist" under a "System" header, and typing a system into it wrote
        // to an artist property the kiosk never reads.
        secondaryField: 'emulator_system',
        secondaryLabel: 'System',
        editorTitleId: 'gamePlaylistTitle',        // Title is now an input
        titleInputId: 'gamePlaylistTitle',
        curatorInputId: 'gamePlaylistCurator',
        descInputId: 'gamePlaylistDesc',
        loopCheckboxId: 'gamePlaylistLoop',
        saveBtnId: 'saveGamePlaylist',
        searchInputId: 'romLibrarySearch',         // New search input
        listContainerId: 'romsList',
        countBadgeId: 'romCount',
        sourceTabPrefix: 'sourceGame',
        // Data (using AppState for centralized management)
        getItems: () => AppState.playlists.getItems('game'),
        setItems: (items) => AppState.playlists.setItems('game', items),
        getAvailable: () => AppState.media.roms,
        // API
        endpoint: '/admin/roms',
        uploadEndpoint: (system) => `/admin/upload/rom/${system}`,
        // Display
        icon: spriteIcon('ic-games'),
        sourceType: 'emulated_game',
        emptyMessage: 'No ROMs available',
        allAddedMessage: 'All ROMs have been added to the playlist',
        libraryTabText: 'ROM Library',
        libraryViewId: 'romLibraryView'
    }
};

// Separate playlist builders for videos and games
let videoPlaylistItems = [];
let gamePlaylistItems = [];

// Touch drag state
let touchDragElement = null;
let touchDragClone = null;
let touchStartX = 0;
let touchStartY = 0;
let isDragging = false;
let longPressTimer = null;

// Detect device type
const isMobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent);
const hasTouch = 'ontouchstart' in window || navigator.maxTouchPoints > 0;

// ===== INITIALIZATION =====


/* ── Install coaching (Add to Home Screen) ──────────────────────────
 * Nothing in the product told a new owner that installing to the home
 * screen is the intended first step, so it was tribal knowledge.
 *
 * Two things this deliberately gets right:
 *
 * 1. THE ADDRESS. iOS pins an installed app to the origin it was
 *    installed FROM, permanently — no manifest key or redirect can move
 *    it afterwards. The kiosk's pairing QR hands out the raw DHCP IP
 *    (correct for pairing: a code lives ~2 minutes, far shorter than any
 *    lease), which means the phone is standing on the worst possible
 *    origin at exactly the moment someone would tap Add to Home Screen.
 *    An icon made there dies at the next lease change and then points at
 *    whatever device inherited the address. So: if we are on a bare IP,
 *    coach the user to switch to the .local name FIRST rather than
 *    encouraging an install that will silently rot.
 *
 * 2. THE REMOTE STILL NEEDS PAIRING. Scanning the QR authenticates
 *    Safari, and on iOS an installed app has a separate cookie jar — so
 *    the app must pair itself with the 6-digit code. Say so here, where
 *    the user is deciding to install, rather than letting them discover
 *    it as a dead end later.
 */
function maybeShowInstallHint() {
    // Fetch the box's stable .local name first, so that if we are on a
    // bare IP the toast can offer a working alternative rather than
    // telling the user to go find one. Also fetch the dynamic manifest:
    // for a PAIRED session its start_url carries this device's install
    // token, and any cross-origin steering link must carry it too —
    // cookies are per-origin, so a bare link to the .local origin would
    // demand the 6-digit code all over again even though this phone is
    // already paired. (Same-origin fetch sends the pairing cookie; an
    // unpaired session just gets a tokenless start_url.) Failure is fine
    // — the toast falls back to generic wording / a plain link.
    Promise.all([
        fetch('/api/host-info')
            .then(r => r.ok ? r.json() : null)
            .then(info => { if (info && info.mdns) window.MDB_MDNS_HOST = info.mdns; })
            .catch(() => {}),
        fetch('/manifest.webmanifest')
            .then(r => r.ok ? r.json() : null)
            .then(man => {
                const su = (man && man.start_url) || '';
                const q = su.indexOf('?device_token=');
                if (q !== -1) window.MDB_INSTALL_TOKEN_QS = su.slice(q);
            })
            .catch(() => {}),
    ]).finally(() => renderInstallHint());
}

function renderInstallHint() {
    try {
        const isStandalone =
            (typeof window.navigator.standalone !== 'undefined' && window.navigator.standalone === true) ||
            window.matchMedia('(display-mode: standalone)').matches;
        if (isStandalone) return;          // already installed — nothing to say

        const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent);
        const isAndroid = /Android/.test(navigator.userAgent);
        if (!isIOS && !isAndroid) return;  // desktop: installing adds nothing here

        if (localStorage.getItem('mdb_install_hint_shown') === '1') return;

        const host = location.hostname;
        const onBareIp = /^\d{1,3}(\.\d{1,3}){3}$/.test(host);

        const el = document.createElement('div');
        el.className = 'install-toast';

        // Set when the manifest fetch found a paired session's install
        // token (maybeShowInstallHint). Rides ONLY in the link href —
        // the visible text stays the clean address — and the destination
        // origin's redeem 303 strips it from the URL on arrival.
        const tokenQs = window.MDB_INSTALL_TOKEN_QS || '';

        if (onBareIp) {
            // Don't encourage an install that will break. Offer the stable
            // address instead — same page, resolvable name. For a paired
            // phone the link carries the install token so the .local origin
            // pairs itself on arrival (cookies are per-origin; a bare link
            // would demand the 6-digit code all over again).
            const stable = window.MDB_MDNS_HOST || '';
            el.innerHTML =
                '<strong>Before installing</strong><br>' +
                'This address (' + host + ') can change when your router ' +
                'reassigns it, which would break a saved icon.' +
                (stable
                    ? ' Open <a href="http://' + stable + ':5000/' + tokenQs + '">' + stable + ':5000</a> and install from there.'
                    : ' Use the box&rsquo;s <em>.local</em> name shown on the kiosk instead.');
        } else {
            el.innerHTML =
                '<strong>Add to Home Screen</strong> to use this as an app.<br>' +
                'Tap ' + (isIOS ? 'the Share button' : 'the menu') + ' &rarr; Add to Home Screen.' +
                (tokenQs
                    ? '<br><span class="install-toast__sub">Your remote pairing comes along automatically.</span>'
                    : '<br><span class="install-toast__sub">The remote pairs separately &mdash; ' +
                      'open it in the app and enter the code from the kiosk.</span>');
        }

        // Clicking anywhere on the toast dismisses it for good, except on
        // the address link (which should navigate, not just dismiss).
        el.addEventListener('click', (ev) => {
            if (ev.target && ev.target.tagName === 'A') return;
            el.remove();
            localStorage.setItem('mdb_install_hint_shown', '1');
        });
        document.body.appendChild(el);
    } catch (e) {
        // Coaching is a nicety; never let it break the Content Manager.
        console.warn('install hint skipped:', e);
    }
}

document.addEventListener('DOMContentLoaded', () => {
    // 1. Start discovery IMMEDIATELY - this is the most important thing
    discoverDevices();

    // 2. Initialize UI components safely
    try {
        initializeEventListeners();
    } catch (e) {
        console.error('UI Initialization failed:', e);
    }

    // 3. Initialize other UI elements
    try {
        initializeCollapsibleSections();
    } catch (e) {
        console.error('Collapsible sections failed:', e);
    }

    // 4. Coach first-time phone visitors to install. Last, and wrapped,
    //    so it can never delay or break the actual manager UI.
    try {
        maybeShowInstallHint();
    } catch (e) {
        console.warn('install hint failed:', e);
    }

    // Auto-refresh device list every 30 seconds
    setInterval(discoverDevices, 30000);
});

// ===== COLLAPSIBLE SECTIONS =====

function toggleSection(sectionId) {
    const section = document.getElementById(sectionId);
    const icon = document.getElementById(sectionId + 'Icon');

    if (!section) return;

    const isCollapsed = section.classList.contains('collapsed');

    if (isCollapsed) {
        // Expand
        section.classList.remove('collapsed');
        if (icon) icon.classList.remove('collapsed');
    } else {
        // Collapse
        section.classList.add('collapsed');
        if (icon) icon.classList.add('collapsed');
    }
}

function initializeCollapsibleSections() {
    // Start with device selector open, others can be configured
    // All sections open by default
}

function initializeEventListeners() {
    // Tab switching - shared function for top and bottom nav
    function switchTab(tabId) {
        // Update all tab buttons (both top and bottom nav)
        document.querySelectorAll('.tab, .bottom-nav-item').forEach(t => t.classList.remove('active'));
        document.querySelectorAll(`.tab[data-tab="${tabId}"], .bottom-nav-item[data-tab="${tabId}"]`).forEach(t => t.classList.add('active'));

        // Update tab content
        document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
        const tabContent = document.getElementById(tabId);
        if (tabContent) tabContent.classList.add('active');

        // Explicitly initialize the view when switching tabs
        if (tabId === 'roms') {
            switchGameView('playlists');
        } else if (tabId === 'videos') {
            switchVideoView('playlists');
        } else if (tabId === 'settings') {
            // Update mobile settings view with current device info
            updateMobileSettingsView();
        } else if (tabId === 'mediabrowser') {
            refreshMediaBrowserStatus();
        } else if (tabId === 'remote') {
            // Phone-sized touch device → take over the whole window with the
            // standalone remote page. Desktop → render inside the iframe in
            // the existing tab body (already set up in HTML).
            const isPhoneViewport = window.matchMedia('(max-width: 700px) and (pointer: coarse)').matches;
            if (isPhoneViewport) {
                window.location.assign('/admin/remote');
                return;  // browser navigates away; don't continue
            }
            // Desktop case: ensure iframe src is current (in case the user
            // paired/unpaired and we want to re-load).
            const f = document.getElementById('remoteFrame');
            if (f && !f.src.endsWith('/admin/remote')) {
                f.src = '/admin/remote';
            }
        }
    }

    // Top tab navigation
    document.querySelectorAll('.tab').forEach(tab => {
        tab.addEventListener('click', () => {
            const tabId = tab.dataset.tab;
            switchTab(tabId);
        });
    });

    // Bottom navigation for mobile
    document.querySelectorAll('.bottom-nav-item').forEach(navItem => {
        navItem.addEventListener('click', () => {
            const tabId = navItem.dataset.tab;
            switchTab(tabId);
        });
    });

    // Manual connection
    document.getElementById('manualConnect').addEventListener('click', manualConnect);

    // File uploads - video upload is handled by inline onchange="handleDirectUpload(this)"
    // (which routes through server-side transcoding)

    // NOTE: no 'change' listener for #romUpload here. index.html:432 already
    // carries onchange="handleDirectROMUpload(this)", and this listener called
    // uploadROMs() on the same event — so every ROM picked was uploaded TWICE,
    // once per handler. On a phone over WiFi that doubled the time and data for
    // a multi-hundred-MB PS1 image, and the second run wiped the first's
    // progress bars. The inline handler is the one to keep: it matches the
    // video path (handleDirectUpload) and passes autoAddToPlaylist=true, which
    // this listener's bare uploadROMs() dropped.

    // Video playlist save/cancel
    document.getElementById('saveVideoPlaylist').addEventListener('click', () => savePlaylist('video'));
    document.getElementById('cancelVideoPlaylistEdit').addEventListener('click', () => cancelEdit('video'));

    // Game playlist save/cancel
    document.getElementById('saveGamePlaylist').addEventListener('click', () => savePlaylist('game'));
    document.getElementById('cancelGamePlaylistEdit').addEventListener('click', () => cancelEdit('game'));

    // Drag and drop for file inputs (desktop only)
    if (!isMobile) {
        setupDragAndDrop('videoUpload');
        setupDragAndDrop('romUpload');
    }

    // Mobile manual connect button
    const mobileManualConnect = document.getElementById('mobileManualConnect');
    if (mobileManualConnect) {
        mobileManualConnect.addEventListener('click', manualConnect);
    }
}

// ===== MOBILE SETTINGS VIEW =====

/**
 * Update the mobile Settings tab with current device status and health info
 */
function updateMobileSettingsView() {
    const mobileStatusText = document.getElementById('mobileStatusText');
    const mobileDeviceStatus = document.getElementById('mobileDeviceStatus');
    const mobileDeviceList = document.getElementById('mobileDeviceList');
    const mobileHealthInfo = document.getElementById('mobileHealthInfo');

    if (!mobileStatusText || !mobileDeviceStatus) return;

    if (currentDevice) {
        mobileStatusText.textContent = `Connected to ${currentDevice.device_name}`;
        mobileDeviceStatus.classList.add('connected');

        // Show device info in device list
        if (mobileDeviceList) {
            mobileDeviceList.innerHTML = `
                <div class="device-card selected">
                    <div class="device-info">
                        <h4>${spriteIcon('ic-crt')} ${escapeHtml(currentDevice.device_name)}</h4>
                        <div class="meta">${escapeHtml(currentDevice.local_ip)} • ${escapeHtml(currentDevice.hostname)}</div>
                    </div>
                    <div class="device-stats">
                        <div>${plural(currentDevice.stats?.playlists, 'playlist')}</div>
                        <div>${plural(currentDevice.stats?.videos, 'video')}</div>
                        <div>${plural(currentDevice.stats?.roms, 'ROM')}</div>
                    </div>
                </div>
            `;
        }

        if (mobileHealthInfo) {
            refreshHealthInfo();
        }
        loadBoxHealth();
    } else {
        mobileStatusText.textContent = 'Not connected';
        mobileDeviceStatus.classList.remove('connected');

        if (mobileDeviceList) {
            mobileDeviceList.innerHTML = `
                <p style="color: var(--text-secondary); padding: 0.5rem 0;">
                    No device connected. Use the button below to connect.
                </p>
            `;
        }
    }
}

/**
 * Markup for one icon from the inline sprite in index.html.
 *
 * Use this anywhere JS writes an icon into the DOM. The sprite is stroked in
 * currentColor so each instance picks up its context colour, and `.ic` sizes it
 * to 1.15em — neither of which an emoji glyph can do.
 *
 * @param {string} id sprite symbol id, e.g. 'ic-videos'
 * @returns {string} HTML
 */
function spriteIcon(id) {
    return `<svg class="ic" aria-hidden="true"><use href="#${id}"/></svg>`;
}

/**
 * "1 item" / "2 items" — counts were interpolated as `${n} items` throughout,
 * so every single-item case read "1 items".
 *
 * @param {number} n
 * @param {string} noun singular form
 * @returns {string}
 */
function plural(n, noun) {
    const c = Number(n) || 0;
    return `${c} ${noun}${c === 1 ? '' : 's'}`;
}

/**
 * Byline + item count for a playlist card.
 *
 * Curator is optional and most imported playlists have none, which rendered a
 * bare "By  • 5 items" — the word "By" attributed to nobody, with the stray
 * separator still there. Drop the whole byline when there is no curator rather
 * than leaving the label stranded.
 *
 * @param {{curator?: string, item_count: number}} pl
 * @returns {string} HTML-safe meta line
 */
function playlistMetaLine(pl) {
    const curator = (pl.curator || '').trim();
    const byline = curator ? `By ${escapeHtml(curator)} &bull; ` : '';
    return `${byline}${plural(pl.item_count, 'item')}`;
}

// ===== UTILITY FUNCTIONS =====

function createProgressBar(filename) {
    const container = document.createElement('div');
    container.className = 'progress-bar';

    const fill = document.createElement('div');
    fill.className = 'progress-fill queued';
    fill.style.width = '100%';
    fill.textContent = `${filename} - Queued`;

    container.appendChild(fill);

    return {
        element: container,
        filename: filename,
        setQueued: () => {
            fill.className = 'progress-fill queued';
            fill.style.width = '100%';
            fill.textContent = `${filename} - Queued`;
        },
        update: (percent) => {
            fill.className = 'progress-fill';
            fill.style.width = `${percent}%`;
            fill.textContent = `${filename} - ${percent}%`;
        },
        setRetrying: (attempt, maxRetries) => {
            fill.className = 'progress-fill retrying';
            fill.style.width = '100%';
            fill.textContent = `${filename} - Retrying (${attempt}/${maxRetries})...`;
        },
        complete: () => {
            fill.style.width = '100%';
            fill.textContent = `${filename} - Complete! ✓`;
            fill.className = 'progress-fill complete';
        },
        error: (message = 'Upload failed') => {
            fill.style.width = '100%';
            fill.textContent = `${filename} - ${message} ✗`;
            fill.className = 'progress-fill error';
        }
    };
}

function formatFileSize(bytes) {
    const sizes = ['B', 'KB', 'MB', 'GB'];
    if (bytes === 0) return '0 B';
    const i = Math.floor(Math.log(bytes) / Math.log(1024));
    return Math.round(bytes / Math.pow(1024, i) * 100) / 100 + ' ' + sizes[i];
}

/**
 * Normalize video/media paths for comparison.
 * Handles various path formats from different sources:
 * - ../media/file.mp4 (playlist relative paths)
 * - data/media/file.mp4 (API response)
 * - /data/media/file.mp4 (absolute)
 * - media/file.mp4 (simple)
 * - magic_dingus_box_cpp/data/media/file.mp4 (full relative)
 *
 * Returns just the filename for reliable comparison.
 */
function normalizeVideoPath(p) {
    if (!p) return '';
    if (typeof p !== 'string') return '';
    let clean = p;
    // Remove leading slash
    clean = clean.replace(/^\//, '');
    // Remove ../ prefixes (used in playlist relative paths)
    clean = clean.replace(/^(\.\.\/)+/, '');
    // Remove magic_dingus_box_cpp/ prefix
    clean = clean.replace(/^magic_dingus_box_cpp\//, '');
    // Remove data/ or dev_data/ prefix
    clean = clean.replace(/^(dev_)?data\//, '');
    // Remove media/ prefix
    clean = clean.replace(/^media\//, '');
    // At this point we should have just: filename.mp4
    return clean;
}

// Helper to safely escape strings for HTML attributes
function escapeHtml(str) {
    if (str === null || str === undefined) return '';
    // Force to string if it's not
    if (typeof str !== 'string') str = String(str);

    return str
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#039;");
}

// Helper to escape strings for JavaScript string literals in onclick handlers
function escapeJs(str) {
    if (str === null || str === undefined) return '';
    if (typeof str !== 'string') str = String(str);
    // Callers embed the result in onclick="fn('...')": the HTML parser sees
    // it BEFORE the JS engine, so a raw " ends the attribute and &#39;
    // decodes back to '. Emit only \xNN / \uNNNN for anything either
    // parser acts on — inert as HTML, decoded exactly by JS.
    return str
        .replace(/\\/g, '\\\\')
        .replace(/['"&<>\n\r]/g, c => '\\x' + c.charCodeAt(0).toString(16).padStart(2, '0'))
        .replace(/[\u2028\u2029]/g, c => '\\u' + c.charCodeAt(0).toString(16));
}

// ===== MODAL SUPPORT =====

function showConfirmModal(title, message, onConfirm) {
    const modal = document.getElementById('confirmModal');
    const titleEl = document.getElementById('modalTitle');
    const messageEl = document.getElementById('modalMessage');
    const confirmBtn = document.getElementById('modalConfirm');
    const cancelBtn = document.getElementById('modalCancel');

    if (!modal) {
        // Fallback if modal not found
        if (confirm(message)) onConfirm();
        return;
    }

    titleEl.textContent = title;
    messageEl.textContent = message;

    const closeModal = () => {
        modal.classList.remove('active');
        confirmBtn.onclick = null;
        cancelBtn.onclick = null;
    };

    confirmBtn.onclick = (e) => {
        if (e) e.preventDefault();
        console.log('Confirm button clicked');
        onConfirm();
        closeModal();
    };

    cancelBtn.onclick = (e) => {
        if (e) e.preventDefault();
        closeModal();
    };

    // Close on click outside
    modal.onclick = (e) => {
        if (e.target === modal) closeModal();
    };

    modal.classList.add('active');
}

/**
 * Show a confirmation modal with HTML content.
 * Unlike showConfirmModal, this sets innerHTML instead of textContent.
 */
function showConfirmModalHtml(title, htmlContent, onConfirm) {
    const modal = document.getElementById('confirmModal');
    const titleEl = document.getElementById('modalTitle');
    const messageEl = document.getElementById('modalMessage');
    const confirmBtn = document.getElementById('modalConfirm');
    const cancelBtn = document.getElementById('modalCancel');

    if (!modal) {
        // Fallback if modal not found
        if (confirm(htmlContent.replace(/<[^>]*>/g, ''))) onConfirm();
        return;
    }

    titleEl.textContent = title;
    messageEl.innerHTML = htmlContent;

    const closeModal = () => {
        modal.classList.remove('active');
        confirmBtn.onclick = null;
        cancelBtn.onclick = null;
    };

    confirmBtn.onclick = (e) => {
        if (e) e.preventDefault();
        onConfirm();
        closeModal();
    };

    cancelBtn.onclick = (e) => {
        if (e) e.preventDefault();
        closeModal();
    };

    // Close on click outside
    modal.onclick = (e) => {
        if (e.target === modal) closeModal();
    };

    modal.classList.add('active');
}
