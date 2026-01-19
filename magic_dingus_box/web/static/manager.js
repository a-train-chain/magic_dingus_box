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
AppState.on('deviceChanged', (device) => { currentDevice = device; });
AppState.on('devicesUpdated', (devices) => { discoveredDevices = devices; });
AppState.on('videosUpdated', (videos) => { availableVideos = videos; });
AppState.on('romsUpdated', (roms) => { availableROMs = roms; });
AppState.on('playlistChanged', ({ type, items }) => {
    if (type === 'video') { videoPlaylistItems = items; }
    else { gamePlaylistItems = items; }
});

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
async function apiDelete(url) {
    return apiRequest(url, {
        method: 'DELETE',
        headers: getCsrfHeaders()
    });
}

// ===== UPLOAD CONFIGURATION =====
const UPLOAD_CONFIG = {
    CONCURRENCY_LIMIT: 3,      // Max parallel uploads
    MAX_RETRIES: 2,            // Number of retry attempts per file
    RETRY_DELAY_MS: 2000,      // Delay between retries
    TIMEOUT_MS: 900000,        // 15 minutes per file (supports 1GB+ over WiFi)

    // Server-side transcoding settings (Pi handles conversion via FFmpeg)
    TRANSCODE: {
        ENABLED: true,                    // Enable Pi-side transcoding
        DEFAULT_RESOLUTION: 'crt',        // Default target resolution
    }
};


// ===== MEDIA TYPE CONFIGURATION =====
// Unified configuration for video and game media types
const MEDIA_CONFIG = {
    video: {
        // Element IDs
        containerScope: null,  // No scope restriction for videos
        viewPrefix: 'video',
        playlistItemsId: 'playlistTableBody',  // New table body
        playlistAvailableId: 'libraryList',    // New library list
        playlistEmptyId: 'playlistEmptyState', // New empty state
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
        uploadEndpoint: '/admin/upload',
        // Display
        icon: '📹',
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
        icon: '🎮',
        sourceType: 'emulated_game',
        emptyMessage: 'No ROMs available',
        allAddedMessage: 'All ROMs have been added to the playlist',
        libraryTabText: 'ROM Library',
        libraryViewId: 'romLibraryView'
    }
};

// Emulator core mapping for ROMs
const CORE_MAP = {
    nes: 'fceumm',
    snes: 'snes9x',
    gb: 'gambatte',
    gbc: 'gambatte',
    gba: 'mgba',
    genesis: 'genesis_plus_gx',
    n64: 'mupen64plus_next',
    psx: 'pcsx_rearmed'
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

    document.getElementById('romUpload').addEventListener('change', (e) => {
        if (e.target.files.length > 0) uploadROMs();
    });

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
                        <h4>📺 ${escapeHtml(currentDevice.device_name)}</h4>
                        <div class="meta">${escapeHtml(currentDevice.local_ip)} • ${escapeHtml(currentDevice.hostname)}</div>
                    </div>
                    <div class="device-stats">
                        <div>${currentDevice.stats?.playlists || 0} playlists</div>
                        <div>${currentDevice.stats?.videos || 0} videos</div>
                        <div>${currentDevice.stats?.roms || 0} ROMs</div>
                    </div>
                </div>
            `;
        }

        // Copy health info from main section if available
        const mainHealthInfo = document.getElementById('healthInfo');
        if (mobileHealthInfo && mainHealthInfo) {
            mobileHealthInfo.innerHTML = mainHealthInfo.innerHTML;
        } else if (mobileHealthInfo) {
            refreshHealthInfo();
        }
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

// ===== DEVICE DISCOVERY =====

async function discoverDevices() {
    const statusText = document.getElementById('statusText');
    statusText.textContent = 'Searching for devices...';

    discoveredDevices = [];
    AppState.device._discovered = [];  // Keep AppState in sync

    // 1. Always check current origin first (relative path)
    try {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 2000); // 2s timeout

        const response = await fetch('/admin/device/info', { signal: controller.signal });
        clearTimeout(timeout);

        if (response.ok) {
            const result = await response.json();
            const info = result.data || result;  // Handle both wrapped and unwrapped responses
            info.url = window.location.origin;
            discoveredDevices.push(info);
            AppState.device._discovered.push(info);  // Keep AppState in sync

            // Found it! Connect immediately and STOP scanning
            selectDevice(0);
            return;
        } else {
            throw new Error(`HTTP ${response.status}`);
        }
    } catch (e) {
        console.log('Current origin check failed:', e);
        statusText.textContent = `Connection failed: ${e.message}. Retrying...`;
        statusText.style.color = 'var(--accent-color)';
    }

    // 2. Try specific local addresses
    const hostname = window.location.hostname;
    if (hostname !== 'localhost' && hostname !== '127.0.0.1') {
        await checkDevice('localhost', 5000);
    }

    // 3. Network scan (fallback only)
    if (discoveredDevices.length === 0 && hostname.match(/^\d+\.\d+\.\d+\.\d+$/)) {
        statusText.textContent = 'Scanning network...';
        const subnet = hostname.split('.').slice(0, 3).join('.');

        // Process in chunks to avoid overwhelming browser
        const chunk = 20;
        for (let i = 1; i <= 254; i += chunk) {
            const promises = [];
            for (let j = i; j < i + chunk && j <= 254; j++) {
                promises.push(checkDevice(`${subnet}.${j}`, 5000));
            }
            await Promise.allSettled(promises);

            // Update UI progressively
            if (discoveredDevices.length > 0) {
                displayDevices();
            }
        }
    }

    displayDevices();
}

async function checkDevice(host, port) {
    try {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 1000);

        const url = `http://${host}:${port}/admin/device/info`;
        const response = await fetch(url, { signal: controller.signal });

        clearTimeout(timeout);

        if (response.ok) {
            const result = await response.json();
            const info = result.data || result;  // Handle both wrapped and unwrapped responses
            info.url = `http://${host}:${port}`;

            // Avoid duplicates
            if (!discoveredDevices.find(d => d.device_id === info.device_id)) {
                discoveredDevices.push(info);
                AppState.device._discovered.push(info);  // Keep AppState in sync
            }
        }
    } catch (e) {
        // Device not found or timeout - ignore
    }
}

function displayDevices() {
    const deviceList = document.getElementById('deviceList');
    const statusText = document.getElementById('statusText');

    if (discoveredDevices.length === 0) {
        statusText.textContent = 'No devices found';
        deviceList.innerHTML = `
            <p style="color: var(--text-secondary); padding: 1rem;">
                No Magic Dingus Boxes found on your network. 
                Make sure your device is powered on and connected to the same WiFi.
            </p>
        `;
        return;
    }

    statusText.textContent = `Found ${discoveredDevices.length} device(s)`;

    deviceList.innerHTML = discoveredDevices.map((device, index) => `
        <div class="device-card ${currentDevice && currentDevice.device_id === device.device_id ? 'selected' : ''}" 
             onclick="selectDevice(${index})">
            <div class="device-info">
                <h4>📺 ${escapeHtml(device.device_name)}</h4>
                <div class="meta">
                    ${escapeHtml(device.local_ip)} • ${escapeHtml(device.hostname)}
                </div>
            </div>
            <div class="device-stats">
                <div>${device.stats?.playlists || 0} playlists</div>
                <div>${device.stats?.videos || 0} videos</div>
                <div>${device.stats?.roms || 0} ROMs</div>
            </div>
        </div>
    `).join('');
}

function selectDevice(index) {
    const newDevice = AppState.device.discovered[index];
    const isSameDevice = AppState.device.current && AppState.device.current.device_id === newDevice.device_id;

    // Use AppState to set current device (triggers deviceChanged event)
    AppState.device.current = newDevice;

    document.getElementById('deviceStatus').classList.add('connected');
    document.getElementById('statusText').textContent =
        `Connected to ${AppState.device.current.device_name}`;

    // Update connection status in header (old element, keep for compatibility)
    const statusElement = document.getElementById('deviceConnectionStatus');
    if (statusElement) {
        statusElement.textContent = AppState.device.current.device_name;
        statusElement.classList.add('connected');
    }

    // Update the new connection badge
    updateConnectionBadge();

    displayDevices(); // Refresh to show selected state

    // Only load content if it's a new connection
    if (!isSameDevice) {
        loadAllContentFromDevice();

        // Auto-collapse device selector after connection
        setTimeout(() => {
            toggleSection('deviceSelector');
        }, 1000);
    }
}

/**
 * Update the connection badge in the header with device name and connection type
 */
function updateConnectionBadge() {
    const badge = document.getElementById('connectionBadge');
    const badgeText = document.getElementById('connectionBadgeText');

    if (!badge || !badgeText) return;

    if (currentDevice) {
        // Determine connection type based on IP address
        const deviceUrl = currentDevice.url || '';
        const isUSB = deviceUrl.includes('192.168.7.1');
        const connectionType = isUSB ? 'USB' : 'WiFi';
        const icon = isUSB ? '🔌' : '📶';

        // Update badge classes
        badge.classList.remove('disconnected');
        badge.classList.add('connected');
        badge.classList.remove('wifi', 'usb');
        badge.classList.add(isUSB ? 'usb' : 'wifi');

        // Update badge content
        badge.querySelector('.badge-icon').textContent = icon;
        badgeText.textContent = `${currentDevice.device_name} • ${connectionType}`;
    } else {
        // Not connected
        badge.classList.add('disconnected');
        badge.classList.remove('connected', 'wifi', 'usb');
        badge.querySelector('.badge-icon').textContent = '⚡';
        badgeText.textContent = 'Not Connected';
    }
}

async function manualConnect() {
    const input = prompt('Enter device IP or hostname (e.g., 192.168.1.100 or magicpi.local):');
    if (!input) return;

    // Validate input format
    const trimmed = input.trim();

    // Block dangerous patterns (XSS/injection attempts)
    if (trimmed.includes('<') || trimmed.includes('>') ||
        trimmed.toLowerCase().includes('javascript:') ||
        trimmed.toLowerCase().includes('data:') ||
        trimmed.includes('\\')) {
        alert('Invalid input format');
        return;
    }

    // Extract host and optional port
    // Remove http:// or https:// prefix if provided
    let hostPart = trimmed.replace(/^https?:\/\//i, '');

    // Split host and port
    const portMatch = hostPart.match(/:(\d+)$/);
    let port = 5000; // Default port
    if (portMatch) {
        port = parseInt(portMatch[1], 10);
        hostPart = hostPart.replace(/:(\d+)$/, '');
        if (port < 1 || port > 65535) {
            alert('Invalid port number (must be 1-65535)');
            return;
        }
    }

    // Validate host as IP address or hostname
    const ipPattern = /^(\d{1,3}\.){3}\d{1,3}$/;
    const hostnamePattern = /^[a-zA-Z0-9]([a-zA-Z0-9-]*[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9-]*[a-zA-Z0-9])?)*$/;

    if (!ipPattern.test(hostPart) && !hostnamePattern.test(hostPart)) {
        alert('Please enter a valid IP address (e.g., 192.168.1.100) or hostname (e.g., magicpi.local)');
        return;
    }

    // Additional IP validation - check octet ranges
    if (ipPattern.test(hostPart)) {
        const octets = hostPart.split('.').map(Number);
        if (octets.some(o => o > 255)) {
            alert('Invalid IP address (octets must be 0-255)');
            return;
        }
    }

    try {
        const url = `http://${hostPart}:${port}`;
        const response = await fetch(`${url}/admin/device/info`);

        if (response.ok) {
            const result = await response.json();
            const info = result.data || result;  // Handle both wrapped and unwrapped responses
            info.url = url;

            if (!discoveredDevices.find(d => d.device_id === info.device_id)) {
                discoveredDevices.push(info);
                AppState.device._discovered.push(info);  // Keep AppState in sync
            }

            selectDevice(discoveredDevices.length - 1);
            displayDevices();
        } else {
            alert('Could not connect to device');
        }
    } catch (e) {
        alert('Connection failed: ' + e.message);
    }
}

async function loadAllContentFromDevice() {
    if (!currentDevice) return;

    // Fetch CSRF token before making any state-changing requests
    await fetchCsrfToken();

    await loadVideos();
    await loadROMs();
    await loadExistingPlaylists();
    renderVideoPlaylistAvailable();
    renderGamePlaylistAvailable();
    renderLibraryPanel();  // Render new library panel

    // Load health info (non-blocking)
    refreshHealthInfo();

    // Load version info (non-blocking)
    loadVersionInfo();
}

// ===== UNIFIED MEDIA FUNCTIONS =====
// These functions handle both video and game media types using MEDIA_CONFIG

/**
 * Switch between views (playlists, editor, library) for a media type
 * @param {string} type - 'video' or 'game'
 * @param {string} viewName - 'playlists', 'editor', or 'library'
 */
function switchContentView(type, viewName) {
    const config = MEDIA_CONFIG[type];
    const scope = config.containerScope;

    // Update sub-tabs
    const tabSelector = scope ? `${scope} .sub-tab` : '.sub-tab';
    document.querySelectorAll(tabSelector).forEach(tab => {
        // Only process tabs in the correct scope
        if (scope && !tab.closest(scope)) return;

        tab.classList.remove('active');
        const tabText = tab.textContent.toLowerCase();

        // Handle specific mapping
        if (viewName === 'playlists' && tab.textContent.includes('My Playlists')) {
            tab.classList.add('active');
        } else if (viewName === 'editor' && tab.textContent.includes('Editor')) {
            tab.classList.add('active');
        } else if (viewName === 'library' && tab.textContent.includes(config.libraryTabText)) {
            tab.classList.add('active');
        } else if (tabText.includes(viewName.replace(type, ''))) {
            tab.classList.add('active');
        }
    });

    // Toggle body class for mobile styling hooks (e.g. hiding bottom nav in editor)
    if (viewName === 'editor') {
        document.body.classList.add('editor-active');
    } else {
        document.body.classList.remove('editor-active');
    }

    // Update views
    const viewSelector = scope ? `${scope} .sub-view` : '.sub-view';
    document.querySelectorAll(viewSelector).forEach(view => {
        view.style.display = 'none';
        view.classList.remove('active');
    });

    // Determine view ID
    let viewId = `${config.viewPrefix}${viewName.charAt(0).toUpperCase() + viewName.slice(1)}View`;

    // Handle library special case for games
    if (type === 'game' && viewName === 'library') {
        viewId = config.libraryViewId;
    }

    const view = document.getElementById(viewId);
    if (view) {
        view.style.display = 'block';
        view.classList.add('active');
    }
}

/**
 * Create a new playlist for a media type
 * @param {string} type - 'video' or 'game'
 */
function createNewPlaylist(type) {
    const config = MEDIA_CONFIG[type];

    // Clear form
    document.getElementById(config.titleInputId).value = '';
    document.getElementById(config.curatorInputId).value = '';
    document.getElementById(config.descInputId).value = '';
    document.getElementById(config.loopCheckboxId).checked = false;
    document.getElementById(config.editorTitleId).textContent = 'New Playlist';

    // Clear items
    config.setItems([]);
    renderPlaylistItems(type);

    // Remove editing file reference
    const saveBtn = document.getElementById(config.saveBtnId);
    if (saveBtn) delete saveBtn.dataset.editingFile;

    // Switch to editor
    if (type === 'video') {
        renderVideoPlaylistAvailable();
    } else if (type === 'game') {
        renderGamePlaylistAvailable();
    }
    switchContentView(type, 'editor');
}

/**
 * Filter available items in the library
 * @param {string} type - 'video' or 'game'
 */
function filterLibrary(type) {
    const config = MEDIA_CONFIG[type];
    const query = document.getElementById(config.searchInputId).value.toLowerCase();
    const items = document.querySelectorAll(`#${config.playlistAvailableId} .draggable-item, #${config.playlistAvailableId} .content-item`);

    items.forEach(item => {
        const text = item.textContent.toLowerCase();
        item.style.display = text.includes(query) ? 'flex' : 'none';
    });
}

