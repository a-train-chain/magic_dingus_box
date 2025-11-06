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
    initializeEventListeners();
    discoverDevices();
    
    // Auto-refresh device list every 30 seconds
    setInterval(discoverDevices, 30000);
    
    // Initialize collapsible sections (all open by default)
    initializeCollapsibleSections();
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
    
    // Try localhost first
    await checkDevice('localhost', 8080);
    await checkDevice('127.0.0.1', 8080);
    
    // Try to detect from current URL
    const hostname = window.location.hostname;
    if (hostname && hostname !== 'localhost' && hostname !== '127.0.0.1') {
        await checkDevice(hostname, 8080);
    }
    
    // If on local network, try common IPs
    if (hostname.match(/^\d+\.\d+\.\d+\.\d+$/)) {
        const subnet = hostname.split('.').slice(0, 3).join('.');
        const promises = [];
        for (let i = 1; i <= 254; i++) {
            promises.push(checkDevice(`${subnet}.${i}`, 8080));
        }
        await Promise.allSettled(promises);
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
    currentDevice = discoveredDevices[index];
    
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
    loadAllContentFromDevice();
    
    // Auto-collapse device selector after connection
    setTimeout(() => {
        toggleSection('deviceSelector');
    }, 1000);
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

// ===== VIDEO MANAGEMENT =====

async function loadVideos() {
    if (!currentDevice) return;
    
    try {
        const response = await fetch(`${currentDevice.url}/admin/media`);
        availableVideos = await response.json();
        
        // Update count badge
        const countBadge = document.getElementById('videoCount');
        if (countBadge) {
            countBadge.textContent = availableVideos.length;
        }
        
        const container = document.getElementById('videoList');
        if (availableVideos.length === 0) {
            container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No videos uploaded yet</p>';
            return;
        }
        
        container.innerHTML = availableVideos.map(video => `
            <div class="media-card">
                <h4>📹 ${video.filename}</h4>
                <div class="meta">
                    ${formatFileSize(video.size)}
                </div>
            </div>
        `).join('');
    } catch (e) {
        console.error('Failed to load videos:', e);
    }
}

async function uploadVideos() {
    if (!currentDevice) {
        alert('Please select a device first');
        return;
    }
    
    const input = document.getElementById('videoUpload');
    const files = Array.from(input.files);
    const progress = document.getElementById('uploadProgress');
    
    progress.innerHTML = '';
    
    for (const file of files) {
        const formData = new FormData();
        formData.append('file', file);
        
        const progressBar = createProgressBar(file.name);
        progress.appendChild(progressBar.element);
        
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
                    setTimeout(() => loadVideos(), 500);
                } else {
                    progressBar.error();
                }
            });
            
            xhr.open('POST', `${currentDevice.url}/admin/upload`);
            xhr.send(formData);
            
        } catch (error) {
            console.error('Upload error:', error);
            progressBar.error();
        }
    }
    
    input.value = '';
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
        
        container.innerHTML = systems.map(system => {
            const roms = availableROMs[system];
            return `
                <div class="accordion-item">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <h4>🎮 ${system.toUpperCase()} (${roms.length})</h4>
                        <span>▼</span>
                    </div>
                    <div class="accordion-content">
                        ${roms.map(rom => `
                            <div class="rom-item">
                                <div>${rom.filename}</div>
                                <div class="meta">${formatFileSize(rom.size)}</div>
                            </div>
                        `).join('')}
                    </div>
                </div>
            `;
        }).join('');
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

async function uploadROMs() {
    if (!currentDevice) {
        alert('Please select a device first');
        return;
    }
    
    const input = document.getElementById('romUpload');
    const system = document.getElementById('systemSelect').value;
    const files = Array.from(input.files);
    
    for (const file of files) {
        const formData = new FormData();
        formData.append('file', file);
        
        try {
            await fetch(`${currentDevice.url}/admin/upload/rom/${system}`, {
                method: 'POST',
                body: formData
            });
        } catch (error) {
            console.error('ROM upload error:', error);
            alert(`Failed to upload ${file.name}`);
        }
    }
    
    input.value = '';
    await loadROMs();
    renderAvailableContent();
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
            // Fetch full playlist to check item types
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
        const cancelBtn = document.getElementById(`cancel${prefix.charAt(0).toUpperCase() + prefix.slice(1)}PlaylistEdit`);
        
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
        } else {
            videoPlaylistItems = items;
            renderVideoPlaylistItems();
        }
        
        // Show cancel button and store filename
        if (cancelBtn) cancelBtn.style.display = 'inline-block';
        if (saveBtn) saveBtn.dataset.editingFile = filename;
        
        // Switch to appropriate tab and expand section
        const tabName = type === 'game' ? 'roms' : 'videos';
        document.querySelector(`[data-tab="${tabName}"]`).click();
        
        // Expand the playlist builder section
        const builderSection = document.getElementById(`${prefix}PlaylistBuilder`);
        if (builderSection && builderSection.classList.contains('collapsed')) {
            toggleSection(`${prefix}PlaylistBuilder`);
        }
        
        // Scroll to form
        setTimeout(() => {
            builderSection?.scrollIntoView({ behavior: 'smooth', block: 'start' });
        }, 100);
    } catch (e) {
        console.error('Failed to edit playlist:', e);
        alert('Failed to load playlist');
    }
}

