// ===== GLOBAL STATE =====

let currentDevice = null;
let discoveredDevices = [];
let availableVideos = [];
let availableROMs = {};
let draggedItem = null;

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
    // Tab switching
    document.querySelectorAll('.tab').forEach(tab => {
        tab.addEventListener('click', () => {
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            tab.classList.add('active');
            const tabId = tab.dataset.tab;
            document.getElementById(tabId).classList.add('active');

            // Explicitly initialize the view when switching tabs
            if (tabId === 'roms') {
                switchGameView('playlists');
            } else if (tabId === 'videos') {
                switchVideoView('playlists');
            }
        });
    });

    // Manual connection
    document.getElementById('manualConnect').addEventListener('click', manualConnect);

    // File uploads
    document.getElementById('videoUpload').addEventListener('change', (e) => {
        if (e.target.files.length > 0) uploadVideos();
    });

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
}

// ===== DEVICE DISCOVERY =====

async function discoverDevices() {
    const statusText = document.getElementById('statusText');
    statusText.textContent = 'Searching for devices...';

    discoveredDevices = [];

    // 1. Always check current origin first (relative path)
    try {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 2000); // 2s timeout

        const response = await fetch('/admin/device/info', { signal: controller.signal });
        clearTimeout(timeout);

        if (response.ok) {
            const info = await response.json();
            info.url = window.location.origin;
            discoveredDevices.push(info);

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
            const info = await response.json();
            info.url = `http://${host}:${port}`;

            // Avoid duplicates
            if (!discoveredDevices.find(d => d.device_id === info.device_id)) {
                discoveredDevices.push(info);
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
                <h4>📺 ${device.device_name}</h4>
                <div class="meta">
                    ${device.local_ip} • ${device.hostname}
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
    const newDevice = discoveredDevices[index];
    const isSameDevice = currentDevice && currentDevice.device_id === newDevice.device_id;

    currentDevice = newDevice;

    document.getElementById('deviceStatus').classList.add('connected');
    document.getElementById('statusText').textContent =
        `Connected to ${currentDevice.device_name}`;

    // Update connection status in header
    const statusElement = document.getElementById('deviceConnectionStatus');
    if (statusElement) {
        statusElement.textContent = currentDevice.device_name;
        statusElement.classList.add('connected');
    }

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

async function manualConnect() {
    const ip = prompt('Enter device IP address and port (e.g., 192.168.1.100:8080):');
    if (!ip) return;

    try {
        const url = ip.startsWith('http') ? ip : `http://${ip}`;
        const response = await fetch(`${url}/admin/device/info`);

        if (response.ok) {
            const info = await response.json();
            info.url = url;

            if (!discoveredDevices.find(d => d.device_id === info.device_id)) {
                discoveredDevices.push(info);
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

    await loadVideos();
    await loadROMs();
    await loadExistingPlaylists();
    renderVideoPlaylistAvailable();
    renderGamePlaylistAvailable();
}

// ===== UI NAVIGATION =====

function switchVideoView(viewName) {
    // Update sub-tabs
    document.querySelectorAll('.sub-tab').forEach(tab => {
        tab.classList.remove('active');
        if (tab.textContent.toLowerCase().includes(viewName.replace('video', ''))) {
            tab.classList.add('active');
        }
        // Handle specific mapping
        if (viewName === 'playlists' && tab.textContent.includes('My Playlists')) tab.classList.add('active');
        if (viewName === 'editor' && tab.textContent.includes('Editor')) tab.classList.add('active');
        if (viewName === 'library' && tab.textContent.includes('All Videos')) tab.classList.add('active');
    });

    // Update views
    document.querySelectorAll('.sub-view').forEach(view => {
        view.style.display = 'none';
        view.classList.remove('active');
    });

    const viewId = `video${viewName.charAt(0).toUpperCase() + viewName.slice(1)}View`;
    const view = document.getElementById(viewId);
    if (view) {
        view.style.display = 'block';
        view.classList.add('active');
    }
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
    // Clear form
    document.getElementById('videoPlaylistTitle').value = '';
    document.getElementById('videoPlaylistCurator').value = '';
    document.getElementById('videoPlaylistDesc').value = '';
    document.getElementById('videoPlaylistLoop').checked = false;
    document.getElementById('editorTitle').textContent = 'New Playlist';

    // Clear items
    videoPlaylistItems = [];
    renderVideoPlaylistItems();

    // Remove editing file reference
    const saveBtn = document.getElementById('saveVideoPlaylist');
    if (saveBtn) delete saveBtn.dataset.editingFile;

    // Switch to editor
    switchVideoView('editor');
}

// ===== VIDEO MANAGEMENT =====

async function loadVideos() {
    if (!currentDevice) return;

    try {
        const response = await fetch(`${currentDevice.url}/admin/media`);
        availableVideos = await response.json();

        console.log(`Loaded ${availableVideos.length} videos from /admin/media`);

        // Update count badges
        const countBadges = document.querySelectorAll('#videoCount');
        countBadges.forEach(badge => badge.textContent = availableVideos.length);

        const container = document.getElementById('videoList');
        if (availableVideos.length === 0) {
            container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No videos uploaded yet</p>';
            return;
        }

        // Load all playlists to check video usage
        const playlistsResponse = await fetch(`${currentDevice.url}/admin/playlists`);
        const allPlaylists = await playlistsResponse.json();

        // Build a map of video paths to playlists that use them
        const videoUsageMap = {};
        for (const playlist of allPlaylists) {
            try {
                const detailResponse = await fetch(`${currentDevice.url}/admin/playlists/${playlist.filename}`);
                const fullData = await detailResponse.json();
                // Normalize path for comparison
                const normalizePath = (p) => {
                    if (!p) return '';
                    // Remove leading slash
                    let clean = p.replace(/^\//, '');
                    // Remove data/ or dev_data/ prefix to match base filename/path
                    clean = clean.replace(/^(dev_)?data\//, '');
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
                let clean = p.replace(/^\//, '');
                clean = clean.replace(/^(dev_)?data\//, '');
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
    if (input.files.length > 0) {
        await uploadVideos(input.files, true); // true = autoAddToPlaylist
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

    // Create progress bars for all files first
    const queue = [];
    for (const file of files) {
        const progressBar = createProgressBar(file.name);
        progressElement.appendChild(progressBar.element);
        queue.push({ file, progressBar });
    }

    // Process queue with concurrency limit
    const CONCURRENCY_LIMIT = 3;
    let activeUploads = 0;
    let currentIndex = 0;

    const processQueue = () => {
        while (activeUploads < CONCURRENCY_LIMIT && currentIndex < queue.length) {
            const item = queue[currentIndex++];
            activeUploads++;
            uploadSingleFile(item.file, item.progressBar, autoAddToPlaylist).finally(() => {
                activeUploads--;
                processQueue();
            });
        }
    };

    processQueue();
}

function uploadSingleFile(file, progressBar, autoAddToPlaylist) {
    return new Promise((resolve, reject) => {
        const formData = new FormData();
        formData.append('file', file);

        try {
            const xhr = new XMLHttpRequest();

            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    progressBar.update(percent);
                }
            });

            xhr.addEventListener('load', () => {
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
                                path: response.path || `data/media/${file.name}` // Use server path if available
                            };

                            // Add directly to playlist items array
                            videoPlaylistItems.push(videoItem);
                            renderVideoPlaylistItems();

                            // Switch back to library tab to show it's available
                            setTimeout(() => {
                                switchSourceTab('library');
                            }, 1000);
                        }
                    } catch (e) {
                        console.error('Error parsing upload response:', e);
                    }

                    // Trigger reload but don't wait for it
                    setTimeout(() => loadVideos(), 500);

                    resolve();
                } else {
                    progressBar.error();
                    resolve(); // Resolve anyway to continue queue
                }
            });

            xhr.addEventListener('error', () => {
                progressBar.error();
                resolve();
            });

            xhr.open('POST', `${currentDevice.url}/admin/upload`);
            xhr.send(formData);

        } catch (error) {
            console.error('Upload error:', error);
            progressBar.error();
            resolve();
        }
    });
}

function filterVideoLibrary() {
    const query = document.getElementById('videoSearch').value.toLowerCase();
    const items = document.querySelectorAll('#videoPlaylistAvailable .draggable-item');

    items.forEach(item => {
        const text = item.textContent.toLowerCase();
        item.style.display = text.includes(query) ? 'flex' : 'none';
    });
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

function switchGameView(viewName) {
    // Update sub-tabs
    document.querySelectorAll('.sub-tab').forEach(tab => {
        if (tab.textContent.toLowerCase().includes(viewName.replace('game', ''))) {
            // Only activate if it's inside the games tab
            if (tab.closest('#roms')) tab.classList.add('active');
        } else {
            if (tab.closest('#roms')) tab.classList.remove('active');
        }

        // Handle specific mapping
        if (viewName === 'playlists' && tab.textContent.includes('My Playlists') && tab.closest('#roms')) tab.classList.add('active');
        if (viewName === 'editor' && tab.textContent.includes('Editor') && tab.closest('#roms')) tab.classList.add('active');
        if (viewName === 'library' && tab.textContent.includes('ROM Library') && tab.closest('#roms')) tab.classList.add('active');
    });

    // Update views
    document.querySelectorAll('#roms .sub-view').forEach(view => {
        view.style.display = 'none';
        view.classList.remove('active');
    });

    const viewId = `game${viewName.charAt(0).toUpperCase() + viewName.slice(1)}View`;
    // Handle library special case
    const targetId = viewName === 'library' ? 'romLibraryView' : viewId;

    const view = document.getElementById(targetId);
    if (view) {
        view.style.display = 'block';
        view.classList.add('active');
    }
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
    // Clear form
    document.getElementById('gamePlaylistTitle').value = '';
    document.getElementById('gamePlaylistCurator').value = '';
    document.getElementById('gamePlaylistDesc').value = '';
    document.getElementById('gamePlaylistLoop').checked = false;
    document.getElementById('gameEditorTitle').textContent = 'New Playlist';

    // Clear items
    gamePlaylistItems = [];
    renderGamePlaylistItems();

    // Remove editing file reference
    const saveBtn = document.getElementById('saveGamePlaylist');
    if (saveBtn) delete saveBtn.dataset.editingFile;

    // Switch to editor
    switchGameView('editor');
}

function filterGameLibrary() {
    const query = document.getElementById('gameSearch').value.toLowerCase();
    const items = document.querySelectorAll('#gamePlaylistAvailable .draggable-item');

    items.forEach(item => {
        const text = item.textContent.toLowerCase();
        item.style.display = text.includes(query) ? 'flex' : 'none';
    });
}

// ===== ROM MANAGEMENT =====

async function loadROMs() {
    if (!currentDevice) return;

    try {
        const response = await fetch(`${currentDevice.url}/admin/roms`);
        availableROMs = await response.json();

        // Calculate total ROM count
        const totalROMs = Object.values(availableROMs).reduce((sum, roms) => sum + roms.length, 0);

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
        const selector = document.querySelector('#sourceGameUpload #systemSelect');
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

    // Create progress bars for all files first
    const queue = [];
    for (const file of files) {
        const progressBar = createProgressBar(file.name);
        progressElement.appendChild(progressBar.element);
        queue.push({ file, progressBar });
    }

    // Process queue with concurrency limit
    const CONCURRENCY_LIMIT = 3;
    let activeUploads = 0;
    let currentIndex = 0;

    const processQueue = () => {
        while (activeUploads < CONCURRENCY_LIMIT && currentIndex < queue.length) {
            const item = queue[currentIndex++];
            activeUploads++;
            uploadSingleROM(item.file, system, item.progressBar, autoAddToPlaylist).finally(() => {
                activeUploads--;
                processQueue();
            });
        }
    };

    processQueue();
}

function uploadSingleROM(file, system, progressBar, autoAddToPlaylist) {
    return new Promise((resolve, reject) => {
        const formData = new FormData();
        formData.append('file', file);

        try {
            const xhr = new XMLHttpRequest();

            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    progressBar.update(percent);
                }
            });

            xhr.addEventListener('load', async () => {
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

                            // We need to construct the item correctly for the playlist
                            const playlistItem = {
                                title: file.name.replace(/\.[^/.]+$/, ""), // Remove extension
                                source_type: 'emulated_game',
                                path: result.path || `data/roms/${system}/${file.name}`,
                                emulator_system: system,
                                emulator_core: coreMap[system] || 'auto'
                            };

                            // Add to playlist items
                            gamePlaylistItems.push(playlistItem);
                            renderGamePlaylistItems();

                            // Switch back to library tab
                            setTimeout(() => {
                                switchGameSourceTab('library');
                            }, 1000);
                        }
                    } catch (e) {
                        console.error('Error parsing upload response:', e);
                    }

                    // Refresh ROM list
                    await loadROMs();
                    resolve();
                } else {
                    progressBar.error();
                    resolve(); // Resolve anyway to continue queue
                }
            });

            xhr.addEventListener('error', () => {
                progressBar.error();
                resolve();
            });

            xhr.open('POST', `${currentDevice.url}/admin/upload/rom/${system}`);
            xhr.send(formData);

        } catch (error) {
            console.error('ROM upload error:', error);
            progressBar.error();
            resolve();
        }
    });
}

// ===== PLAYLIST MANAGEMENT =====

async function loadExistingPlaylists() {
    if (!currentDevice) return;

    try {
        const response = await fetch(`${currentDevice.url}/admin/playlists`);
        const allPlaylists = await response.json();

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
                    const fullData = await detailResponse.json();

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
                        <h4>${pl.title}</h4>
                        <div class="playlist-actions">
                            <button onclick="editPlaylist('${pl.filename}', 'video')">Edit</button>
                            <button onclick="deletePlaylist('${pl.filename}', 'video')">Delete</button>
                        </div>
                    </div>
                    <div class="playlist-meta">
                        By ${pl.curator} • ${pl.item_count} items
                        ${pl.description ? `<br><em>${pl.description}</em>` : ''}
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
                        <h4>${pl.title}</h4>
                        <div class="playlist-actions">
                            <button onclick="editPlaylist('${pl.filename}', 'game')">Edit</button>
                            <button onclick="deletePlaylist('${pl.filename}', 'game')">Delete</button>
                        </div>
                    </div>
                    <div class="playlist-meta">
                        By ${pl.curator} • ${pl.item_count} items
                        ${pl.description ? `<br><em>${pl.description}</em>` : ''}
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
        const playlistData = await response.json();

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

        if (type === 'game') {
            gamePlaylistItems = items;
            renderGamePlaylistItems();
            // Switch to game editor view
            switchGameView('editor');
        } else {
            videoPlaylistItems = items;
            renderVideoPlaylistItems();
            // Switch to editor view
            switchVideoView('editor');
        }

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

    const items = type === 'game' ? gamePlaylistItems : videoPlaylistItems;

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
            headers: { 'Content-Type': 'application/json' },
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

        // Clear items
        if (type === 'game') {
            gamePlaylistItems = [];
            renderGamePlaylistItems();
        } else {
            videoPlaylistItems = [];
            renderVideoPlaylistItems();
        }

        // Hide cancel button
        const cancelBtn = document.getElementById(`cancel${prefix.charAt(0).toUpperCase() + prefix.slice(1)}PlaylistEdit`);
        if (cancelBtn) cancelBtn.style.display = 'none';

        // Remove editing file reference
        const saveBtn = document.getElementById(`save${prefix.charAt(0).toUpperCase() + prefix.slice(1)}Playlist`);
        if (saveBtn) delete saveBtn.dataset.editingFile;

        // Switch back to playlists view
        if (type === 'game') {
            switchGameView('playlists');
        } else {
            switchVideoView('playlists');
        }
    } catch (e) {
        console.error('Error in cancelEdit:', e);
    }
}

// ===== PLAYLIST BUILDER =====

function renderVideoPlaylistAvailable() {
    const container = document.getElementById('videoPlaylistAvailable');

    if (availableVideos.length === 0) {
        container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No videos available</p>';
        return;
    }

    // Filter out videos already in the playlist
    const addedPaths = new Set(videoPlaylistItems.map(item => item.path));
    const filteredVideos = availableVideos.filter(video => {
        const videoPath = video.path || `media / ${video.filename}`;
        return !addedPaths.has(videoPath);
    });

    if (filteredVideos.length === 0) {
        container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">All videos have been added to the playlist</p>';
        return;
    }

    container.innerHTML = filteredVideos.map((video, index) => {
        // Find original index for drag handling
        const originalIndex = availableVideos.indexOf(video);
        return `
        <div class="content-item" draggable="true" 
                 data-type="video" data-index="${originalIndex}" data-target="video"
                 ondragstart="handleDragStart(event, 'video')" ondragend="handleDragEnd(event)">
                📹 ${video.filename}
            </div>
            `;
    }).join('');

    // Add touch event handlers for mobile
    if (hasTouch) {
        setupTouchHandlers('video');
    }
}

function renderGamePlaylistAvailable() {
    const container = document.getElementById('gamePlaylistAvailable');

    const systems = Object.keys(availableROMs);
    if (systems.length === 0) {
        container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No ROMs available</p>';
        return;
    }

    // Filter out ROMs already in the playlist
    const addedPaths = new Set(gamePlaylistItems.map(item => item.path));

    let html = '';
    let totalAvailable = 0;
    systems.forEach(system => {
        const roms = availableROMs[system];
        const filteredRoms = roms.filter(rom => {
            const romPath = rom.path;
            return !addedPaths.has(romPath);
        });

        if (filteredRoms.length > 0) {
            html += `<div style="margin-bottom: 1rem;"><strong style="color: var(--accent2);">${system.toUpperCase()}</strong></div>`;
            filteredRoms.forEach((rom) => {
                const originalIndex = roms.indexOf(rom);
                totalAvailable++;
                html += `
            <div class="content-item" draggable="true"
        data-type="rom" data-system="${system}" data-index="${originalIndex}" data-target="game"
        ondragstart="handleDragStart(event, 'game')" ondragend="handleDragEnd(event)">
                        🎮 ${rom.filename}
                    </div>
            `;
            });
        }
    });

    if (totalAvailable === 0) {
        container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">All ROMs have been added to the playlist</p>';
        return;
    }

    container.innerHTML = html;

    // Add touch event handlers for mobile
    if (hasTouch) {
        setupTouchHandlers('game');
    }
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
    const container = document.getElementById('videoPlaylistItems');
    const emptyState = document.getElementById('videoPlaylistEmpty');

    if (videoPlaylistItems.length === 0) {
        container.innerHTML = '';
        emptyState.style.display = 'block';
        return;
    }

    emptyState.style.display = 'none';

    container.innerHTML = videoPlaylistItems.map((item, index) => {
        const icon = item.source_type === 'emulated_game' ? '🎮' : '📹';
        const name = item.title || item.path?.split('/').pop() || 'Unknown';
        const artist = item.artist || '';
        const artistDisplay = artist ? ` - ${artist} ` : ' - <em>No artist</em>';

        return `
            <div class="playlist-item" draggable="true" data-index="${index}" data-playlist-type="video"
        ondragstart="handlePlaylistDragStart(event, 'video')"
        ondragover="handlePlaylistDragOver(event)"
        ondrop="handlePlaylistDrop(event, 'video')"
        ondragend="handleDragEnd(event)">
            <div class="playlist-item-content">
                <div class="playlist-item-info">
                    <span class="item-icon">${icon}</span>
                    <span class="item-title">${name}</span>
                    <span class="item-artist">${artistDisplay}</span>
                </div>
                <div class="playlist-item-actions">
                    <button class="btn-edit" onclick="editPlaylistItem(${index}, 'video')">Edit</button>
                    <button class="btn-remove" onclick="removePlaylistItem(${index}, 'video')">✕</button>
                </div>
            </div>
            </div>
            `;
    }).join('');

    // Add touch event handlers for mobile reordering
    if (hasTouch) {
        setupPlaylistTouchHandlers('video');
    }
}

function renderGamePlaylistItems() {
    const container = document.getElementById('gamePlaylistItems');
    const emptyState = document.getElementById('gamePlaylistEmpty');

    if (gamePlaylistItems.length === 0) {
        container.innerHTML = '';
        emptyState.style.display = 'block';
        return;
    }

    emptyState.style.display = 'none';

    container.innerHTML = gamePlaylistItems.map((item, index) => {
        const icon = item.source_type === 'emulated_game' ? '🎮' : '📹';
        const name = item.title || item.path?.split('/').pop() || 'Unknown';
        const artist = item.artist || '';
        const artistDisplay = artist ? ` - ${artist} ` : ' - <em>No artist</em>';

        return `
            <div class="playlist-item" draggable="true" data-index="${index}" data-playlist-type="game"
        ondragstart="handlePlaylistDragStart(event, 'game')"
        ondragover="handlePlaylistDragOver(event)"
        ondrop="handlePlaylistDrop(event, 'game')"
        ondragend="handleDragEnd(event)">
            <div class="playlist-item-content">
                <div class="playlist-item-info">
                    <span class="item-icon">${icon}</span>
                    <span class="item-title">${name}</span>
                    <span class="item-artist">${artistDisplay}</span>
                </div>
                <div class="playlist-item-actions">
                    <button class="btn-edit" onclick="editPlaylistItem(${index}, 'game')">Edit</button>
                    <button class="btn-remove" onclick="removePlaylistItem(${index}, 'game')">✕</button>
                </div>
            </div>
            </div>
            `;
    }).join('');

    // Add touch event handlers for mobile reordering
    if (hasTouch) {
        setupPlaylistTouchHandlers('game');
    }
}

function removePlaylistItem(index, type) {
    if (type === 'game') {
        gamePlaylistItems.splice(index, 1);
        renderGamePlaylistItems();
        renderGamePlaylistAvailable(); // Update available list
    } else {
        videoPlaylistItems.splice(index, 1);
        renderVideoPlaylistItems();
        renderVideoPlaylistAvailable(); // Update available list
    }
}

function editPlaylistItem(index, type) {
    const items = type === 'game' ? gamePlaylistItems : videoPlaylistItems;
    const item = items[index];

    // Instead of prompt, make the fields inline-editable
    // Find the playlist item element
    const container = type === 'game' ? document.getElementById('gamePlaylistItems') : document.getElementById('videoPlaylistItems');
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
    const panelId = type === 'game' ? 'gamePlaylistItems' : 'videoPlaylistItems';
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

    // Add to appropriate playlist
    if (type === 'game') {
        gamePlaylistItems.push(item);
        renderGamePlaylistItems();
        renderGamePlaylistAvailable(); // Update available ROMs to hide added item
    } else {
        videoPlaylistItems.push(item);
        renderVideoPlaylistItems();
        renderVideoPlaylistAvailable(); // Update available videos to hide added item
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
        const items = type === 'game' ? gamePlaylistItems : videoPlaylistItems;
        const item = items[draggedPlaylistIndex];
        items.splice(draggedPlaylistIndex, 1);
        items.splice(dropIndex, 0, item);

        if (type === 'game') {
            renderGamePlaylistItems();
        } else {
            renderVideoPlaylistItems();
        }
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
    fill.className = 'progress-fill';
    fill.style.width = '0%';
    fill.textContent = `${filename} - 0 % `;

    container.appendChild(fill);

    return {
        element: container,
        update: (percent) => {
            fill.style.width = `${percent}% `;
            fill.textContent = `${filename} - ${percent}% `;
        },
        complete: () => {
            fill.style.width = '100%';
            fill.textContent = `${filename} - Complete! ✓`;
            fill.classList.add('complete');
        },
        error: () => {
            fill.style.width = '100%';
            fill.textContent = `${filename} - Error ✗`;
            fill.classList.add('error');
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

async function deleteROM(path, filename) {
    showConfirmModal(
        'Delete ROM',
        `Are you sure you want to delete "${filename}" ? `,
        async () => {
            try {
                // Encode the path components to handle special characters in URL
                const encodedPath = path.split('/').map(encodeURIComponent).join('/');

                const response = await fetch(`${currentDevice.url}/admin/roms/${encodedPath}`, {
                    method: 'DELETE'
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
                    method: 'DELETE'
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
        const panelId = playlistType === 'game' ? 'gamePlaylistItems' : 'videoPlaylistItems';
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
        const panelId = playlistType === 'game' ? 'gamePlaylistItems' : 'videoPlaylistItems';
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
    const panelId = type === 'game' ? 'gamePlaylistItems' : 'videoPlaylistItems';
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
        const panelId = type === 'game' ? 'gamePlaylistItems' : 'videoPlaylistItems';
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

            const items = type === 'game' ? gamePlaylistItems : videoPlaylistItems;
            const item = items[touchReorderStartIndex];
            items.splice(touchReorderStartIndex, 1);
            items.splice(touchReorderCurrentIndex, 0, item);

            // Haptic feedback
            if (navigator.vibrate) {
                navigator.vibrate([50, 50, 50]);
            }

            // Re-render the correct playlist
            if (type === 'game') {
                renderGamePlaylistItems();
            } else {
                renderVideoPlaylistItems();
            }
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
        const panelId = type === 'game' ? 'gamePlaylistItems' : 'videoPlaylistItems';
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