/**
 * Render playlist items in the editor
 * @param {string} type - 'video' or 'game'
 */
function renderPlaylistItems(type) {
    const config = MEDIA_CONFIG[type];
    const container = document.getElementById(config.playlistItemsId);
    const emptyState = document.getElementById(config.playlistEmptyId);
    const countEl = document.getElementById('playlistItemCount');
    const items = config.getItems();

    // Update count
    const actualItems = items.filter(item => !item.isEmpty);
    if (countEl) countEl.textContent = `${actualItems.length} item${actualItems.length !== 1 ? 's' : ''}`;

    if (items.length === 0) {
        container.innerHTML = '';
        if (emptyState) emptyState.style.display = 'block';
        return;
    }

    if (emptyState) emptyState.style.display = 'none';

    // Render as table rows
    container.innerHTML = items.map((item, index) => {
        const isEmptySlot = item.isEmpty || false;
        const rowClass = isEmptySlot ? 'empty-slot' : '';

        if (isEmptySlot) {
            return `
                <tr class="${rowClass}" draggable="true" data-index="${index}" data-playlist-type="${type}"
                    ondragstart="handlePlaylistDragStart(event, '${type}')"
                    ondragover="handlePlaylistDragOver(event)"
                    ondrop="handlePlaylistDrop(event, '${type}')"
                    ondragend="handleDragEnd(event)">
                    <td class="col-num">${index + 1}</td>
                    <td class="col-title"><span class="empty-label">(empty slot)</span></td>
                    <td class="col-artist"></td>
                    <td class="col-actions">
                        <div class="row-actions">
                            <button onclick="movePlaylistItem(${index}, -1, '${type}')" ${index === 0 ? 'disabled' : ''}>▲</button>
                            <button onclick="movePlaylistItem(${index}, 1, '${type}')" ${index === items.length - 1 ? 'disabled' : ''}>▼</button>
                            <button class="btn-delete" onclick="removePlaylistItem(${index}, '${type}')">✕</button>
                        </div>
                    </td>
                </tr>
            `;
        }

        const title = item.title || item.path?.split('/').pop()?.replace(/\.[^.]+$/, '') || 'Untitled';
        const artist = item.artist || '';

        return `
            <tr class="${rowClass}" draggable="true" data-index="${index}" data-playlist-type="${type}"
                ondragstart="handlePlaylistDragStart(event, '${type}')"
                ondragover="handlePlaylistDragOver(event)"
                ondrop="handlePlaylistDrop(event, '${type}')"
                ondragend="handleDragEnd(event)">
                <td class="col-num">${index + 1}</td>
                <td class="col-title">
                    <input type="text" class="inline-input" value="${escapeHtml(title)}" 
                           placeholder="Title" onchange="updatePlaylistItem(${index}, 'title', this.value, '${type}')">
                </td>
                <td class="col-artist">
                    <input type="text" class="inline-input" value="${escapeHtml(artist)}" 
                           placeholder="Artist" onchange="updatePlaylistItem(${index}, 'artist', this.value, '${type}')">
                </td>
                <td class="col-actions">
                    <div class="row-actions">
                        <button onclick="movePlaylistItem(${index}, -1, '${type}')" ${index === 0 ? 'disabled' : ''}>▲</button>
                        <button onclick="movePlaylistItem(${index}, 1, '${type}')" ${index === items.length - 1 ? 'disabled' : ''}>▼</button>
                        <button class="btn-delete" onclick="removePlaylistItem(${index}, '${type}')">✕</button>
                    </div>
                </td>
            </tr>
        `;
    }).join('');
}

/**
 * Render available items that can be added to a playlist
 * @param {string} type - 'video' or 'game'
 */
function renderPlaylistAvailable(type) {
    const config = MEDIA_CONFIG[type];
    const container = document.getElementById(config.playlistAvailableId);
    const items = config.getItems();
    const addedPaths = new Set(items.map(item => item.path));

    if (type === 'video') {
        // Video rendering
        const available = config.getAvailable();
        if (available.length === 0) {
            container.innerHTML = `<p style="color: var(--text-secondary); padding: 1rem;">${config.emptyMessage}</p>`;
            return;
        }

        const filteredVideos = available.filter(video => {
            const videoPath = video.path || `media/${video.filename}`;
            return !addedPaths.has(videoPath);
        });

        if (filteredVideos.length === 0) {
            container.innerHTML = `<p style="color: var(--text-secondary); padding: 1rem;">${config.allAddedMessage}</p>`;
            return;
        }

        container.innerHTML = filteredVideos.map((video, index) => {
            const originalIndex = available.indexOf(video);
            return `
                <div class="content-item" draggable="true"
                    data-type="video" data-index="${originalIndex}" data-target="video"
                    ondragstart="handleDragStart(event, 'video')" ondragend="handleDragEnd(event)">
                    ${config.icon} ${escapeHtml(video.filename)}
                </div>
            `;
        }).join('');
    } else {
        // Game/ROM rendering - organized by system
        const availableROMs = config.getAvailable();
        const systems = Object.keys(availableROMs);

        if (systems.length === 0) {
            container.innerHTML = `<p style="color: var(--text-secondary); padding: 1rem;">${config.emptyMessage}</p>`;
            return;
        }

        let html = '';
        let totalAvailable = 0;

        systems.forEach(system => {
            const roms = availableROMs[system];
            const filteredRoms = roms.filter(rom => !addedPaths.has(rom.path));

            if (filteredRoms.length > 0) {
                html += `<div style="margin-bottom: 1rem;"><strong style="color: var(--accent2);">${escapeHtml(system.toUpperCase())}</strong></div>`;
                filteredRoms.forEach((rom) => {
                    const originalIndex = roms.indexOf(rom);
                    totalAvailable++;
                    html += `
                        <div class="content-item" draggable="true"
                            data-type="rom" data-system="${escapeHtml(system)}" data-index="${originalIndex}" data-target="game"
                            ondragstart="handleDragStart(event, 'game')" ondragend="handleDragEnd(event)">
                            ${config.icon} ${escapeHtml(rom.filename)}
                        </div>
                    `;
                });
            }
        });

        if (totalAvailable === 0) {
            container.innerHTML = `<p style="color: var(--text-secondary); padding: 1rem;">${config.allAddedMessage}</p>`;
            return;
        }

        container.innerHTML = html;
    }

    // Add touch event handlers for mobile
    if (hasTouch) {
        setupTouchHandlers(type);
    }
}

// ===== UI NAVIGATION =====
// Legacy wrapper functions for backward compatibility

function switchVideoView(viewName) {
    switchContentView('video', viewName);
}

function switchSourceTab(tabName) {
    // Update tabs
    document.querySelectorAll('.sidebar-tab').forEach(tab => tab.classList.remove('active'));
    event.target.classList.add('active');

    // Update content
    document.querySelectorAll('.source-content').forEach(content => content.style.display = 'none');
    document.getElementById(`source${tabName.charAt(0).toUpperCase() + tabName.slice(1)}`).style.display = 'flex';
}

function createNewVideoPlaylist() {
    createNewPlaylist('video');
}

// ===== VIDEO MANAGEMENT =====