async function deletePlaylist(filename, type) {
    if (!confirm(`Delete playlist "${filename}"?`)) return;
    
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

async function savePlaylist(type) {
    if (!currentDevice) {
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
            alert('Playlist saved!');
            cancelEdit(type);
            await loadExistingPlaylists();
        } else {
            alert('Failed to save playlist');
        }
    } catch (e) {
        console.error('Failed to save playlist:', e);
        alert('Failed to save playlist');
    }
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
    const prefix = type === 'game' ? 'game' : 'video';
    
    // Clear form
    document.getElementById(`${prefix}PlaylistTitle`).value = '';
    document.getElementById(`${prefix}PlaylistCurator`).value = '';
    document.getElementById(`${prefix}PlaylistDesc`).value = '';
    document.getElementById(`${prefix}PlaylistLoop`).checked = false;
    
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
}

// ===== PLAYLIST BUILDER =====

function renderVideoPlaylistAvailable() {
    const container = document.getElementById('videoPlaylistAvailable');
    
    if (availableVideos.length === 0) {
        container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No videos available</p>';
        return;
    }
    
    container.innerHTML = availableVideos.map((video, index) => `
        <div class="content-item" draggable="true" 
             data-type="video" data-index="${index}" data-target="video"
             ondragstart="handleDragStart(event, 'video')" ondragend="handleDragEnd(event)">
            📹 ${video.filename}
        </div>
    `).join('');
    
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
    
    let html = '';
    systems.forEach(system => {
        const roms = availableROMs[system];
        html += `<div style="margin-bottom: 1rem;"><strong style="color: var(--accent2);">${system.toUpperCase()}</strong></div>`;
        roms.forEach((rom, index) => {
            html += `
                <div class="content-item" draggable="true"
                     data-type="rom" data-system="${system}" data-index="${index}" data-target="game"
                     ondragstart="handleDragStart(event, 'game')" ondragend="handleDragEnd(event)">
                    🎮 ${rom.filename}
                </div>
            `;
        });
    });
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
        const artistDisplay = artist ? ` - ${artist}` : ' - <em>No artist</em>';
        
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
        const artistDisplay = artist ? ` - ${artist}` : ' - <em>No artist</em>';
        
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
    } else {
        videoPlaylistItems.splice(index, 1);
        renderVideoPlaylistItems();
    }
}

function editPlaylistItem(index, type) {
    const items = type === 'game' ? gamePlaylistItems : videoPlaylistItems;
    const item = items[index];
    
    // Prompt for new values
    const newTitle = prompt('Edit title:', item.title);
    if (newTitle === null) return; // User cancelled
    
    const newArtist = prompt('Edit artist:', item.artist || '');
    if (newArtist === null) return; // User cancelled
    
    // Update the item
    items[index].title = newTitle.trim();
    items[index].artist = newArtist.trim();
    
    // Re-render
    if (type === 'game') {
        renderGamePlaylistItems();
    } else {
        renderVideoPlaylistItems();
    }
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
        
        // Auto-detect emulator core
        const coreMap = {
            'nes': 'fceumm_libretro',
            'snes': 'snes9x_libretro',
            'n64': 'parallel_n64_libretro',
            'ps1': 'pcsx_rearmed_libretro'
        };
        
        // Only include fields that have values (matches YAML format)
        item = {
            title: rom.filename,
            artist: '',  // Empty artist field (games don't typically have artists)
            source_type: 'emulated_game',
            path: rom.path,
            emulator_core: coreMap[system] || 'unknown',
            emulator_system: system.toUpperCase()
        };
    }
    
    // Add to appropriate playlist
    if (type === 'game') {
        gamePlaylistItems.push(item);
        renderGamePlaylistItems();
    } else {
        videoPlaylistItems.push(item);
        renderVideoPlaylistItems();
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
    fill.textContent = `${filename} - 0%`;
    
    container.appendChild(fill);
    
    return {
        element: container,
        update: (percent) => {
            fill.style.width = `${percent}%`;
            fill.textContent = `${filename} - ${percent}%`;
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