async function loadVideos() {
    if (!AppState.device.current) return;

    try {
        const response = await fetch(`${AppState.device.url}/admin/media`);
        const result = await response.json();
        // Use AppState to store videos (triggers videosUpdated event)
        AppState.media.videos = result.data || result;

        console.log(`Loaded ${AppState.media.videos.length} videos from /admin/media`);

        // Update count badges
        const countBadges = document.querySelectorAll('#videoCount');
        countBadges.forEach(badge => badge.textContent = AppState.media.videos.length);

        const container = document.getElementById('videoList');
        if (availableVideos.length === 0) {
            container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No videos uploaded yet</p>';
            return;
        }

        // Load all playlists to check video usage
        const playlistsResponse = await fetch(`${currentDevice.url}/admin/playlists`);
        const playlistsResult = await playlistsResponse.json();
        const allPlaylists = playlistsResult.data || playlistsResult;

        // Build a map of video paths to playlists that use them
        const videoUsageMap = {};
        for (const playlist of allPlaylists) {
            try {
                const detailResponse = await fetch(`${currentDevice.url}/admin/playlists/${playlist.filename}`);
                const detailResult = await detailResponse.json();
                const fullData = detailResult.data || detailResult;
                // Normalize path for comparison - handles various path formats:
                // ../media/file.mp4, data/media/file.mp4, /data/media/file.mp4, media/file.mp4
                const normalizePath = (p) => {
                    if (!p) return '';
                    let clean = p;
                    // Remove leading slash
                    clean = clean.replace(/^\//, '');
                    // Remove ../ prefixes (used in playlist relative paths)
                    clean = clean.replace(/^\.\.\/+/, '');
                    // Remove data/ or dev_data/ prefix
                    clean = clean.replace(/^(dev_)?data\//, '');
                    // Remove media/ prefix
                    clean = clean.replace(/^media\//, '');
                    // At this point we should have: filename.mp4
                    return clean;
                };

                const items = fullData.items || [];
                items.forEach(item => {
                    if (item.source_type === 'local' || item.source_type === 'emulated_game') {
                        const normalizedPath = normalizePath(item.path);

                        if (!videoUsageMap[normalizedPath]) {
                            videoUsageMap[normalizedPath] = {
                                playlists: [],
                                title: item.title, // Use the first title found
                                artist: item.artist
                            };
                        }

                        // Add this playlist to the usage list if not already there
                        if (!videoUsageMap[normalizedPath].playlists.includes(playlist.title)) {
                            videoUsageMap[normalizedPath].playlists.push(playlist.title);
                        }// This way if renamed differently in different playlists, we track all versions
                        if (!videoUsageMap[normalizedPath].allTitles) {
                            videoUsageMap[normalizedPath].allTitles = new Set();
                        }
                        videoUsageMap[normalizedPath].allTitles.add(`${item.title}${item.artist ? ' - ' + item.artist : ''}`);
                    }
                });
            } catch (e) {
                // Skip problematic playlist
            }
        }

        // Store globally for library panel filtering
        globalVideoUsageMap = videoUsageMap;

        // Create table view function (reusable)
        const createTable = (videos) => `
            <div style="overflow-x: auto; width: 100%;">
                <table class="video-library-table">
                    <thead>
                        <tr>
                            <th style="min-width: 150px;">Title</th>
                            <th style="min-width: 120px;">Artist</th>
                            <th style="min-width: 200px;">Filename</th>
                            <th style="min-width: 80px;">Size</th>
                            <th style="min-width: 200px;">Used In Playlists</th>
                            <th style="min-width: 80px;">Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        ${videos.map(video => {
            const videoPath = video.path || `media/${video.filename}`;
            // Normalize the video path same as we did for playlist items
            const normalizePath = (p) => {
                if (!p) return '';
                let clean = p;
                clean = clean.replace(/^\//, ''); // Remove leading slash
                clean = clean.replace(/^\.\.\/+/, ''); // Remove ../ prefixes
                clean = clean.replace(/^(dev_)?data\//, ''); // Remove data/ prefix
                clean = clean.replace(/^media\//, ''); // Remove media/ prefix
                return clean;
            };

            const normalizedVideoPath = normalizePath(videoPath);
            const usage = videoUsageMap[normalizedVideoPath];
            const inPlaylists = usage && usage.playlists.length > 0;
            const title = usage?.title || video.filename;
            const artist = usage?.artist || '';

            return `
                                <tr>
                                    <td class="video-title">${escapeHtml(title)}</td>
                                    <td class="video-artist">${escapeHtml(artist) || '<em style="color: var(--text-secondary);">-</em>'}</td>
                                    <td class="video-filename">${escapeHtml(video.filename)}</td>
                                    <td class="video-size">${formatFileSize(video.size)}</td>
                                    <td class="video-usage">
                                        ${inPlaylists
                    ? `<span class="usage-badge in-use">✓ ${escapeHtml(usage.playlists.join(', '))}</span>`
                    : '<span class="usage-badge unused">⚠ Not used</span>'}
                                    </td>
                                    <td class="video-actions">
                                        <button class="btn-remove" onclick="deleteVideo('${escapeJs(video.path)}', '${escapeJs(video.filename)}')">Delete</button>
                                    </td>
                                </tr>
                            `;
        }).join('')}
                    </tbody>
                </table>
            </div>
        `;

        container.innerHTML = createTable(availableVideos);

        // CRITICAL: Update playlist builder's available videos after loading
        renderVideoPlaylistAvailable();
    } catch (e) {
        console.error('Failed to load videos:', e);
    }
}

async function handleDirectUpload(input) {
    if (input.files.length === 0) return;

    const files = Array.from(input.files);
    const targetResolution = document.getElementById('targetResolution')?.value || UPLOAD_CONFIG.TRANSCODE.DEFAULT_RESOLUTION;
    const normalizeAudio = document.getElementById('normalizeAudio')?.checked || false;

    // Auto-expand the upload section if it's collapsed
    const uploadContent = document.getElementById('libraryUploadContent');
    const uploadIcon = document.getElementById('uploadToggleIcon');
    if (uploadContent && uploadContent.style.display === 'none') {
        uploadContent.style.display = 'block';
        if (uploadIcon) uploadIcon.textContent = '▲';
    }

    // Get the status container
    const statusEl = document.getElementById('transcodeStatus');
    const labelEl = statusEl?.querySelector('.transcode-label');
    const progressEl = statusEl?.querySelector('.transcode-progress-fill');
    const detailsEl = statusEl?.querySelector('.transcode-details');
    const iconEl = statusEl?.querySelector('.transcode-icon');

    // Show batch status immediately with uploading state
    if (statusEl) statusEl.style.display = 'block';
    if (iconEl) iconEl.textContent = '⬆️';  // Upload icon
    if (labelEl) labelEl.textContent = `Uploading ${files.length} file${files.length > 1 ? 's' : ''}`;
    if (progressEl) progressEl.style.width = '0%';
    if (detailsEl) detailsEl.textContent = 'Preparing upload...';
    statusEl?.classList.add('loading');

    let completed = 0;
    let skipped = 0;  // Files that were already compatible
    let errors = 0;

    // Process files - track transcoding jobs
    const pendingJobs = [];

    for (const file of files) {
        const truncatedName = file.name.length > 25 ? file.name.substring(0, 22) + '...' : file.name;

        try {
            // Update status for this file
            if (detailsEl) detailsEl.textContent = `Uploading: ${truncatedName}`;
            console.log(`[SmartUpload] Processing: ${file.name}`);

            // Upload to smart-upload endpoint
            const formData = new FormData();
            formData.append('file', file);
            formData.append('resolution', targetResolution);
            formData.append('normalize_audio', normalizeAudio ? 'true' : 'false');

            const uploadResponse = await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.open('POST', `${currentDevice.url}/admin/smart-upload`);

                if (csrfToken) {
                    xhr.setRequestHeader('X-CSRF-Token', csrfToken);
                }

                xhr.upload.onprogress = (e) => {
                    if (e.lengthComputable) {
                        const mb = (e.loaded / 1024 / 1024).toFixed(1);
                        if (detailsEl) detailsEl.textContent = `Uploading ${truncatedName}: ${mb}MB`;
                    }
                };

                xhr.onload = () => {
                    if (xhr.status >= 200 && xhr.status < 300) {
                        try {
                            resolve(JSON.parse(xhr.responseText));
                        } catch (e) {
                            reject(new Error('Invalid server response'));
                        }
                    } else {
                        reject(new Error(`Upload failed: ${xhr.status}`));
                    }
                };

                xhr.onerror = () => reject(new Error('Network error'));
                xhr.send(formData);
            });

            if (uploadResponse.data?.action === 'direct') {
                // File was compatible - already saved directly
                console.log(`[SmartUpload] ${file.name}: Direct upload (compatible)`);
                completed++;
                skipped++;
            } else if (uploadResponse.data?.action === 'transcode') {
                // File needs transcoding - track the job
                console.log(`[SmartUpload] ${file.name}: Transcoding started (job: ${uploadResponse.data.job_id})`);
                pendingJobs.push({
                    jobId: uploadResponse.data.job_id,
                    fileName: file.name,
                    truncatedName
                });
            }

        } catch (error) {
            console.error(`[SmartUpload] Error processing ${file.name}:`, error);
            errors++;
        }

        // Update overall progress
        const overallProgress = Math.round(((completed + pendingJobs.length + errors) / files.length) * 50);
        if (progressEl) progressEl.style.width = `${overallProgress}%`;
    }

    // Now poll for pending transcoding jobs
    if (pendingJobs.length > 0) {
        // Change icon and label for transcoding phase
        if (iconEl) iconEl.textContent = '⚙️';  // Transcoding icon
        if (labelEl) labelEl.textContent = `Transcoding ${pendingJobs.length} file${pendingJobs.length > 1 ? 's' : ''}`;

        while (pendingJobs.length > 0) {
            await new Promise(resolve => setTimeout(resolve, 1000)); // Poll every second

            for (let i = pendingJobs.length - 1; i >= 0; i--) {
                const job = pendingJobs[i];

                try {
                    const statusResponse = await fetch(`${currentDevice.url}/admin/transcode-status/${job.jobId}`);
                    const statusData = await statusResponse.json();

                    if (statusData.data?.status === 'complete') {
                        console.log(`[SmartUpload] ${job.fileName}: Transcode complete`);
                        completed++;
                        pendingJobs.splice(i, 1);
                    } else if (statusData.data?.status === 'error') {
                        console.error(`[SmartUpload] ${job.fileName}: Transcode failed`);
                        errors++;
                        pendingJobs.splice(i, 1);
                    } else {
                        // Still running - update status
                        if (detailsEl && pendingJobs.length === 1) {
                            detailsEl.textContent = statusData.data?.message || `Transcoding: ${job.truncatedName}`;
                        }
                    }
                } catch (e) {
                    console.warn(`[SmartUpload] Error polling job ${job.jobId}:`, e);
                }
            }

            // Update progress based on completed/pending
            const overallProgress = 50 + Math.round((completed / files.length) * 50);
            if (progressEl) progressEl.style.width = `${overallProgress}%`;

            if (detailsEl && pendingJobs.length > 1) {
                detailsEl.textContent = `${pendingJobs.length} files still transcoding...`;
            }
        }
    }

    // All done!
    if (progressEl) progressEl.style.width = '100%';
    statusEl?.classList.remove('loading');
    statusEl?.classList.add('complete');

    // Build completion message
    const parts = [];
    if (skipped > 0) parts.push(`${skipped} already compatible`);
    if (completed - skipped > 0) parts.push(`${completed - skipped} transcoded`);
    if (errors > 0) parts.push(`${errors} failed`);

    if (labelEl) labelEl.textContent = `Complete!`;
    if (detailsEl) detailsEl.textContent = parts.join(', ') || `${completed} files processed`;

    // Wait a moment then hide
    await new Promise(resolve => setTimeout(resolve, 2500));
    if (statusEl) statusEl.style.display = 'none';
    statusEl?.classList.remove('complete', 'loading', 'error');

    // Refresh media list
    try {
        await loadAllContentFromDevice();

        // Auto-add uploaded files to the current playlist
        const successfulFiles = files.filter((f, i) => {
            // Exclude files that had errors
            return true; // We tracked completed count above
        }).map(f => f.name);

        // Check if we're in the playlist editor (videoEditorView is visible)
        const editorView = document.getElementById('videoEditorView');
        if (editorView && editorView.style.display !== 'none') {
            const config = MEDIA_CONFIG['video'];
            const currentItems = config.getItems();
            const currentPaths = new Set(currentItems.map(item => item.path));

            // Find matching videos in the refreshed media list
            let addedCount = 0;
            for (const fileName of successfulFiles) {
                // Video might have been transcoded to .mp4
                const baseName = fileName.replace(/\.[^.]+$/, '');
                const matchingVideo = availableVideos.find(v => {
                    const videoBaseName = v.filename.replace(/\.[^.]+$/, '');
                    return videoBaseName === baseName || v.filename === fileName;
                });

                if (matchingVideo) {
                    const videoPath = matchingVideo.path || `media/${matchingVideo.filename}`;

                    // Only add if not already in playlist
                    if (!currentPaths.has(videoPath)) {
                        // Find first empty slot or add to end
                        const emptySlotIndex = currentItems.findIndex(item => item.isEmpty);

                        const newItem = {
                            source_type: 'local',
                            path: videoPath,
                            title: matchingVideo.filename.replace(/\.[^.]+$/, ''),
                            artist: ''
                        };

                        if (emptySlotIndex >= 0) {
                            currentItems[emptySlotIndex] = newItem;
                        } else {
                            currentItems.push(newItem);
                        }
                        currentPaths.add(videoPath);
                        addedCount++;
                    }
                }
            }

            if (addedCount > 0) {
                config.setItems(currentItems);
                renderPlaylistItems('video');
                renderLibraryPanel();
                console.log(`[SmartUpload] Auto-added ${addedCount} videos to playlist`);
            }
        }
    } catch (e) {
        console.warn('[SmartUpload] Media refresh failed:', e);
    }

    // Clear the input
    input.value = '';
}

/**
 * Process a video file: upload to Pi and transcode server-side
 * @param {File} file - The video file
 * @param {string} resolution - Target resolution key ('crt' or 'modern')
 * @param {boolean} autoAddToPlaylist - Whether to add to current playlist
 */
async function processVideoFile(file, resolution, autoAddToPlaylist) {
    const statusEl = document.getElementById('transcodeStatus');
    const labelEl = statusEl?.querySelector('.transcode-label');
    const progressEl = statusEl?.querySelector('.transcode-progress-fill');
    const detailsEl = statusEl?.querySelector('.transcode-details');
    const iconEl = statusEl?.querySelector('.transcode-icon');

    // Helper to update status UI
    const updateStatus = (label, details, progress = null, state = '') => {
        if (statusEl) statusEl.style.display = 'block';
        if (labelEl) labelEl.textContent = label;
        if (detailsEl) detailsEl.textContent = details;
        if (progress !== null && progressEl) progressEl.style.width = `${progress}%`;
        if (statusEl) {
            statusEl.classList.remove('loading', 'complete', 'error');
            if (state) statusEl.classList.add(state);
        }
        if (iconEl) {
            iconEl.textContent = state === 'error' ? '❌' : state === 'complete' ? '✓' : '⚙️';
        }
    };

    // Helper to hide status
    const hideStatus = () => {
        if (statusEl) statusEl.style.display = 'none';
    };

    try {
        const truncatedName = file.name.length > 30 ? file.name.substring(0, 27) + '...' : file.name;

        // Step 1: Upload file to Pi for transcoding
        updateStatus(`Sending to Pi`, truncatedName, 0, 'loading');
        console.log(`[Transcode] Uploading ${file.name} for server-side transcoding`);

        const formData = new FormData();
        formData.append('file', file);
        formData.append('resolution', resolution);

        // Upload with progress tracking
        const uploadResponse = await new Promise((resolve, reject) => {
            const xhr = new XMLHttpRequest();
            xhr.open('POST', `${currentDevice.url}/admin/upload-and-transcode`);

            // Add CSRF token (use global csrfToken variable)
            if (csrfToken) {
                xhr.setRequestHeader('X-CSRF-Token', csrfToken);
            }

            xhr.upload.onprogress = (e) => {
                if (e.lengthComputable) {
                    const pct = Math.round((e.loaded / e.total) * 50); // Upload is 0-50%
                    if (progressEl) progressEl.style.width = `${pct}%`;
                    if (detailsEl) {
                        const mbLoaded = (e.loaded / 1024 / 1024).toFixed(1);
                        const mbTotal = (e.total / 1024 / 1024).toFixed(1);
                        detailsEl.textContent = `Uploading ${mbLoaded}/${mbTotal} MB`;
                    }
                }
            };

            xhr.onload = () => {
                if (xhr.status >= 200 && xhr.status < 300) {
                    try {
                        resolve(JSON.parse(xhr.responseText));
                    } catch (e) {
                        reject(new Error('Invalid server response'));
                    }
                } else {
                    reject(new Error(`Upload failed: ${xhr.status}`));
                }
            };

            xhr.onerror = () => reject(new Error('Network error'));
            xhr.send(formData);
        });

        if (!uploadResponse.data?.job_id) {
            throw new Error('No job ID returned from server');
        }

        const jobId = uploadResponse.data.job_id;
        console.log(`[Transcode] Job started: ${jobId}`);

        // Step 2: Poll for transcoding progress
        updateStatus('Converting on Pi', truncatedName, 50);

        let complete = false;
        let lastProgress = 0;
        while (!complete) {
            await new Promise(resolve => setTimeout(resolve, 1000)); // Poll every second

            const statusResponse = await fetch(`${currentDevice.url}/admin/transcode-status/${jobId}`);
            const statusData = await statusResponse.json();

            if (!statusData.data) {
                console.warn('[Transcode] No status data received');
                continue;
            }

            const { status, progress, message } = statusData.data;

            // Map transcoding progress to 50-100% range (upload was 0-50%)
            const displayProgress = 50 + Math.round(progress / 2);
            if (progressEl) progressEl.style.width = `${displayProgress}%`;
            if (detailsEl) detailsEl.textContent = message;
            lastProgress = displayProgress;

            if (status === 'complete') {
                complete = true;
                console.log(`[Transcode] Complete! Output: ${statusData.data.output_filename}`);
            } else if (status === 'error') {
                throw new Error(message || 'Transcoding failed');
            }
        }

        // Success!
        updateStatus('Complete!', 'Video is ready', 100, 'complete');
        await new Promise(resolve => setTimeout(resolve, 1000));
        hideStatus();
    } catch (error) {
        console.error('[Transcode] Error:', error);

        // Show error state
        updateStatus('Conversion failed', error.message, 0, 'error');
        await new Promise(resolve => setTimeout(resolve, 2000));
        hideStatus();

        // Ask user if they want to upload original without transcoding
        const uploadOriginal = confirm(
            `Video conversion failed: ${error.message}\n\n` +
            `Would you like to upload the original file without conversion?\n` +
            `Note: It may not play correctly on the Magic Dingus Box.`
        );

        if (uploadOriginal) {
            await uploadVideos([file], autoAddToPlaylist);
        }
        return; // Exit early on error
    }

    // Refresh media list AFTER successful transcoding (in separate try/catch)
    try {
        await loadAllContentFromDevice();
    } catch (refreshError) {
        console.warn('[Transcode] Media refresh failed:', refreshError);
        // Don't trigger fallback for refresh errors
    }
}


async function uploadVideos(fileList = null, autoAddToPlaylist = false) {
    if (!currentDevice) {
        alert('Please select a device first');
        return;
    }

    // Use provided fileList or get from default input
    let files = [];
    let progressElement = null;

    if (fileList) {
        files = Array.from(fileList);
        // For direct upload, use the progress in the upload tab
        progressElement = document.querySelector('#sourceUpload #uploadProgress');
    } else {
        const input = document.getElementById('videoUpload');
        files = Array.from(input.files);
        progressElement = document.getElementById('uploadProgress');
        input.value = ''; // Clear input
    }

    if (!progressElement) progressElement = document.getElementById('uploadProgress');
    progressElement.innerHTML = '';

    // Create batch progress header
    const batchHeader = document.createElement('div');
    batchHeader.className = 'batch-progress-header';
    batchHeader.innerHTML = `
        <div class="batch-progress-text">
            <span class="batch-status">Uploading...</span>
            <span class="batch-counts">0 of ${files.length} complete</span>
        </div>
        <div class="batch-progress-bar">
            <div class="batch-progress-fill" style="width: 0%"></div>
        </div>
    `;
    progressElement.appendChild(batchHeader);

    // Batch tracking state
    const batchState = {
        total: files.length,
        completed: 0,
        failed: 0,
        failedFiles: [],
        updateHeader: function () {
            const statusEl = batchHeader.querySelector('.batch-status');
            const countsEl = batchHeader.querySelector('.batch-counts');
            const fillEl = batchHeader.querySelector('.batch-progress-fill');

            const done = this.completed + this.failed;
            const percent = Math.round((done / this.total) * 100);

            countsEl.textContent = `${this.completed} of ${this.total} complete` +
                (this.failed > 0 ? ` (${this.failed} failed)` : '');
            fillEl.style.width = `${percent}%`;

            if (done === this.total) {
                if (this.failed === 0) {
                    statusEl.textContent = 'All uploads complete! ✓';
                    batchHeader.classList.add('complete');
                } else {
                    statusEl.textContent = `Done with ${this.failed} error(s)`;
                    batchHeader.classList.add('has-errors');
                }
            }
        }
    };

    // Create progress bars for all files first (marked as queued)
    const queue = [];
    for (const file of files) {
        const progressBar = createProgressBar(file.name);
        progressElement.appendChild(progressBar.element);
        queue.push({ file, progressBar, status: 'queued' });
    }

    // Process queue with concurrency limit
    const { CONCURRENCY_LIMIT } = UPLOAD_CONFIG;
    let activeUploads = 0;
    let currentIndex = 0;

    return new Promise((resolveAll) => {
        const processQueue = () => {
            while (activeUploads < CONCURRENCY_LIMIT && currentIndex < queue.length) {
                const item = queue[currentIndex++];
                activeUploads++;

                uploadSingleFile(item.file, item.progressBar, autoAddToPlaylist)
                    .then((result) => {
                        if (result.success) {
                            batchState.completed++;
                        } else {
                            batchState.failed++;
                            batchState.failedFiles.push({
                                file: item.file,
                                error: result.error
                            });
                        }
                        batchState.updateHeader();
                    })
                    .finally(() => {
                        activeUploads--;
                        processQueue();

                        // Check if all done
                        if (activeUploads === 0 && currentIndex >= queue.length) {
                            // Show retry button if there were failures
                            if (batchState.failedFiles.length > 0) {
                                const retryBtn = document.createElement('button');
                                retryBtn.className = 'btn-primary small';
                                retryBtn.textContent = `Retry ${batchState.failedFiles.length} Failed Upload(s)`;
                                retryBtn.style.marginTop = '1rem';
                                retryBtn.onclick = () => {
                                    const failedFileList = batchState.failedFiles.map(f => f.file);
                                    // Create a fake FileList-like object
                                    uploadVideos(failedFileList, autoAddToPlaylist);
                                };
                                progressElement.appendChild(retryBtn);
                            }

                            // Refresh video list once at the end
                            setTimeout(() => loadVideos(), 500);
                            resolveAll();
                        }
                    });
            }
        };

        processQueue();
    });
}

function uploadSingleFile(file, progressBar, autoAddToPlaylist) {
    return new Promise((resolve) => {
        const { MAX_RETRIES, RETRY_DELAY_MS, TIMEOUT_MS } = UPLOAD_CONFIG;
        let attempts = 0;

        const attemptUpload = () => {
            attempts++;

            const formData = new FormData();
            formData.append('file', file);

            const xhr = new XMLHttpRequest();
            let timeoutId = null;

            // Set up timeout
            xhr.timeout = TIMEOUT_MS;

            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    progressBar.update(percent);
                }
            });

            xhr.addEventListener('load', () => {
                clearTimeout(timeoutId);

                if (xhr.status === 200) {
                    progressBar.complete();

                    try {
                        const response = JSON.parse(xhr.responseText);

                        // If auto-add is requested, add to playlist immediately
                        if (autoAddToPlaylist) {
                            const videoItem = {
                                title: file.name.replace(/\.[^/.]+$/, ""), // Remove extension
                                artist: '',
                                source_type: 'local',
                                path: response.path || `data/media/${file.name}`
                            };

                            const config = MEDIA_CONFIG['video'];
                            const items = config.getItems();
                            items.push(videoItem);
                            config.setItems(items);
                            renderPlaylistItems('video');

                            setTimeout(() => {
                                switchSourceTab('library');
                            }, 1000);
                        }
                    } catch (e) {
                        console.error('Error parsing upload response:', e);
                    }

                    resolve({ success: true });
                } else {
                    handleError(`Server error (${xhr.status})`);
                }
            });

            xhr.addEventListener('error', () => {
                clearTimeout(timeoutId);
                handleError('Network error');
            });

            xhr.addEventListener('timeout', () => {
                handleError('Upload timed out');
            });

            const handleError = (errorMessage) => {
                if (attempts < MAX_RETRIES) {
                    progressBar.setRetrying(attempts, MAX_RETRIES);
                    setTimeout(attemptUpload, RETRY_DELAY_MS * attempts); // Exponential backoff
                } else {
                    progressBar.error(errorMessage);
                    resolve({ success: false, error: errorMessage });
                }
            };

            try {
                xhr.open('POST', `${currentDevice.url}/admin/upload`);
                if (csrfToken) {
                    xhr.setRequestHeader('X-CSRF-Token', csrfToken);
                }
                xhr.send(formData);
            } catch (error) {
                console.error('Upload error:', error);
                handleError('Upload failed');
            }
        };

        attemptUpload();
    });
}

function filterVideoLibrary() {
    filterLibrary('video');
}

function filterMainLibrary(input) {
    const query = input.value.toLowerCase();
    const rows = document.querySelectorAll('.video-library-table tbody tr');

    rows.forEach(row => {
        const text = row.textContent.toLowerCase();
        row.style.display = text.includes(query) ? 'table-row' : 'none';
    });
}

// ===== GAME UI NAVIGATION =====
// Legacy wrapper functions for backward compatibility

function switchGameView(viewName) {
    switchContentView('game', viewName);
}

function switchGameSourceTab(tabName) {
    // Update tabs
    document.querySelectorAll('#roms .sidebar-tab').forEach(tab => tab.classList.remove('active'));
    event.target.classList.add('active');

    // Update content
    document.querySelectorAll('#roms .source-content').forEach(content => content.style.display = 'none');
    document.getElementById(`sourceGame${tabName.charAt(0).toUpperCase() + tabName.slice(1)}`).style.display = 'flex';
}

function createNewGamePlaylist() {
    createNewPlaylist('game');
}

function filterGameLibrary() {
    filterLibrary('game');
}

// ===== ROM MANAGEMENT =====

async function loadROMs() {
    if (!AppState.device.current) return;

    try {
        const response = await fetch(`${AppState.device.url}/admin/roms`);
        const result = await response.json();
        // Use AppState to store ROMs (triggers romsUpdated event)
        AppState.media.roms = result.data || result;

        // Calculate total ROM count
        const totalROMs = Object.values(AppState.media.roms).reduce((sum, roms) => sum + roms.length, 0);

        // Update count badge
        const countBadge = document.getElementById('romCount');
        if (countBadge) {
            countBadge.textContent = totalROMs;
        }

        const container = document.getElementById('romsList');
        const systems = Object.keys(availableROMs);

        if (systems.length === 0) {
            container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No ROMs uploaded yet</p>';
            return;
        }

        // Flatten ROMs list for table view
        let allRoms = [];
        systems.forEach(system => {
            availableROMs[system].forEach(rom => {
                rom.system = system;
                allRoms.push(rom);
            });
        });

        // Create table view
        container.innerHTML = `
            <div style="overflow-x: auto; width: 100%;">
                <table class="video-library-table">
                    <thead>
                        <tr>
                            <th style="min-width: 200px;">Filename</th>
                            <th style="min-width: 100px;">System</th>
                            <th style="min-width: 100px;">Size</th>
                            <th style="min-width: 80px;">Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        ${allRoms.map(rom => `
                            <tr>
                                <td class="video-title">${escapeHtml(rom.filename)}</td>
                                <td class="video-artist">${escapeHtml(rom.system.toUpperCase())}</td>
                                <td class="video-size">${formatFileSize(rom.size)}</td>
                                <td class="video-actions">
                                    <button class="btn-remove" onclick="deleteROM('${escapeJs(rom.path)}', '${escapeJs(rom.filename)}')">Delete</button>
                                </td>
                            </tr>
                        `).join('')}
                    </tbody>
                </table>
            </div>
        `;

        // Update playlist builder available items
        renderGamePlaylistAvailable();

    } catch (e) {
        console.error('Failed to load ROMs:', e);
    }
}



function toggleAccordion(header) {
    const content = header.nextElementSibling;
    const isActive = content.classList.contains('active');

    // Close all accordions
    document.querySelectorAll('.accordion-content').forEach(c => c.classList.remove('active'));

    // Toggle this one
    if (!isActive) {
        content.classList.add('active');
    }
}

async function handleDirectROMUpload(input) {
    if (input.files.length > 0) {
        await uploadROMs(input.files, true); // true = autoAddToPlaylist
    }
}

async function uploadROMs(fileList = null, autoAddToPlaylist = false) {
    if (!currentDevice) {
        alert('Please select a device first');
        return;
    }

    let files = [];
    let system = 'nes'; // Default
    let progressElement = null;

    if (fileList) {
        files = Array.from(fileList);
        // Get system from the selector in the upload tab
        const selector = document.getElementById('systemSelect');
        if (selector) system = selector.value;
        progressElement = document.getElementById('romUploadProgress');
    } else {
        const input = document.getElementById('romUpload');
        files = Array.from(input.files);
        system = document.getElementById('systemSelect').value;
        progressElement = document.getElementById('romUploadProgress');
        input.value = '';
    }

    if (!progressElement) progressElement = document.getElementById('romUploadProgress');
    progressElement.innerHTML = '';

    // Create batch progress header
    const batchHeader = document.createElement('div');
    batchHeader.className = 'batch-progress-header';
    batchHeader.innerHTML = `
        <div class="batch-progress-text">
            <span class="batch-status">Uploading ROMs...</span>
            <span class="batch-counts">0 of ${files.length} complete</span>
        </div>
        <div class="batch-progress-bar">
            <div class="batch-progress-fill" style="width: 0%"></div>
        </div>
    `;
    progressElement.appendChild(batchHeader);

    // Batch tracking state
    const batchState = {
        total: files.length,
        completed: 0,
        failed: 0,
        failedFiles: [],
        updateHeader: function () {
            const statusEl = batchHeader.querySelector('.batch-status');
            const countsEl = batchHeader.querySelector('.batch-counts');
            const fillEl = batchHeader.querySelector('.batch-progress-fill');

            const done = this.completed + this.failed;
            const percent = Math.round((done / this.total) * 100);

            countsEl.textContent = `${this.completed} of ${this.total} complete` +
                (this.failed > 0 ? ` (${this.failed} failed)` : '');
            fillEl.style.width = `${percent}%`;

            if (done === this.total) {
                if (this.failed === 0) {
                    statusEl.textContent = 'All uploads complete! ✓';
                    batchHeader.classList.add('complete');
                } else {
                    statusEl.textContent = `Done with ${this.failed} error(s)`;
                    batchHeader.classList.add('has-errors');
                }
            }
        }
    };

    // Create progress bars for all files first (marked as queued)
    const queue = [];
    for (const file of files) {
        const progressBar = createProgressBar(file.name);
        progressElement.appendChild(progressBar.element);
        queue.push({ file, progressBar, status: 'queued' });
    }

    // Process queue with concurrency limit
    const { CONCURRENCY_LIMIT } = UPLOAD_CONFIG;
    let activeUploads = 0;
    let currentIndex = 0;

    return new Promise((resolveAll) => {
        const processQueue = () => {
            while (activeUploads < CONCURRENCY_LIMIT && currentIndex < queue.length) {
                const item = queue[currentIndex++];
                activeUploads++;

                uploadSingleROM(item.file, system, item.progressBar, autoAddToPlaylist)
                    .then((result) => {
                        if (result.success) {
                            batchState.completed++;
                        } else {
                            batchState.failed++;
                            batchState.failedFiles.push({
                                file: item.file,
                                error: result.error
                            });
                        }
                        batchState.updateHeader();
                    })
                    .finally(() => {
                        activeUploads--;
                        processQueue();

                        // Check if all done
                        if (activeUploads === 0 && currentIndex >= queue.length) {
                            // Show retry button if there were failures
                            if (batchState.failedFiles.length > 0) {
                                const retryBtn = document.createElement('button');
                                retryBtn.className = 'btn-primary small';
                                retryBtn.textContent = `Retry ${batchState.failedFiles.length} Failed Upload(s)`;
                                retryBtn.style.marginTop = '1rem';
                                retryBtn.onclick = () => {
                                    const failedFileList = batchState.failedFiles.map(f => f.file);
                                    uploadROMs(failedFileList, autoAddToPlaylist);
                                };
                                progressElement.appendChild(retryBtn);
                            }

                            // Refresh ROM list once at the end
                            setTimeout(() => loadROMs(), 500);
                            resolveAll();
                        }
                    });
            }
        };

        processQueue();
    });
}

function uploadSingleROM(file, system, progressBar, autoAddToPlaylist) {
    return new Promise((resolve) => {
        const { MAX_RETRIES, RETRY_DELAY_MS, TIMEOUT_MS } = UPLOAD_CONFIG;
        let attempts = 0;

        const attemptUpload = () => {
            attempts++;

            const formData = new FormData();
            formData.append('file', file);

            const xhr = new XMLHttpRequest();
            let timeoutId = null;

            // Set up timeout
            xhr.timeout = TIMEOUT_MS;

            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    progressBar.update(percent);
                }
            });

            xhr.addEventListener('load', () => {
                clearTimeout(timeoutId);

                if (xhr.status === 200) {
                    progressBar.complete();

                    try {
                        const result = JSON.parse(xhr.responseText);

                        if (autoAddToPlaylist) {
                            // Define core map for uploads too
                            const coreMap = {
                                'nes': 'nestopia_libretro',
                                'snes': 'snes9x2010_libretro',
                                'genesis': 'genesis_plus_gx_libretro',
                                'ps1': 'pcsx_rearmed_libretro',
                                'atari7800': 'prosystem_libretro',
                                'pcengine': 'mednafen_pce_fast_libretro',
                                'arcade': 'fbneo_libretro'
                            };

                            const playlistItem = {
                                title: file.name.replace(/\.[^/.]+$/, ""),
                                source_type: 'emulated_game',
                                path: result.path || `data/roms/${system}/${file.name}`,
                                emulator_system: system,
                                emulator_core: coreMap[system] || 'auto'
                            };

                            const config = MEDIA_CONFIG['game'];
                            const items = config.getItems();
                            items.push(playlistItem);
                            config.setItems(items);
                            renderPlaylistItems('game');

                            setTimeout(() => {
                                switchGameSourceTab('library');
                            }, 1000);
                        }
                    } catch (e) {
                        console.error('Error parsing upload response:', e);
                    }

                    resolve({ success: true });
                } else {
                    handleError(`Server error (${xhr.status})`);
                }
            });

            xhr.addEventListener('error', () => {
                clearTimeout(timeoutId);
                handleError('Network error');
            });

            xhr.addEventListener('timeout', () => {
                handleError('Upload timed out');
            });

            const handleError = (errorMessage) => {
                if (attempts < MAX_RETRIES) {
                    progressBar.setRetrying(attempts, MAX_RETRIES);
                    setTimeout(attemptUpload, RETRY_DELAY_MS * attempts);
                } else {
                    progressBar.error(errorMessage);
                    resolve({ success: false, error: errorMessage });
                }
            };

            try {
                xhr.open('POST', `${currentDevice.url}/admin/upload/rom/${system}`);
                if (csrfToken) {
                    xhr.setRequestHeader('X-CSRF-Token', csrfToken);
                }
                xhr.send(formData);
            } catch (error) {
                console.error('ROM upload error:', error);
                handleError('Upload failed');
            }
        };

        attemptUpload();
    });
}

// ===== PLAYLIST MANAGEMENT =====

async function loadExistingPlaylists() {
    if (!currentDevice) return;

    try {
        const response = await fetch(`${currentDevice.url}/admin/playlists`);
        const playlistsResult = await response.json();
        const allPlaylists = playlistsResult.data || playlistsResult;

        // Separate playlists by type (read full data to determine type)
        const videoPlaylists = [];
        const gamePlaylists = [];

        for (const pl of allPlaylists) {
            // Use explicit type if available, otherwise infer from items (legacy support)
            if (pl.playlist_type === 'game') {
                gamePlaylists.push(pl);
            } else if (pl.playlist_type === 'video') {
                videoPlaylists.push(pl);
            } else {
                // Fallback: Fetch full playlist to check item types
                try {
                    const detailResponse = await fetch(`${currentDevice.url}/admin/playlists/${pl.filename}`);
                    const detailResult = await detailResponse.json();
                    const fullData = detailResult.data || detailResult;

                    // Check if it's a game playlist (all items are games)
                    const items = fullData.items || [];
                    const isGamePlaylist = items.length > 0 && items.every(item => item.source_type === 'emulated_game');

                    if (isGamePlaylist) {
                        gamePlaylists.push(pl);
                    } else {
                        videoPlaylists.push(pl);
                    }
                } catch (e) {
                    // Default to video if can't determine
                    videoPlaylists.push(pl);
                }
            }
        }

        // Update count badges
        const videoCountBadge = document.getElementById('videoPlaylistCount');
        const gameCountBadge = document.getElementById('gamePlaylistCount');
        if (videoCountBadge) videoCountBadge.textContent = videoPlaylists.length;
        if (gameCountBadge) gameCountBadge.textContent = gamePlaylists.length;

        // Render video playlists
        const videoContainer = document.getElementById('videoPlaylistsList');
        if (videoPlaylists.length === 0) {
            videoContainer.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No video playlists created yet</p>';
        } else {
            videoContainer.innerHTML = videoPlaylists.map(pl => `
                <div class="playlist-card">
                    <div class="playlist-header">
                        <h4>${escapeHtml(pl.title)}</h4>
                        <div class="playlist-actions">
                            <button onclick="editPlaylist('${escapeJs(pl.filename)}', 'video')">Edit</button>
                            <button onclick="deletePlaylist('${escapeJs(pl.filename)}', 'video')">Delete</button>
                        </div>
                    </div>
                    <div class="playlist-meta">
                        By ${escapeHtml(pl.curator)} • ${pl.item_count} items
                        ${pl.description ? `<br><em>${escapeHtml(pl.description)}</em>` : ''}
                    </div>
                </div>
            `).join('');
        }

        // Render game playlists
        const gameContainer = document.getElementById('gamePlaylistsList');
        if (gamePlaylists.length === 0) {
            gameContainer.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No game playlists created yet</p>';
        } else {
            gameContainer.innerHTML = gamePlaylists.map(pl => `
                <div class="playlist-card">
                    <div class="playlist-header">
                        <h4>${escapeHtml(pl.title)}</h4>
                        <div class="playlist-actions">
                            <button onclick="editPlaylist('${escapeJs(pl.filename)}', 'game')">Edit</button>
                            <button onclick="deletePlaylist('${escapeJs(pl.filename)}', 'game')">Delete</button>
                        </div>
                    </div>
                    <div class="playlist-meta">
                        By ${escapeHtml(pl.curator)} • ${pl.item_count} items
                        ${pl.description ? `<br><em>${escapeHtml(pl.description)}</em>` : ''}
                    </div>
                </div>
            `).join('');
        }
    } catch (e) {
        console.error('Failed to load playlists:', e);
    }
}

async function editPlaylist(filename, type) {
    if (!currentDevice) return;

    try {
        const response = await fetch(`${currentDevice.url}/admin/playlists/${filename}`);
        const result = await response.json();
        const playlistData = result.data || result;

        // Determine which form to use
        const prefix = type === 'game' ? 'game' : 'video';
        const titleId = `${prefix}PlaylistTitle`;
        const curatorId = `${prefix}PlaylistCurator`;
        const descId = `${prefix}PlaylistDesc`;
        const loopId = `${prefix}PlaylistLoop`;
        const saveBtn = document.getElementById(`save${prefix.charAt(0).toUpperCase() + prefix.slice(1)}Playlist`);

        // Update editor title
        const editorTitle = document.getElementById('editorTitle');
        if (editorTitle && type === 'video') {
            editorTitle.textContent = 'Edit Playlist';
        }

        // Populate form
        document.getElementById(titleId).value = playlistData.title || '';
        document.getElementById(curatorId).value = playlistData.curator || '';
        document.getElementById(descId).value = playlistData.description || '';
        document.getElementById(loopId).checked = playlistData.loop || false;

        // Load items - ensure they're clean
        const items = cleanPlaylistItems(playlistData.items || []);

        // Use unified config to set items and switch view
        const config = MEDIA_CONFIG[type];
        config.setItems(items);
        renderPlaylistItems(type);

        // Refresh library availability view to reflect current playlist items
        if (type === 'video') {
            renderVideoPlaylistAvailable();
        } else if (type === 'game') {
            renderGamePlaylistAvailable();
        }

        switchContentView(type, 'editor');

        // Store filename
        if (saveBtn) saveBtn.dataset.editingFile = filename;

    } catch (e) {
        console.error('Failed to edit playlist:', e);
        alert('Failed to load playlist');
    }
}



async function performSavePlaylist(type) {
    console.log(`Saving ${type} playlist...`);
    if (!currentDevice) {
        console.error('No device connected');
        alert('Please select a device first');
        return;
    }

    // Determine which form to use
    const prefix = type === 'game' ? 'game' : 'video';
    const titleId = `${prefix}PlaylistTitle`;
    const curatorId = `${prefix}PlaylistCurator`;
    const descId = `${prefix}PlaylistDesc`;
    const loopId = `${prefix}PlaylistLoop`;
    const saveBtn = document.getElementById(`save${prefix.charAt(0).toUpperCase() + prefix.slice(1)}Playlist`);

    const title = document.getElementById(titleId).value.trim();
    const curator = document.getElementById(curatorId).value.trim();
    const description = document.getElementById(descId).value.trim();
    const loop = document.getElementById(loopId).checked;

    if (!title) {
        alert('Please enter a playlist title');
        return;
    }

    const items = MEDIA_CONFIG[type].getItems();

    if (items.length === 0) {
        alert('Please add at least one item to the playlist');
        return;
    }

    // Clean up playlist data
    const playlistData = {
        title,
        curator: curator || 'Unknown',
        loop,
        playlist_type: type, // 'game' or 'video'
        items: cleanPlaylistItems(items)
    };

    // Only add description if it has a value
    if (description) {
        playlistData.description = description;
    }

    // Determine filename
    const editingFile = saveBtn?.dataset.editingFile;
    const filename = editingFile || `${title.toLowerCase().replace(/\s+/g, '_')}.yaml`;

    try {
        const response = await fetch(`${currentDevice.url}/admin/playlists/${filename}`, {
            method: 'POST',
            headers: getCsrfHeaders(),
            body: JSON.stringify(playlistData)
        });

        if (response.ok) {
            cancelEdit(type);

            // Force switch view immediately
            if (type === 'game') {
                switchGameView('playlists');
            } else {
                switchVideoView('playlists');
            }

            await loadExistingPlaylists();
        } else {
            alert('Failed to save playlist');
        }
    } catch (e) {
        console.error('Failed to save playlist:', e);
        alert('Failed to save playlist');
    }
}

function savePlaylist(type) {
    console.log(`savePlaylist called for ${type}`);
    showConfirmModal(
        'Save Playlist?',
        'Are you sure you want to save changes to this playlist?',
        () => {
            console.log('Modal confirmed, calling performSavePlaylist');
            performSavePlaylist(type);
        }
    );
}

function cleanPlaylistItems(items) {
    // Remove any null/undefined/empty fields from items to match YAML format
    return items.map(item => {
        const cleaned = {};

        // Always include these core fields
        if (item.title) cleaned.title = item.title;

        // Always include artist field (even if empty, for consistency)
        cleaned.artist = item.artist || '';

        if (item.source_type) cleaned.source_type = item.source_type;
        if (item.path) cleaned.path = item.path;

        // Optional fields - only include if they have values
        if (item.url) cleaned.url = item.url;
        if (item.start !== null && item.start !== undefined) cleaned.start = item.start;
        if (item.end !== null && item.end !== undefined) cleaned.end = item.end;
        if (item.tags && item.tags.length > 0) cleaned.tags = item.tags;
        if (item.emulator_core) cleaned.emulator_core = item.emulator_core;
        if (item.emulator_system) cleaned.emulator_system = item.emulator_system;

        return cleaned;
    });
}

function cancelEdit(type) {
    try {
        const prefix = type === 'game' ? 'game' : 'video';

        // Clear form
        const titleInput = document.getElementById(`${prefix}PlaylistTitle`);
        if (titleInput) titleInput.value = '';

        const curatorInput = document.getElementById(`${prefix}PlaylistCurator`);
        if (curatorInput) curatorInput.value = '';

        const descInput = document.getElementById(`${prefix}PlaylistDesc`);
        if (descInput) descInput.value = '';

        const loopInput = document.getElementById(`${prefix}PlaylistLoop`);
        if (loopInput) loopInput.checked = false;

        // Clear items using unified config
        const config = MEDIA_CONFIG[type];
        config.setItems([]);
        renderPlaylistItems(type);

        // Hide cancel button
        const cancelBtn = document.getElementById(`cancel${prefix.charAt(0).toUpperCase() + prefix.slice(1)}PlaylistEdit`);
        if (cancelBtn) cancelBtn.style.display = 'none';

        // Remove editing file reference
        const saveBtn = document.getElementById(`save${prefix.charAt(0).toUpperCase() + prefix.slice(1)}Playlist`);
        if (saveBtn) delete saveBtn.dataset.editingFile;

        // Switch back to playlists view
        switchContentView(type, 'playlists');
    } catch (e) {
        console.error('Error in cancelEdit:', e);
    }
}

// ===== PLAYLIST BUILDER =====

function renderVideoPlaylistAvailable() {
    renderLibraryPanel();  // Use new library panel styling
}

function renderGamePlaylistAvailable() {
    renderPlaylistAvailable('game');
}

function handleDragStart(event, playlistType) {
    const type = event.target.dataset.type;
    const index = parseInt(event.target.dataset.index);

    if (type === 'video') {
        draggedItem = {
            type: 'local',
            data: availableVideos[index],
            playlistType: playlistType
        };
    } else if (type === 'rom') {
        const system = event.target.dataset.system;
        const rom = availableROMs[system][index];
        draggedItem = {
            type: 'emulated_game',
            data: rom,
            system: system,
            playlistType: playlistType
        };
    }

    event.target.classList.add('dragging');
}

function handleDragEnd(event) {
    event.target.classList.remove('dragging');
}

function renderVideoPlaylistItems() {
    renderPlaylistItems('video');
}

function renderGamePlaylistItems() {
    renderPlaylistItems('game');
}

function removePlaylistItem(index, type) {
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    items.splice(index, 1);
    config.setItems(items);
    renderPlaylistItems(type);
    // Use new library panel for videos
    if (type === 'video') {
        renderLibraryPanel();
    } else {
        renderPlaylistAvailable(type);
    }
}

function editPlaylistItem(index, type) {
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    const item = items[index];

    // Instead of prompt, make the fields inline-editable
    // Find the playlist item element
    const container = document.getElementById(config.playlistItemsId);
    const playlistItems = container.querySelectorAll('.playlist-item');
    const itemElement = playlistItems[index];

    if (!itemElement) return;

    // Find the title and artist spans
    const titleSpan = itemElement.querySelector('.item-title');
    const artistSpan = itemElement.querySelector('.item-artist');

    if (!titleSpan) return;

    // Get current values
    const currentTitle = item.title || item.path?.split('/').pop() || '';
    const currentArtist = item.artist || '';

    // Replace with input fields
    const titleInput = document.createElement('input');
    titleInput.type = 'text';
    titleInput.value = currentTitle;
    titleInput.className = 'edit-input';
    titleInput.style.cssText = 'background: var(--bg-dark); border: 1px solid var(--accent); color: var(--text-primary); padding: 0.25rem; border-radius: 4px; font-size: 0.9rem; width: 200px;';

    const artistInput = document.createElement('input');
    artistInput.type = 'text';
    artistInput.value = currentArtist;
    artistInput.placeholder = 'Artist (optional)';
    artistInput.className = 'edit-input';
    artistInput.style.cssText = 'background: var(--bg-dark); border: 1px solid var(--accent2); color: var(--text-secondary); padding: 0.25rem; border-radius: 4px; font-size: 0.85rem; margin-left: 0.5rem; width: 150px;';

    // Create save/cancel buttons
    const saveBtn = document.createElement('button');
    saveBtn.textContent = '✓ Save';
    saveBtn.className = 'btn-edit';
    saveBtn.style.cssText = 'margin-left: 0.5rem; background: var(--success); padding: 0.25rem 0.5rem;';

    const cancelBtn = document.createElement('button');
    cancelBtn.textContent = '✕';
    cancelBtn.className = 'btn-remove';
    cancelBtn.style.cssText = 'margin-left: 0.25rem; padding: 0.25rem 0.5rem;';

    // Save handler
    const saveEdit = () => {
        const newTitle = titleInput.value.trim();
        const newArtist = artistInput.value.trim();

        if (newTitle) {
            items[index].title = newTitle;
        }
        items[index].artist = newArtist;

        // Re-render
        if (type === 'game') {
            renderGamePlaylistItems();
            renderGamePlaylistAvailable();
        } else {
            renderVideoPlaylistItems();
            renderVideoPlaylistAvailable();
        }
    };

    // Cancel handler
    const cancelEdit = () => {
        if (type === 'game') {
            renderGamePlaylistItems();
        } else {
            renderVideoPlaylistItems();
        }
    };

    saveBtn.addEventListener('click', saveEdit);
    cancelBtn.addEventListener('click', cancelEdit);

    // Allow Enter to save, Escape to cancel
    const handleKey = (e) => {
        if (e.key === 'Enter') saveEdit();
        if (e.key === 'Escape') cancelEdit();
    };
    titleInput.addEventListener('keydown', handleKey);
    artistInput.addEventListener('keydown', handleKey);

    // Replace the spans with inputs
    const infoDiv = itemElement.querySelector('.playlist-item-info');
    infoDiv.innerHTML = '';
    infoDiv.appendChild(titleInput);
    infoDiv.appendChild(artistInput);
    infoDiv.appendChild(saveBtn);
    infoDiv.appendChild(cancelBtn);

    // Focus the title input
    titleInput.focus();
    titleInput.select();
}

// Setup drop zones for both playlist panels
document.addEventListener('DOMContentLoaded', () => {
    setupPlaylistDropZone('video');
    setupPlaylistDropZone('game');
});

function setupPlaylistDropZone(type) {
    const panelId = MEDIA_CONFIG[type].playlistItemsId;
    const panel = document.getElementById(panelId);

    if (panel) {
        panel.addEventListener('dragover', (e) => {
            e.preventDefault();
            panel.style.background = 'var(--bg-mid)';
        });

        panel.addEventListener('dragleave', (e) => {
            panel.style.background = '';
        });

        panel.addEventListener('drop', (e) => {
            e.preventDefault();
            panel.style.background = '';

            if (draggedItem && draggedItem.playlistType === type) {
                addItemToPlaylist(draggedItem, type);
                draggedItem = null;
            }
        });
    }
}

function addItemToPlaylist(draggedItem, type) {
    let item;

    if (draggedItem.type === 'local') {
        const video = draggedItem.data;
        // Only include fields that have values (matches YAML format)
        item = {
            title: video.filename,
            artist: '',  // Empty artist field, user can edit later
            source_type: 'local',
            path: video.path
        };
    } else if (draggedItem.type === 'emulated_game') {
        const rom = draggedItem.data;
        const system = draggedItem.system;

        // Auto-detect emulator core (using 64-bit compatible cores)
        const coreMap = {
            'nes': 'nestopia_libretro',
            'snes': 'snes9x2010_libretro',
            'genesis': 'genesis_plus_gx_libretro',
            'ps1': 'pcsx_rearmed_libretro',
            'atari7800': 'prosystem_libretro',
            'pcengine': 'mednafen_pce_fast_libretro',
            'arcade': 'fbneo_libretro'
        };

        // Only include fields that have values (matches YAML format)
        item = {
            title: rom.filename,
            artist: '',  // Empty artist field (games don't typically have artists)
            source_type: 'emulated_game',
            path: rom.path,
            emulator_core: coreMap[system] || 'auto',
            emulator_system: system // Keep lowercase for consistency
        };
    }

    // Add to appropriate playlist using unified config
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    items.push(item);
    config.setItems(items);
    renderPlaylistItems(type);

    // Refresh available items using correct renderer
    if (type === 'video') {
        renderVideoPlaylistAvailable();
    } else {
        renderPlaylistAvailable(type);
    }
}

// Playlist item reordering
let draggedPlaylistIndex = null;
let draggedPlaylistType = null;

function handlePlaylistDragStart(event, type) {
    draggedPlaylistIndex = parseInt(event.target.dataset.index);
    draggedPlaylistType = type;
    event.target.classList.add('dragging');
}

function handlePlaylistDragOver(event) {
    event.preventDefault();
}

function handlePlaylistDrop(event, type) {
    event.preventDefault();
    const dropIndex = parseInt(event.target.closest('.playlist-item').dataset.index);

    if (draggedPlaylistIndex !== null && draggedPlaylistIndex !== dropIndex && draggedPlaylistType === type) {
        const config = MEDIA_CONFIG[type];
        const items = config.getItems();
        const item = items[draggedPlaylistIndex];
        items.splice(draggedPlaylistIndex, 1);
        items.splice(dropIndex, 0, item);
        config.setItems(items);
        renderPlaylistItems(type);
    }

    draggedPlaylistIndex = null;
    draggedPlaylistType = null;
}

// ===== DRAG AND DROP FOR FILE UPLOADS =====

function setupDragAndDrop(inputId) {
    const input = document.getElementById(inputId);
    const label = input.nextElementSibling;

    ['dragenter', 'dragover'].forEach(event => {
        label.addEventListener(event, (e) => {
            e.preventDefault();
            label.classList.add('drag-over');
        });
    });

    ['dragleave', 'drop'].forEach(event => {
        label.addEventListener(event, (e) => {
            label.classList.remove('drag-over');
        });
    });

    label.addEventListener('drop', (e) => {
        e.preventDefault();
        if (e.dataTransfer.files.length) {
            input.files = e.dataTransfer.files;

            if (inputId === 'videoUpload') {
                uploadVideos();
            } else if (inputId === 'romUpload') {
                uploadROMs();
            }
        }
    });
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

// Helper to safely escape strings for HTML attributes
function escapeHtml(str) {
    if (!str) return '';
    return str
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#039;");
}

// Helper to escape strings for JavaScript string literals in onclick handlers
function escapeJs(str) {
    if (!str) return '';
    return str.replace(/\\/g, '\\\\').replace(/'/g, "\\'");
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

async function deleteVideo(path, filename) {
    showConfirmModal(
        'Delete Video',
        `Are you sure you want to delete "${escapeHtml(filename)}"?`,
        async () => {
            try {
                // Encode the path components to handle special characters in URL
                const encodedPath = path.split('/').map(encodeURIComponent).join('/');

                const response = await fetch(`${currentDevice.url}/admin/media/${encodedPath}`, {
                    method: 'DELETE',
                    headers: getCsrfHeaders(false)
                });

                if (response.ok) {
                    loadVideos(); // Refresh list
                } else {
                    const err = await response.json();
                    alert(`Failed to delete: ${err.error || 'Unknown error'}`);
                }
            } catch (e) {
                console.error('Delete failed:', e);
                alert('Delete failed: ' + e.message);
            }
        }
    );
}

async function deleteROM(path, filename) {
    showConfirmModal(
        'Delete ROM',
        `Are you sure you want to delete "${escapeHtml(filename)}"?`,
        async () => {
            try {
                // Encode the path components to handle special characters in URL
                const encodedPath = path.split('/').map(encodeURIComponent).join('/');

                const response = await fetch(`${currentDevice.url}/admin/roms/${encodedPath}`, {
                    method: 'DELETE',
                    headers: getCsrfHeaders(false)
                });

                if (response.ok) {
                    loadROMs(); // Refresh list
                } else {
                    const err = await response.json();
                    alert(`Failed to delete: ${err.error || 'Unknown error'}`);
                }
            } catch (e) {
                console.error('Delete failed:', e);
                alert('Delete failed: ' + e.message);
            }
        }
    );
}

async function deletePlaylist(filename, type) {
    showConfirmModal(
        'Delete Playlist',
        `Delete playlist "${filename}"?`,
        async () => {
            if (!currentDevice) return;

            try {
                await fetch(`${currentDevice.url}/admin/playlists/${filename}`, {
                    method: 'DELETE',
                    headers: getCsrfHeaders(false)
                });
                await loadExistingPlaylists();
            } catch (e) {
                console.error('Failed to delete playlist:', e);
                alert('Failed to delete playlist');
            }
        }
    );
}

// ===== TOUCH DRAG-AND-DROP SUPPORT =====

function setupTouchHandlers(type) {
    // Add touch handlers to content items in the specific panel
    const containerId = type === 'game' ? 'gamePlaylistAvailable' : 'videoPlaylistAvailable';
    const container = document.getElementById(containerId);
    if (!container) return;

    const contentItems = container.querySelectorAll('.content-item');

    contentItems.forEach(item => {
        item.addEventListener('touchstart', (e) => handleContentTouchStart(e, type), { passive: false });
        item.addEventListener('touchmove', (e) => handleContentTouchMove(e, type), { passive: false });
        item.addEventListener('touchend', (e) => handleContentTouchEnd(e, type), { passive: false });
    });
}

function handleContentTouchStart(e, playlistType) {
    const item = e.currentTarget;

    // Start long press timer
    longPressTimer = setTimeout(() => {
        // Long press detected - start drag
        isDragging = true;
        touchDragElement = item;

        // Store the dragged item data
        const type = item.dataset.type;
        const index = parseInt(item.dataset.index);

        if (type === 'video') {
            draggedItem = {
                type: 'local',
                data: availableVideos[index],
                playlistType: playlistType
            };
        } else if (type === 'rom') {
            const system = item.dataset.system;
            const rom = availableROMs[system][index];
            draggedItem = {
                type: 'emulated_game',
                data: rom,
                system: system,
                playlistType: playlistType
            };
        }

        // Visual feedback
        item.style.opacity = '0.5';
        item.style.transform = 'scale(1.05)';

        // Haptic feedback if available
        if (navigator.vibrate) {
            navigator.vibrate(50);
        }

        // Create a floating clone
        touchDragClone = item.cloneNode(true);
        touchDragClone.style.position = 'fixed';
        touchDragClone.style.pointerEvents = 'none';
        touchDragClone.style.zIndex = '10000';
        touchDragClone.style.opacity = '0.8';
        touchDragClone.style.transform = 'scale(1.1)';
        touchDragClone.style.boxShadow = '0 10px 30px rgba(255, 64, 160, 0.5)';

        const touch = e.touches[0];
        touchDragClone.style.left = (touch.pageX - 50) + 'px';
        touchDragClone.style.top = (touch.pageY - 20) + 'px';
        touchDragClone.style.width = item.offsetWidth + 'px';

        document.body.appendChild(touchDragClone);

    }, 300); // 300ms long press

    const touch = e.touches[0];
    touchStartX = touch.pageX;
    touchStartY = touch.pageY;
}

function handleContentTouchMove(e, playlistType) {
    if (longPressTimer && !isDragging) {
        // Check if user moved too much - cancel long press
        const touch = e.touches[0];
        const deltaX = Math.abs(touch.pageX - touchStartX);
        const deltaY = Math.abs(touch.pageY - touchStartY);

        if (deltaX > 10 || deltaY > 10) {
            clearTimeout(longPressTimer);
            longPressTimer = null;
        }
        return;
    }

    if (isDragging && touchDragClone) {
        e.preventDefault();

        const touch = e.touches[0];
        touchDragClone.style.left = (touch.pageX - 50) + 'px';
        touchDragClone.style.top = (touch.pageY - 20) + 'px';

        // Check if over the correct playlist panel
        const panelId = MEDIA_CONFIG[playlistType].playlistItemsId;
        const playlistPanel = document.getElementById(panelId);
        if (!playlistPanel) return;

        const rect = playlistPanel.getBoundingClientRect();

        if (touch.pageX >= rect.left && touch.pageX <= rect.right &&
            touch.pageY >= rect.top && touch.pageY <= rect.bottom) {
            playlistPanel.style.background = 'var(--bg-mid)';
            playlistPanel.style.borderColor = 'var(--accent2)';
        } else {
            playlistPanel.style.background = '';
            playlistPanel.style.borderColor = '';
        }
    }
}

function handleContentTouchEnd(e, playlistType) {
    if (longPressTimer) {
        clearTimeout(longPressTimer);
        longPressTimer = null;
    }

    if (isDragging && touchDragElement) {
        e.preventDefault();

        // Reset visual state
        touchDragElement.style.opacity = '';
        touchDragElement.style.transform = '';

        // Check if dropped on correct playlist panel
        const touch = e.changedTouches[0];
        const panelId = MEDIA_CONFIG[playlistType].playlistItemsId;
        const playlistPanel = document.getElementById(panelId);

        if (playlistPanel) {
            const rect = playlistPanel.getBoundingClientRect();

            if (touch.pageX >= rect.left && touch.pageX <= rect.right &&
                touch.pageY >= rect.top && touch.pageY <= rect.bottom) {
                // Add to playlist!
                if (draggedItem && draggedItem.playlistType === playlistType) {
                    addItemToPlaylist(draggedItem, playlistType);

                    // Haptic feedback
                    if (navigator.vibrate) {
                        navigator.vibrate([50, 50, 50]);
                    }
                }
            }

            // Reset panel style
            playlistPanel.style.background = '';
            playlistPanel.style.borderColor = '';
        }

        // Remove clone
        if (touchDragClone) {
            touchDragClone.remove();
            touchDragClone = null;
        }

        // Reset state
        isDragging = false;
        touchDragElement = null;
        draggedItem = null;
    }
}

function setupPlaylistTouchHandlers(type) {
    // Add touch handlers for reordering playlist items in the specific panel
    const panelId = MEDIA_CONFIG[type].playlistItemsId;
    const panel = document.getElementById(panelId);
    if (!panel) return;

    const playlistItems = panel.querySelectorAll('.playlist-item');

    playlistItems.forEach(item => {
        item.addEventListener('touchstart', (e) => handlePlaylistTouchStart(e, type), { passive: false });
        item.addEventListener('touchmove', (e) => handlePlaylistTouchMove(e, type), { passive: false });
        item.addEventListener('touchend', (e) => handlePlaylistTouchEnd(e, type), { passive: false });
    });
}

let touchReorderStartIndex = null;
let touchReorderCurrentIndex = null;
let touchReorderType = null;

function handlePlaylistTouchStart(e, type) {
    // Don't start drag if tapping remove or edit buttons
    if (e.target.classList.contains('btn-remove') || e.target.classList.contains('btn-edit')) {
        return;
    }

    const item = e.currentTarget;

    // Start long press timer for reordering
    longPressTimer = setTimeout(() => {
        isDragging = true;
        touchDragElement = item;
        touchReorderStartIndex = parseInt(item.dataset.index);
        touchReorderCurrentIndex = touchReorderStartIndex;
        touchReorderType = type;

        // Visual feedback
        item.style.opacity = '0.5';
        item.classList.add('dragging');

        // Haptic feedback
        if (navigator.vibrate) {
            navigator.vibrate(50);
        }

        // Create floating clone
        touchDragClone = item.cloneNode(true);
        touchDragClone.style.position = 'fixed';
        touchDragClone.style.pointerEvents = 'none';
        touchDragClone.style.zIndex = '10000';
        touchDragClone.style.opacity = '0.9';
        touchDragClone.style.boxShadow = '0 10px 30px rgba(64, 255, 200, 0.5)';

        const touch = e.touches[0];
        touchDragClone.style.left = (touch.pageX - item.offsetWidth / 2) + 'px';
        touchDragClone.style.top = (touch.pageY - item.offsetHeight / 2) + 'px';
        touchDragClone.style.width = item.offsetWidth + 'px';

        document.body.appendChild(touchDragClone);

    }, 300);

    const touch = e.touches[0];
    touchStartX = touch.pageX;
    touchStartY = touch.pageY;
}

function handlePlaylistTouchMove(e, type) {
    if (longPressTimer && !isDragging) {
        const touch = e.touches[0];
        const deltaX = Math.abs(touch.pageX - touchStartX);
        const deltaY = Math.abs(touch.pageY - touchStartY);

        if (deltaX > 10 || deltaY > 10) {
            clearTimeout(longPressTimer);
            longPressTimer = null;
        }
        return;
    }

    if (isDragging && touchDragClone) {
        e.preventDefault();

        const touch = e.touches[0];
        touchDragClone.style.left = (touch.pageX - touchDragClone.offsetWidth / 2) + 'px';
        touchDragClone.style.top = (touch.pageY - touchDragClone.offsetHeight / 2) + 'px';

        // Find which playlist item we're over (in the correct panel)
        const panelId = MEDIA_CONFIG[type].playlistItemsId;
        const panel = document.getElementById(panelId);
        if (!panel) return;

        const playlistItemsElements = panel.querySelectorAll('.playlist-item');

        playlistItemsElements.forEach(item => {
            const rect = item.getBoundingClientRect();

            if (touch.pageY >= rect.top && touch.pageY <= rect.bottom) {
                const overIndex = parseInt(item.dataset.index);

                if (overIndex !== touchReorderCurrentIndex && overIndex !== touchReorderStartIndex) {
                    // Highlight potential drop position
                    item.style.borderTop = '3px solid var(--accent2)';
                } else {
                    item.style.borderTop = '';
                }

                touchReorderCurrentIndex = overIndex;
            } else {
                item.style.borderTop = '';
            }
        });
    }
}

function handlePlaylistTouchEnd(e, type) {
    if (longPressTimer) {
        clearTimeout(longPressTimer);
        longPressTimer = null;
    }

    if (isDragging && touchDragElement) {
        e.preventDefault();

        // Perform reorder if position changed
        if (touchReorderStartIndex !== null && touchReorderCurrentIndex !== null &&
            touchReorderStartIndex !== touchReorderCurrentIndex && touchReorderType === type) {

            const config = MEDIA_CONFIG[type];
            const items = config.getItems();
            const item = items[touchReorderStartIndex];
            items.splice(touchReorderStartIndex, 1);
            items.splice(touchReorderCurrentIndex, 0, item);
            config.setItems(items);

            // Haptic feedback
            if (navigator.vibrate) {
                navigator.vibrate([50, 50, 50]);
            }

            // Re-render the playlist
            renderPlaylistItems(type);
        }

        // Remove clone
        if (touchDragClone) {
            touchDragClone.remove();
            touchDragClone = null;
        }

        // Reset state
        isDragging = false;
        touchDragElement = null;
        touchReorderStartIndex = null;
        touchReorderCurrentIndex = null;
        touchReorderType = null;

        // Clear any border highlights (in the correct panel)
        const panelId = MEDIA_CONFIG[type].playlistItemsId;
        const panel = document.getElementById(panelId);
        if (panel) {
            panel.querySelectorAll('.playlist-item').forEach(item => {
                item.style.opacity = '';
                item.style.borderTop = '';
                item.classList.remove('dragging');
            });
        }
    }
}

// ===== BACKUP & RESTORE FUNCTIONS =====

/**
 * Download a backup of all device data (playlists, settings, device info)
 */
function downloadBackup() {
    if (!currentDevice) {
        alert('No device connected');
        return;
    }

    // Create a temporary link to download the backup
    const backupUrl = `${currentDevice.url}/admin/backup`;
    const link = document.createElement('a');
    link.href = backupUrl;
    link.download = ''; // Let the server set the filename
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}

/**
 * Handle restore file upload
 * @param {HTMLInputElement} input - The file input element
 */
async function handleRestoreUpload(input) {
    if (!currentDevice) {
        alert('No device connected');
        input.value = '';
        return;
    }

    const file = input.files[0];
    if (!file) return;

    // Validate file extension
    if (!file.name.toLowerCase().endsWith('.zip')) {
        showRestoreStatus('error', 'Please select a valid backup file (.zip)');
        input.value = '';
        return;
    }

    // Confirm restore action
    const confirmed = confirm(
        `Are you sure you want to restore from "${escapeHtml(file.name)}"?\n\n` +
        'This will overwrite existing playlists, settings, and device info.'
    );
    if (!confirmed) {
        input.value = '';
        return;
    }

    showRestoreStatus('loading', 'Restoring backup...');

    try {
        const formData = new FormData();
        formData.append('file', file);

        const response = await fetch(`${currentDevice.url}/admin/restore`, {
            method: 'POST',
            headers: {
                'X-CSRF-Token': csrfToken
            },
            body: formData
        });

        const result = await response.json();

        if (result.ok) {
            let message = result.message || 'Backup restored successfully';
            if (result.data?.warnings && result.data.warnings.length > 0) {
                message += `\n\nWarnings:\n${result.data.warnings.join('\n')}`;
            }
            showRestoreStatus('success', message);

            // Reload content to reflect restored data
            await loadAllContentFromDevice();
        } else {
            showRestoreStatus('error', result.error?.message || 'Failed to restore backup');
        }
    } catch (e) {
        console.error('Restore failed:', e);
        showRestoreStatus('error', `Restore failed: ${e.message}`);
    } finally {
        input.value = '';
    }
}

/**
 * Show restore status message
 * @param {string} type - 'success', 'error', or 'loading'
 * @param {string} message - The message to display
 */
function showRestoreStatus(type, message) {
    const statusEl = document.getElementById('restoreStatus');
    if (!statusEl) return;

    statusEl.textContent = message;
    statusEl.className = 'restore-status visible ' + type;

    // Auto-hide success/error messages after 5 seconds
    if (type !== 'loading') {
        setTimeout(() => {
            statusEl.classList.remove('visible');
        }, 5000);
    }
}

// ===== HEALTH MONITORING FUNCTIONS =====

/**
 * Refresh and display system health information
 */
async function refreshHealthInfo() {
    if (!currentDevice) return;

    const healthEl = document.getElementById('healthInfo');
    if (!healthEl) return;

    healthEl.innerHTML = '<span class="loading">Loading...</span>';

    try {
        const data = await apiGet(`${currentDevice.url}/admin/health/detailed`);
        displayHealthInfo(data);
    } catch (e) {
        console.error('Health check failed:', e);
        healthEl.innerHTML = `<span class="loading">Failed to load health info</span>`;
    }
}

/**
 * Display system health information
 * @param {object} data - Health data from the API
 */
function displayHealthInfo(data) {
    const healthEl = document.getElementById('healthInfo');
    if (!healthEl) return;

    let html = '';

    // Status row
    const statusClass = data.status === 'healthy' ? 'good' :
        data.status === 'warning' ? 'warning' : 'error';
    html += `<div class="health-row">
        <span class="health-label">Status</span>
        <span class="health-value ${statusClass}">${escapeHtml(data.status || 'Unknown')}</span>
    </div>`;

    // CPU Temperature
    if (data.cpu_temperature_c !== undefined) {
        const tempClass = data.cpu_temperature_c > 75 ? 'warning' :
            data.cpu_temperature_c > 80 ? 'error' : 'good';
        html += `<div class="health-row">
            <span class="health-label">CPU Temp</span>
            <span class="health-value ${tempClass}">${data.cpu_temperature_c.toFixed(1)}°C</span>
        </div>`;
    }

    // CPU Usage
    if (data.cpu_percent !== undefined) {
        const cpuClass = data.cpu_percent > 90 ? 'error' :
            data.cpu_percent > 70 ? 'warning' : 'good';
        html += `<div class="health-row">
            <span class="health-label">CPU Usage</span>
            <span class="health-value ${cpuClass}">${data.cpu_percent.toFixed(1)}%</span>
        </div>`;
    }

    // Memory
    if (data.memory) {
        const memClass = data.memory.percent > 90 ? 'error' :
            data.memory.percent > 80 ? 'warning' : 'good';
        html += `<div class="health-row">
            <span class="health-label">Memory</span>
            <span class="health-value ${memClass}">${data.memory.percent.toFixed(1)}% (${data.memory.used_mb.toFixed(0)} / ${data.memory.total_mb.toFixed(0)} MB)</span>
        </div>`;
    }

    // Disk
    if (data.disk) {
        const diskClass = data.disk.percent > 90 ? 'error' :
            data.disk.percent > 80 ? 'warning' : 'good';
        html += `<div class="health-row">
            <span class="health-label">Disk</span>
            <span class="health-value ${diskClass}">${data.disk.percent.toFixed(1)}% (${data.disk.free_gb.toFixed(1)} GB free)</span>
        </div>`;
    }

    // Uptime
    if (data.uptime_human) {
        html += `<div class="health-row">
            <span class="health-label">Uptime</span>
            <span class="health-value">${escapeHtml(data.uptime_human)}</span>
        </div>`;
    }

    // App Service Status
    if (data.app_service) {
        const serviceClass = data.app_service === 'active' ? 'good' : 'error';
        html += `<div class="health-row">
            <span class="health-label">App Service</span>
            <span class="health-value ${serviceClass}">${escapeHtml(data.app_service)}</span>
        </div>`;
    }

    // Content stats
    if (data.content) {
        html += `<div class="health-row">
            <span class="health-label">Content</span>
            <span class="health-value">${data.content.playlists} playlists, ${data.content.videos} videos, ${data.content.roms} ROMs</span>
        </div>`;
    }

    healthEl.innerHTML = html;
}

/**
 * Show the settings section (called when device connects)
 */
function showSettingsSection() {
    const section = document.getElementById('settingsSectionWrapper');
    if (section) {
        section.style.display = 'block';
    }
}

/**
 * Hide the settings section (called when device disconnects)
 */
function hideSettingsSection() {
    const section = document.getElementById('settingsSectionWrapper');
    if (section) {
        section.style.display = 'none';
    }
}

// ===== NEW PLAYLIST EDITOR FUNCTIONS =====

/**
 * Update a playlist item's property (inline editing)
 */
function updatePlaylistItem(index, property, value, type) {
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    if (items[index]) {
        items[index][property] = value;
        config.setItems(items);
    }
}

/**
 * Move a playlist item up or down
 */
function movePlaylistItem(index, direction, type) {
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    const newIndex = index + direction;

    if (newIndex < 0 || newIndex >= items.length) return;

    // Swap items
    [items[index], items[newIndex]] = [items[newIndex], items[index]];
    config.setItems(items);
    renderPlaylistItems(type);
}

/**
 * Add an empty slot to the playlist
 */
function addEmptySlot(type = 'video') {
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    items.push({ isEmpty: true });
    config.setItems(items);
    renderPlaylistItems(type);
}

/**
 * Render the library panel with draggable video items
 */
function renderLibraryPanel() {
    const container = document.getElementById('libraryList');
    const countEl = document.getElementById('libraryCount');
    const hideUsedCheckbox = document.getElementById('hideUsedVideos');
    const searchInput = document.getElementById('librarySearch');

    if (!container) return;

    const videos = availableVideos || [];
    // Normalize path for comparison
    const normalizePath = (p) => {
        if (!p) return '';
        let clean = p;
        clean = clean.replace(/^\//, ''); // Remove leading slash
        clean = clean.replace(/^\.\.\/+/, ''); // Remove ../ prefixes
        clean = clean.replace(/^(dev_)?data\//, ''); // Remove data/ prefix
        clean = clean.replace(/^media\//, ''); // Remove media/ prefix
        return clean;
    };

    const currentPlaylistItems = AppState.playlists.getItems('video');
    const currentPlaylistPaths = new Set(currentPlaylistItems.map(item => normalizePath(item.path)));

    // Update count
    if (countEl) countEl.textContent = videos.length;

    if (videos.length === 0) {
        container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No videos in library</p>';
        return;
    }

    const searchQuery = (searchInput?.value || '').toLowerCase();
    const hideUsed = hideUsedCheckbox?.checked || false;

    container.innerHTML = videos.map((video, index) => {
        const path = video.path || `media/${video.filename}`;
        const normalizedPath = normalizePath(path);

        // Check current playlist
        const inCurrentPlaylist = currentPlaylistPaths.has(normalizedPath);

        // Check ALL playlists via globalVideoUsageMap
        const inAnyPlaylist = !!globalVideoUsageMap[normalizedPath];

        // Combine both checks - in current playlist OR in any saved playlist
        const isUsed = inCurrentPlaylist;

        const name = video.filename;

        // Apply filters - use combined check for hide-used filter
        const matchesSearch = !searchQuery || name.toLowerCase().includes(searchQuery);
        const shouldShow = matchesSearch && (!hideUsed || !isUsed);

        if (!shouldShow) return '';

        return `
            <div class="library-item ${isUsed ? 'in-playlist' : ''}" 
                 draggable="true" 
                 data-index="${index}"
                 data-type="video"
                 data-path="${escapeHtml(path)}"
                 ondragstart="handleLibraryDragStart(event)"
                 ondragend="handleDragEnd(event)">
                <span class="video-icon">📹</span>
                <span class="video-name">${escapeHtml(name)}</span>
                <button class="add-btn" onclick="addVideoToPlaylist(${index})" title="Add to playlist">+</button>
            </div>
        `;
    }).join('');
}

/**
 * Handle drag start from library
 */
function handleLibraryDragStart(event) {
    const index = parseInt(event.target.dataset.index);
    const path = event.target.dataset.path;

    draggedItem = {
        type: 'local',
        data: availableVideos[index],
        path: path,
        source: 'library'
    };

    event.target.classList.add('dragging');
    event.dataTransfer.effectAllowed = 'copy';
}

/**
 * Add a video from library to playlist
 */
function addVideoToPlaylist(index) {
    const video = availableVideos[index];
    if (!video) return;

    const config = MEDIA_CONFIG['video'];
    const items = config.getItems();

    // Find first empty slot or add to end
    const emptySlotIndex = items.findIndex(item => item.isEmpty);

    const newItem = {
        source_type: 'local',
        path: video.path || `media/${video.filename}`,
        title: video.filename.replace(/\.[^.]+$/, ''),
        artist: ''
    };

    if (emptySlotIndex >= 0) {
        // Fill empty slot
        items[emptySlotIndex] = newItem;
    } else {
        // Add to end
        items.push(newItem);
    }

    config.setItems(items);
    renderPlaylistItems('video');
    renderLibraryPanel();
}

/**
 * Filter library based on search and checkbox
 */
function filterLibrary() {
    renderLibraryPanel();
}

/**
 * Toggle the upload section in library panel
 */
function toggleLibraryUpload() {
    const content = document.getElementById('libraryUploadContent');
    const icon = document.getElementById('uploadToggleIcon');

    if (content.style.display === 'none') {
        content.style.display = 'block';
        if (icon) icon.textContent = '▲';
    } else {
        content.style.display = 'none';
        if (icon) icon.textContent = '▼';
    }
}

/**
 * Toggle ROM upload section in library panel
 */
function toggleROMLibraryUpload() {
    const content = document.getElementById('romLibraryUploadContent');
    const icon = document.getElementById('romUploadToggleIcon');

    if (content.style.display === 'none') {
        content.style.display = 'block';
        if (icon) icon.textContent = '▲';
    } else {
        content.style.display = 'none';
        if (icon) icon.textContent = '▼';
    }
}

/**
 * Render the ROM library panel with draggable ROM items
 */
function renderROMLibraryPanel() {
    const container = document.getElementById('romLibraryList');
    const countEl = document.getElementById('romLibraryCount');
    const hideUsedCheckbox = document.getElementById('hideUsedROMs');
    const searchInput = document.getElementById('romLibrarySearch');

    if (!container) return;

    const allROMs = availableROMs || {};
    const currentPlaylistItems = AppState.playlists.getItems('game');

    // Normalize path for comparison - strips data/ and dev_data/ prefixes
    const normalizePath = (p) => {
        if (!p) return '';
        let clean = p;
        clean = clean.replace(/^\//, '');
        clean = clean.replace(/^\.\.\/+/, '');
        clean = clean.replace(/^(dev_)?data\//, '');
        clean = clean.replace(/^media\//, '');
        return clean;
    };

    const currentPlaylistPaths = new Set(currentPlaylistItems.map(item => normalizePath(item.path)));

    // Flatten ROMs from all systems
    let flatROMs = [];
    for (const [system, roms] of Object.entries(allROMs)) {
        roms.forEach((rom, index) => {
            flatROMs.push({
                ...rom,
                system,
                originalIndex: index
                // Use rom.path from server directly - don't override it
            });
        });
    }

    // Update count
    if (countEl) countEl.textContent = flatROMs.length;

    if (flatROMs.length === 0) {
        container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No ROMs in library</p>';
        return;
    }

    const searchQuery = (searchInput?.value || '').toLowerCase();
    const hideUsed = hideUsedCheckbox?.checked || false;

    const html = flatROMs.map((rom, index) => {
        const normalizedRomPath = normalizePath(rom.path);
        const inPlaylist = currentPlaylistPaths.has(normalizedRomPath);
        const name = rom.filename;

        // Apply filters
        const matchesSearch = !searchQuery || name.toLowerCase().includes(searchQuery);
        const shouldShow = matchesSearch && (!hideUsed || !inPlaylist);

        if (!shouldShow) return '';

        const systemLabel = rom.system.toUpperCase();

        return `
            <div class="library-item ${inPlaylist ? 'in-playlist' : ''}" 
                 draggable="true" 
                 data-index="${rom.originalIndex}"
                 data-system="${rom.system}"
                 data-type="rom"
                 data-path="${escapeHtml(rom.path)}"
                 ondragstart="handleROMLibraryDragStart(event)"
                 ondragend="handleDragEnd(event)">
                <span class="video-icon">🎮</span>
                <span class="video-name">${escapeHtml(name)}</span>
                <span style="font-size: 0.7rem; color: var(--text-secondary); margin-left: 0.5rem;">${systemLabel}</span>
                <button class="add-btn" onclick="addROMToPlaylist('${rom.system}', ${rom.originalIndex})" title="Add to playlist">+</button>
            </div>
        `;
    }).join('');

    container.innerHTML = html || '<p style="color: var(--text-secondary); padding: 1rem;">No ROMs match your filters</p>';
}

/**
 * Handle drag start from ROM library
 */
function handleROMLibraryDragStart(event) {
    const index = parseInt(event.target.dataset.index);
    const system = event.target.dataset.system;
    const path = event.target.dataset.path;

    const rom = availableROMs[system]?.[index];
    if (!rom) return;

    draggedItem = {
        type: 'emulated_game',
        data: rom,
        system: system,
        path: path,
        source: 'library'
    };

    event.target.classList.add('dragging');
    event.dataTransfer.effectAllowed = 'copy';
}

/**
 * Add a ROM from library to playlist
 */
function addROMToPlaylist(system, index) {
    const rom = availableROMs[system]?.[index];
    if (!rom) return;

    const config = MEDIA_CONFIG['game'];
    const items = config.getItems();

    // Find first empty slot or add to end
    const emptySlotIndex = items.findIndex(item => item.isEmpty);

    // Auto-detect emulator core (using 64-bit compatible cores)
    const coreMap = {
        'nes': 'nestopia_libretro',
        'snes': 'snes9x2010_libretro',
        'genesis': 'genesis_plus_gx_libretro',
        'ps1': 'pcsx_rearmed_libretro',
        'atari7800': 'prosystem_libretro',
        'pcengine': 'mednafen_pce_fast_libretro',
        'arcade': 'fbneo_libretro'
    };

    const newItem = {
        source_type: 'emulated_game',
        path: rom.path,  // Use actual path from server
        title: rom.filename.replace(/\.[^.]+$/, ''),
        emulator_system: system,
        emulator_core: coreMap[system] || 'auto'
    };

    if (emptySlotIndex >= 0) {
        items[emptySlotIndex] = newItem;
    } else {
        items.push(newItem);
    }

    config.setItems(items);
    renderPlaylistItems('game');
    renderROMLibraryPanel();
}

/**
 * Filter ROM library based on search and checkbox
 */
function filterROMLibrary() {
    renderROMLibraryPanel();
}

/**
 * Update renderGamePlaylistAvailable to use new ROM library panel
 */
function renderGamePlaylistAvailable() {
    renderROMLibraryPanel();  // Use new ROM library panel styling
}

// ===== OTA UPDATE FUNCTIONS =====

let updateData = null;  // Store update info between check and install

/**
 * Load current version on page load
 */
async function loadVersionInfo() {
    if (!currentDevice) return;

    const versionEl = document.getElementById('currentVersion');
    if (!versionEl) return;

    try {
        const data = await apiGet(`${currentDevice.url}/admin/update/version`);
        versionEl.textContent = `v${data.version}`;
    } catch (e) {
        console.error('Failed to load version:', e);
        versionEl.textContent = 'Unknown';
    }
}

/**
 * Check for available updates
 */
async function checkForUpdates() {
    if (!currentDevice) {
        alert('Not connected to a device');
        return;
    }

    const statusEl = document.getElementById('updateStatus');
    const availableEl = document.getElementById('updateAvailable');
    const checkBtn = document.getElementById('checkUpdateBtn');
    const installBtn = document.getElementById('installUpdateBtn');
    const rollbackBtn = document.getElementById('rollbackBtn');

    // Reset UI state
    statusEl.classList.remove('complete', 'error');
    if (statusEl) statusEl.style.display = 'block';
    if (availableEl) availableEl.style.display = 'none';
    if (installBtn) installBtn.style.display = 'none';
    if (checkBtn) checkBtn.disabled = true;

    const labelEl = statusEl?.querySelector('.update-label');
    const progressEl = statusEl?.querySelector('.update-progress-fill');
    const detailsEl = statusEl?.querySelector('.update-details');
    const iconEl = statusEl?.querySelector('.update-icon');

    if (labelEl) labelEl.textContent = 'Checking for updates...';
    if (progressEl) progressEl.style.width = '50%';
    if (iconEl) iconEl.textContent = '🔍';
    if (detailsEl) detailsEl.textContent = '';

    try {
        const response = await fetch(`${currentDevice.url}/admin/update/check`);
        const result = await response.json();

        if (!result.ok) {
            throw new Error(result.error?.message || 'Check failed');
        }

        const data = result.data;
        updateData = data;  // Store for install

        // Update current version display
        const versionEl = document.getElementById('currentVersion');
        if (versionEl) versionEl.textContent = `v${data.current_version}`;

        if (data.update_available) {
            // Show update available
            if (statusEl) statusEl.style.display = 'none';
            if (availableEl) availableEl.style.display = 'block';
            if (installBtn) installBtn.style.display = 'block';

            const newVersionLabel = document.getElementById('newVersionLabel');
            const releaseNotes = document.getElementById('releaseNotes');

            if (newVersionLabel) newVersionLabel.textContent = `v${data.latest_version}`;
            if (releaseNotes) releaseNotes.textContent = data.release_notes || 'No release notes available.';

            // Show rollback if backup exists
            if (rollbackBtn && data.has_backup) rollbackBtn.style.display = 'block';

        } else {
            // Already up to date
            if (labelEl) labelEl.textContent = 'Up to date!';
            if (progressEl) progressEl.style.width = '100%';
            if (detailsEl) detailsEl.textContent = `Current version: v${data.current_version}`;
            if (iconEl) iconEl.textContent = '✓';
            statusEl?.classList.add('complete');

            // Show rollback option if backup exists
            if (rollbackBtn && data.has_backup) rollbackBtn.style.display = 'block';

            // Hide after delay
            setTimeout(() => {
                if (statusEl) statusEl.style.display = 'none';
                statusEl?.classList.remove('complete');
            }, 3000);
        }

    } catch (e) {
        console.error('Update check failed:', e);
        if (labelEl) labelEl.textContent = 'Check failed';
        if (detailsEl) detailsEl.textContent = e.message;
        if (iconEl) iconEl.textContent = '✗';
        statusEl?.classList.add('error');

        setTimeout(() => {
            if (statusEl) statusEl.style.display = 'none';
            statusEl?.classList.remove('error');
        }, 5000);
    }

    if (checkBtn) checkBtn.disabled = false;
}

/**
 * Install an available update
 */
async function installUpdate() {
    if (!currentDevice || !updateData?.download_url) {
        alert('No update available to install');
        return;
    }

    if (!confirm(`Install update v${updateData.latest_version}?\n\nThis will restart the device services and may take several minutes.`)) {
        return;
    }

    const statusEl = document.getElementById('updateStatus');
    const availableEl = document.getElementById('updateAvailable');
    const checkBtn = document.getElementById('checkUpdateBtn');
    const installBtn = document.getElementById('installUpdateBtn');
    const rollbackBtn = document.getElementById('rollbackBtn');

    // Reset and show progress
    statusEl?.classList.remove('complete', 'error');
    if (statusEl) statusEl.style.display = 'block';
    if (availableEl) availableEl.style.display = 'none';
    if (checkBtn) checkBtn.disabled = true;
    if (installBtn) installBtn.disabled = true;
    if (rollbackBtn) rollbackBtn.style.display = 'none';

    const labelEl = statusEl?.querySelector('.update-label');
    const progressEl = statusEl?.querySelector('.update-progress-fill');
    const detailsEl = statusEl?.querySelector('.update-details');
    const iconEl = statusEl?.querySelector('.update-icon');

    if (labelEl) labelEl.textContent = 'Starting update...';
    if (progressEl) progressEl.style.width = '0%';
    if (iconEl) iconEl.textContent = '⬇️';

    // Polling timeout constants
    const MAX_POLL_TIME_MS = 45 * 60 * 1000;  // 45 minutes
    const pollStartTime = Date.now();

    try {
        // Start update
        const startResponse = await apiPost(`${currentDevice.url}/admin/update/install`, {
            version: updateData.latest_version,
            download_url: updateData.download_url
        });

        const jobId = startResponse.job_id;

        // Poll for progress
        const stageLabels = {
            'preparing': 'Preparing...',
            'downloading': 'Downloading update...',
            'extracting': 'Extracting files...',
            'backing_up': 'Creating backup...',
            'stopping_services': 'Stopping services...',
            'installing': 'Installing files...',
            'building': 'Building application...',
            'restarting_services': 'Restarting services...',
            'complete': 'Update complete!'
        };

        let notFoundCount = 0;  // Track consecutive 404s

        while (true) {
            // Check for overall timeout
            const elapsedTime = Date.now() - pollStartTime;
            if (elapsedTime > MAX_POLL_TIME_MS) {
                throw new Error('Update timed out after 45 minutes. Check device status manually.');
            }

            await new Promise(resolve => setTimeout(resolve, 1500));

            try {
                const statusResponse = await fetch(`${currentDevice.url}/admin/update/status/${jobId}`);
                const statusData = await statusResponse.json();

                // Handle 404 - job not found (happens after service restart)
                if (statusResponse.status === 404 || !statusData.ok) {
                    notFoundCount++;
                    console.log(`Status poll failed (attempt ${notFoundCount}):`, statusData);

                    // After a few 404s, check if update actually completed
                    if (notFoundCount >= 3) {
                        if (detailsEl) detailsEl.textContent = 'Service restarting, verifying...';

                        const verifyStartTime = Date.now();
                        const MAX_VERIFY_TIME_MS = 2 * 60 * 1000;  // 2 minute cap
                        const backoffDelays = [2000, 4000, 8000, 16000, 16000, 16000];
                        let verifyAttempt = 0;

                        while (Date.now() - verifyStartTime < MAX_VERIFY_TIME_MS) {
                            verifyAttempt++;
                            const delay = backoffDelays[Math.min(verifyAttempt - 1, backoffDelays.length - 1)];

                            if (detailsEl) {
                                const elapsed = Math.round((Date.now() - verifyStartTime) / 1000);
                                detailsEl.textContent = `Verifying update... (${elapsed}s)`;
                            }

                            await new Promise(resolve => setTimeout(resolve, delay));

                            try {
                                await fetchCsrfToken();
                                const versionCheck = await fetch(`${currentDevice.url}/admin/update/check`);
                                const versionData = await versionCheck.json();

                                console.log(`Verify attempt ${verifyAttempt}:`, versionData);

                                if (versionData.ok && versionData.data?.current_version === updateData.latest_version) {
                                    // Success!
                                    if (labelEl) labelEl.textContent = 'Update complete!';
                                    if (progressEl) progressEl.style.width = '100%';
                                    if (detailsEl) detailsEl.textContent = `Now running v${updateData.latest_version}`;
                                    if (iconEl) iconEl.textContent = '✓';
                                    statusEl?.classList.add('complete');

                                    const versionEl = document.getElementById('currentVersion');
                                    if (versionEl) versionEl.textContent = `v${updateData.latest_version}`;
                                    if (rollbackBtn) rollbackBtn.style.display = 'block';

                                    setTimeout(() => {
                                        alert('Update complete! Now running the latest version.');
                                    }, 1000);
                                    return;
                                }
                            } catch (verifyError) {
                                console.log(`Verify attempt ${verifyAttempt} failed:`, verifyError);
                            }
                        }

                        throw new Error('Could not verify update. Check device version manually.');
                    }
                    continue;
                }

                // Reset counter on successful poll
                notFoundCount = 0;
                const job = statusData.data;

                // Update UI
                if (labelEl) labelEl.textContent = stageLabels[job.stage] || job.stage;
                if (progressEl) progressEl.style.width = `${job.progress}%`;
                if (detailsEl && job.message) detailsEl.textContent = job.message;

                // Show elapsed time during build stage
                if (job.stage === 'building') {
                    const elapsedMinutes = Math.floor((Date.now() - pollStartTime) / 60000);
                    if (elapsedMinutes > 0 && detailsEl) {
                        detailsEl.textContent = `${job.message || 'Compiling...'} (${elapsedMinutes}m elapsed)`;
                    }
                }

                if (job.status === 'complete') {
                    if (iconEl) iconEl.textContent = '✓';
                    statusEl?.classList.add('complete');

                    // Update version display
                    const versionEl = document.getElementById('currentVersion');
                    if (versionEl && job.new_version) {
                        versionEl.textContent = `v${job.new_version}`;
                    }

                    // Show rollback button
                    if (rollbackBtn) rollbackBtn.style.display = 'block';

                    setTimeout(() => {
                        alert('Update complete! The device is now running the latest version.');
                    }, 1000);

                    break;
                }

                if (job.status === 'error') {
                    throw new Error(job.message || 'Update failed');
                }

            } catch (pollError) {
                // Network error - device might be restarting
                console.log('Polling error (may be normal during restart):', pollError);
                if (detailsEl) detailsEl.textContent = 'Reconnecting to device...';
                notFoundCount++;
            }
        }

    } catch (e) {
        console.error('Update failed:', e);
        if (labelEl) labelEl.textContent = 'Update failed';
        if (detailsEl) detailsEl.textContent = e.message;
        if (iconEl) iconEl.textContent = '✗';
        statusEl?.classList.add('error');

        if (rollbackBtn) rollbackBtn.style.display = 'block';
    }

    if (checkBtn) checkBtn.disabled = false;
    if (installBtn) installBtn.disabled = false;
}

/**
 * Rollback to previous version
 */
async function rollbackUpdate() {
    if (!currentDevice) {
        alert('Not connected to a device');
        return;
    }

    if (!confirm('Rollback to the previous version?\n\nThis will restart the device services.')) {
        return;
    }

    const statusEl = document.getElementById('updateStatus');
    const availableEl = document.getElementById('updateAvailable');
    const checkBtn = document.getElementById('checkUpdateBtn');
    const installBtn = document.getElementById('installUpdateBtn');
    const rollbackBtn = document.getElementById('rollbackBtn');

    // Reset and show progress
    statusEl?.classList.remove('complete', 'error');
    if (statusEl) statusEl.style.display = 'block';
    if (availableEl) availableEl.style.display = 'none';
    if (checkBtn) checkBtn.disabled = true;
    if (installBtn) installBtn.style.display = 'none';
    if (rollbackBtn) rollbackBtn.disabled = true;

    const labelEl = statusEl?.querySelector('.update-label');
    const progressEl = statusEl?.querySelector('.update-progress-fill');
    const detailsEl = statusEl?.querySelector('.update-details');
    const iconEl = statusEl?.querySelector('.update-icon');

    if (labelEl) labelEl.textContent = 'Rolling back...';
    if (progressEl) progressEl.style.width = '50%';
    if (iconEl) iconEl.textContent = '↩️';
    if (detailsEl) detailsEl.textContent = '';

    // Get current version before rollback to detect change
    let previousVersion = null;
    try {
        const checkResponse = await fetch(`${currentDevice.url}/admin/update/check`);
        const checkData = await checkResponse.json();
        if (checkData.ok) previousVersion = checkData.data?.current_version;
    } catch (e) {
        console.log('Could not get current version before rollback');
    }

    try {
        const response = await apiPost(`${currentDevice.url}/admin/update/rollback`, {});

        if (response.stage === 'complete' || response.ok) {
            if (labelEl) labelEl.textContent = 'Rollback complete!';
            if (progressEl) progressEl.style.width = '100%';
            if (iconEl) iconEl.textContent = '✓';
            statusEl?.classList.add('complete');

            // Update version display
            const versionEl = document.getElementById('currentVersion');
            if (versionEl && response.version) {
                versionEl.textContent = `v${response.version}`;
            }

            setTimeout(() => {
                if (statusEl) statusEl.style.display = 'none';
                statusEl?.classList.remove('complete');
                alert('Rollback complete! The device is now running the previous version.');
            }, 2000);
        }

    } catch (e) {
        console.error('Rollback error:', e);

        // Service restarts during rollback - wait and verify it worked
        if (detailsEl) detailsEl.textContent = 'Service restarting...';

        // Retry loop - service can take 10-15 seconds to restart
        let rollbackVerified = false;
        for (let attempt = 0; attempt < 6; attempt++) {
            if (detailsEl) detailsEl.textContent = `Waiting for service... (${attempt + 1}/6)`;
            await new Promise(resolve => setTimeout(resolve, 3000));

            try {
                await fetchCsrfToken();
                const checkResponse = await fetch(`${currentDevice.url}/admin/update/check`);
                const checkData = await checkResponse.json();

                console.log(`Verify attempt ${attempt + 1}:`, checkData);

                if (checkData.ok && previousVersion && checkData.data?.current_version !== previousVersion) {
                    // Version changed - rollback succeeded
                    if (labelEl) labelEl.textContent = 'Rollback complete!';
                    if (progressEl) progressEl.style.width = '100%';
                    if (iconEl) iconEl.textContent = '✓';
                    statusEl?.classList.add('complete');

                    const versionEl = document.getElementById('currentVersion');
                    if (versionEl) versionEl.textContent = `v${checkData.data.current_version}`;

                    setTimeout(() => {
                        if (statusEl) statusEl.style.display = 'none';
                        statusEl?.classList.remove('complete');
                        alert('Rollback complete! The device is now running the previous version.');
                    }, 2000);

                    if (checkBtn) checkBtn.disabled = false;
                    if (rollbackBtn) rollbackBtn.disabled = false;
                    rollbackVerified = true;
                    return;
                }
            } catch (verifyError) {
                console.log(`Verify attempt ${attempt + 1} failed:`, verifyError);
            }
        }

        // If we get here, rollback couldn't be verified
        if (labelEl) labelEl.textContent = 'Rollback status unknown';
        if (detailsEl) detailsEl.textContent = 'Refresh page to check current version';
        if (iconEl) iconEl.textContent = '?';
        statusEl?.classList.add('error');

        setTimeout(() => {
            if (statusEl) statusEl.style.display = 'none';
            statusEl?.classList.remove('error');
        }, 5000);
    }

    if (checkBtn) checkBtn.disabled = false;
    if (rollbackBtn) rollbackBtn.disabled = false;
}

// Add event listeners for "Add Empty Slot" buttons and filters
document.addEventListener('DOMContentLoaded', () => {
    // Video empty slot button
    const addSlotBtn = document.getElementById('addEmptySlot');
    if (addSlotBtn) {
        addSlotBtn.addEventListener('click', () => addEmptySlot('video'));
    }

    // Game empty slot button
    const addGameSlotBtn = document.getElementById('addGameEmptySlot');
    if (addGameSlotBtn) {
        addGameSlotBtn.addEventListener('click', () => addEmptySlot('game'));
    }

    // Library search filter (videos)
    const searchInput = document.getElementById('librarySearch');
    if (searchInput) {
        searchInput.addEventListener('input', filterLibrary);
    }

    // Hide used checkbox (videos)
    const hideUsedCheckbox = document.getElementById('hideUsedVideos');
    if (hideUsedCheckbox) {
        hideUsedCheckbox.addEventListener('change', filterLibrary);
    }

    // ROM library search filter
    const romSearchInput = document.getElementById('romLibrarySearch');
    if (romSearchInput) {
        romSearchInput.addEventListener('input', filterROMLibrary);
    }

    // Hide used checkbox (ROMs)
    const hideUsedROMsCheckbox = document.getElementById('hideUsedROMs');
    if (hideUsedROMsCheckbox) {
        hideUsedROMsCheckbox.addEventListener('change', filterROMLibrary);
    }
});
