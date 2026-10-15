// Playlist editor (videos and games share it through MEDIA_CONFIG): the
// playlist list, edit/save/cancel, the builder and library panels, drag-handle
// and touch reordering, and playlist delete.
//
// Part of the Content Manager front end; see the load-order note at the top
// of manager.js. Loaded after: manager.js, manager_devices.js, manager_media.js, manager_roms.js.

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
/**
 * Render playlist items in the editor
 * @param {string} type - 'video' or 'game'
 */
function renderPlaylistItems(type) {
    const config = MEDIA_CONFIG[type];
    const container = document.getElementById(config.playlistItemsId);
    const emptyState = document.getElementById(config.playlistEmptyId);
    // Per-type, not hardcoded: this function is shared by both editors, so a
    // literal 'playlistItemCount' here wrote the VIDEO counter even when
    // rendering games — leaving the games editor's own counter permanently
    // reading "0 items" no matter how many ROMs were in the playlist.
    const countEl = document.getElementById(config.playlistCountId);
    const items = config.getItems();

    // Update count
    const actualItems = items.filter(item => !item.isEmpty);
    if (countEl) countEl.textContent = `${actualItems.length} item${actualItems.length !== 1 ? 's' : ''}`;

    // Bind the drop handlers BEFORE the empty-playlist early return below.
    // They used to be bound only at the end of this function, which an empty
    // playlist never reaches — so dragging a video into a brand new playlist
    // had no target at all and silently did nothing.
    bindPlaylistDropTargets(container, type);

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
                <tr class="${rowClass}" data-index="${index}" data-playlist-type="${type}">
                    <td class="col-handle">
                        <span class="drag-handle" draggable="true"
                              ondragstart="handlePlaylistDragStart(event, '${type}', ${index})"
                              ondragend="handlePlaylistDragEnd(event, '${type}')"
                              title="Drag to reorder">⋮⋮</span>
                    </td>
                    <td class="col-num">${index + 1}</td>
                    <td class="col-title"><span class="empty-label">(empty slot)</span></td>
                    <td class="col-artist"></td>
                    <td class="col-actions">
                        <div class="row-actions">
                            <button class="btn-delete" onclick="removePlaylistItem(${index}, '${type}')">✕</button>
                        </div>
                    </td>
                </tr>
            `;
        }

        const title = item.title || item.path?.split('/').pop()?.replace(/\.[^.]+$/, '') || 'Untitled';
        const secondaryField = config.secondaryField || 'artist';
        const secondary = item[secondaryField] || '';

        return `
            <tr class="${rowClass}" data-index="${index}" data-playlist-type="${type}">
                <td class="col-handle">
                    <span class="drag-handle" draggable="true"
                          ondragstart="handlePlaylistDragStart(event, '${type}', ${index})"
                          ondragend="handlePlaylistDragEnd(event, '${type}')"
                          title="Drag to reorder">⋮⋮</span>
                </td>
                <td class="col-num">${index + 1}</td>
                <td class="col-title">
                    <input type="text" class="inline-input" draggable="false" value="${escapeHtml(title)}"
                           placeholder="Title" onchange="updatePlaylistItem(${index}, 'title', this.value, '${type}')">
                </td>
                <td class="col-artist">
                    <input type="text" class="inline-input" draggable="false" value="${escapeHtml(secondary)}"
                           placeholder="${config.secondaryLabel || 'Artist'}" onchange="updatePlaylistItem(${index}, '${secondaryField}', this.value, '${type}')">
                </td>
                <td class="col-actions">
                    <div class="row-actions">
                        <button class="btn-delete" onclick="removePlaylistItem(${index}, '${type}')">✕</button>
                    </div>
                </td>
            </tr>
        `;
    }).join('');

    // Drop targets are bound near the top of this function so they also exist
    // for an empty playlist; re-bound here because innerHTML replaced the rows.
    bindPlaylistDropTargets(container, type);

    // Touch equivalent of the above. HTML5 dnd does not fire on touch at all,
    // so without this the ⋮⋮ handle is inert on a phone and — with the up/down
    // arrows removed — there is no way to reorder a playlist on the device this
    // page is primarily used from. Idempotent (on* properties), so re-running
    // on every render rebinds rather than stacking handlers.
    setupPlaylistTouchHandlers(type);
}

/**
 * Render available items that can be added to a playlist
 * @param {string} type - 'video' or 'game'
 */
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
                    const detailResponse = await fetch(`${currentDevice.url}/admin/playlists/${encodeURIComponent(pl.filename)}`);
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
                        ${playlistMetaLine(pl)}
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
                        ${playlistMetaLine(pl)}
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
        const response = await fetch(`${currentDevice.url}/admin/playlists/${encodeURIComponent(filename)}`);
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
        alert(`Failed to load playlist: ${e.message}\n\n${e.stack}`);
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
        // Blank stays blank. This wrote the literal string 'Unknown', so merely
        // opening one of the imported playlists and saving it permanently
        // changed its byline from nothing to "By Unknown". An empty curator is
        // a legitimate value the kiosk and the playlist cards both handle.
        curator: curator,
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
    const filename = editingFile || playlistFilenameFor(title);

    // For a NEW playlist (not editing an existing file), ask the server to
    // refuse if the derived filename already belongs to a different
    // playlist — prevents silently clobbering it. Editing an existing file
    // is an intentional overwrite.
    const isNew = !editingFile;

    async function doSave(overwrite) {
        const qs = overwrite ? '?overwrite=true' : '?overwrite=false';
        // Guard against a double-tap firing two concurrent POSTs to the same
        // playlist path (no visual feedback otherwise, so slow WiFi trains
        // re-taps). Disable the Save button for the duration of the request;
        // re-enable in finally so the 409→confirm→retry path stays usable.
        let response;
        if (saveBtn) saveBtn.disabled = true;
        try {
            response = await fetch(`${currentDevice.url}/admin/playlists/${encodeURIComponent(filename)}${qs}`, {
                method: 'POST',
                headers: getCsrfHeaders(),
                body: JSON.stringify(playlistData)
            });
        } finally {
            if (saveBtn) saveBtn.disabled = false;
        }

        if (response.ok) {
            cancelEdit(type);
            if (type === 'game') {
                switchGameView('playlists');
            } else {
                switchVideoView('playlists');
            }
            await loadExistingPlaylists();
            return;
        }

        if (response.status === 409) {
            // Name collision with an existing playlist. Let the user decide.
            showConfirmModal(
                'Playlist already exists',
                `A playlist named "${filename}" already exists. Overwrite it? ` +
                `(To keep both, cancel and rename this playlist.)`,
                () => { doSave(true).catch((e) => {
                    console.error('Failed to save playlist:', e);
                    alert('Failed to save playlist');
                }); }
            );
            return;
        }

        alert('Failed to save playlist');
    }

    try {
        await doSave(!isNew);  // editing → overwrite; new → guard first
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
    if (!Array.isArray(items)) return [];

    // Remove any null/undefined/empty fields from items to match YAML format
    return items.filter(item => item && typeof item === 'object').map(item => {
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

// NOTE: the canonical renderGamePlaylistAvailable() lives later in this file
// and calls renderROMLibraryPanel() (the current touch-friendly ROM UI). A
// second, older declaration used to sit here calling the superseded
// renderPlaylistAvailable('game'); JS hoisting meant the later one always
// won, so this was dead code that would silently resurrect the broken ROM
// panel if the two were ever reordered. Removed to kill that footgun.

function handleDragEnd(event) {
    event.target.classList.remove('dragging');
}

function removePlaylistItem(index, type) {
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    items.splice(index, 1);
    config.setItems(items);
    renderPlaylistItems(type);
    // Use the touch-friendly panels for BOTH types. The game branch must
    // call renderGamePlaylistAvailable() (→ renderROMLibraryPanel, with the
    // +Add buttons), NOT the legacy renderPlaylistAvailable('game') which
    // re-renders the old .content-item markup with no working touch handlers
    // — that silently broke "add ROM" after deleting a row on iPhone.
    if (type === 'video') {
        renderLibraryPanel();
    } else {
        renderGamePlaylistAvailable();
    }
}

// Setup drop zones for both playlist panels
document.addEventListener('DOMContentLoaded', () => {
    setupPlaylistDropZone('video');
    setupPlaylistDropZone('game');
});

function setupPlaylistDropZone(type) {
    const panelId = MEDIA_CONFIG[type].playlistItemsId;
    const tbody = document.getElementById(panelId);

    // Bind to the surrounding container as well, not just the tbody.
    //
    // An EMPTY playlist has a zero-height tbody, so there was literally nothing
    // on screen to drop a library item onto — dragging a video into a brand new
    // playlist did nothing, with no feedback. The visible affordance is
    // .playlist-empty-state, the dashed "Drag videos here" pocket, but that is a
    // SIBLING of <table> and is pointer-events: none (deliberately, so it never
    // swallows a drop), so events over it reach neither the tbody nor anything
    // that bubbles to it. The container sits underneath both and has real area.
    //
    // Binding both is safe: a drop lands on whichever is under the pointer, and
    // the handler is the same. Events that hit a row still bubble to the tbody
    // first, so row-level behaviour is unchanged.
    const shell = tbody ? tbody.closest('.playlist-table-container') : null;
    const panels = [tbody, shell && shell !== tbody ? shell : null].filter(Boolean);

    for (const panel of panels) {
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
            title: titleForLibraryItem(video),
            artist: '',  // Empty artist field, user can edit later
            source_type: 'local',
            path: video.path
        };
    } else if (draggedItem.type === 'emulated_game') {
        const rom = draggedItem.data;
        const system = draggedItem.system;

        // Refuse rather than write an item the kiosk will reject at launch.
        const core = coreForSystem(system);
        if (!core) {
            showNotification(
                `No emulator core is configured for "${system}" — cannot add this ROM.`,
                'error');
            return;
        }

        // Only include fields that have values (matches YAML format)
        item = {
            title: titleForLibraryItem(rom),
            artist: '',  // Empty artist field (games don't typically have artists)
            source_type: 'emulated_game',
            path: rom.path,
            emulator_core: core,
            emulator_system: system // Keep lowercase for consistency
        };
    }

    // Add to appropriate playlist using unified config
    const config = MEDIA_CONFIG[type];
    const items = config.getItems();
    items.push(item);
    config.setItems(items);
    renderPlaylistItems(type);

    // Refresh available items using the touch-friendly renderer for both
    // types (game → renderROMLibraryPanel via renderGamePlaylistAvailable;
    // see removePlaylistItem for why the legacy path is avoided).
    if (type === 'video') {
        renderVideoPlaylistAvailable();
    } else {
        renderGamePlaylistAvailable();
    }
}

// ===== PLAYLIST DRAG-HANDLE REORDERING =====
//
// Each playlist row has a <span class="drag-handle" draggable="true"> in its
// leftmost cell. Dragging the handle shows a horizontal drop indicator that
// follows the pointer, marking the insert-before or insert-after position.
// On drop, the dragged item is spliced out of its old position and inserted
// at the indicator's position (not swapped).
//
// Module-level state for the in-flight drag:
let draggedPlaylistIndex = null;     // original index of the item being dragged
let draggedPlaylistType = null;      // 'video' or 'game'
let pendingDropIndex = null;         // where the drop would land, computed in dragover

function handlePlaylistDragStart(event, type, index) {
    draggedPlaylistIndex = index;
    draggedPlaylistType = type;
    pendingDropIndex = index; // until we hear otherwise

    // Visual feedback on the source row
    const row = event.target.closest('tr');
    if (row) row.classList.add('dragging');

    // Firefox requires dataTransfer to be set for the drag to initiate
    if (event.dataTransfer) {
        event.dataTransfer.effectAllowed = 'move';
        try { event.dataTransfer.setData('text/plain', String(index)); } catch (e) {}
    }
}

function handlePlaylistDragOver(event, type) {
    // Only respond if we're dragging within the same playlist type.
    if (draggedPlaylistType !== type || draggedPlaylistIndex === null) return;

    event.preventDefault(); // allow drop
    if (event.dataTransfer) event.dataTransfer.dropEffect = 'move';

    // Find the row under the pointer
    const row = event.target.closest('tr[data-index]');
    const container = event.currentTarget.closest('.playlist-table-container');
    if (!container) return;
    const indicator = container.querySelector('.playlist-drop-indicator');
    if (!indicator) return;

    if (!row) {
        // Pointer is in the tbody but not over a specific row (e.g. gap at the
        // very bottom). Treat as "insert at end".
        const items = container.querySelectorAll('tbody tr[data-index]');
        if (items.length === 0) { indicator.style.display = 'none'; return; }
        const lastRow = items[items.length - 1];
        positionIndicatorBelow(indicator, container, lastRow);
        pendingDropIndex = items.length;
        return;
    }

    const rowIndex = parseInt(row.dataset.index, 10);
    const rect = row.getBoundingClientRect();
    const midY = rect.top + rect.height / 2;
    const before = event.clientY < midY;

    if (before) {
        positionIndicatorAbove(indicator, container, row);
        pendingDropIndex = rowIndex;
    } else {
        positionIndicatorBelow(indicator, container, row);
        pendingDropIndex = rowIndex + 1;
    }
}

function handlePlaylistDrop(event, type) {
    if (draggedPlaylistType !== type || draggedPlaylistIndex === null) return;
    event.preventDefault();

    const container = event.currentTarget.closest('.playlist-table-container');
    const indicator = container ? container.querySelector('.playlist-drop-indicator') : null;
    if (indicator) indicator.style.display = 'none';

    const fromIndex = draggedPlaylistIndex;
    let toIndex = pendingDropIndex != null ? pendingDropIndex : fromIndex;

    // Normalize: if dropping "after" its own position, the splice math needs
    // the toIndex to account for the fact that we remove from fromIndex first,
    // shifting later indices down by one.
    if (toIndex > fromIndex) toIndex -= 1;

    // No-op if dropped on itself
    if (toIndex !== fromIndex) {
        const config = MEDIA_CONFIG[type];
        const items = config.getItems();
        const [moved] = items.splice(fromIndex, 1);
        items.splice(toIndex, 0, moved);
        config.setItems(items);
        renderPlaylistItems(type);
    }

    draggedPlaylistIndex = null;
    draggedPlaylistType = null;
    pendingDropIndex = null;
}

function handlePlaylistDragEnd(event, type) {
    // Always called, even if the drag ended outside a drop target.
    // Hide indicator + clear state.
    document.querySelectorAll('.playlist-drop-indicator').forEach(el => {
        el.style.display = 'none';
    });
    document.querySelectorAll('tr.dragging').forEach(el => {
        el.classList.remove('dragging');
    });
    draggedPlaylistIndex = null;
    draggedPlaylistType = null;
    pendingDropIndex = null;
}

// Position the drop indicator at the top edge of a target row.
function positionIndicatorAbove(indicator, container, row) {
    const containerRect = container.getBoundingClientRect();
    const rowRect = row.getBoundingClientRect();
    indicator.style.top = `${rowRect.top - containerRect.top}px`;
    indicator.style.display = 'block';
}

// Position the drop indicator at the bottom edge of a target row.
function positionIndicatorBelow(indicator, container, row) {
    const containerRect = container.getBoundingClientRect();
    const rowRect = row.getBoundingClientRect();
    indicator.style.top = `${rowRect.bottom - containerRect.top}px`;
    indicator.style.display = 'block';
}

async function deletePlaylist(filename, type) {
    if (!currentDevice) return;

    // Show loading overlay while fetching playlist details
    const loadingOverlay = document.createElement('div');
    loadingOverlay.className = 'loading-overlay';
    loadingOverlay.innerHTML = `
        <div class="loading-spinner"></div>
        <div class="loading-content">
            <div class="loading-title">Loading Playlist</div>
            <div class="loading-status">Checking video usage...</div>
            <div class="loading-details">${escapeHtml(filename)}</div>
        </div>
    `;
    document.body.appendChild(loadingOverlay);

    try {
        // Fetch playlist details to get video list
        const detailResponse = await fetch(`${currentDevice.url}/admin/playlists/${encodeURIComponent(filename)}`);
        const detailResult = await detailResponse.json();
        const playlistData = detailResult.data || detailResult;

        // Get local video paths from this playlist
        const playlistVideos = (playlistData.items || [])
            .filter(item => item.source_type === 'local' && item.path)
            .map(item => ({
                path: item.path,
                title: item.title || normalizeVideoPath(item.path),
                normalized: normalizeVideoPath(item.path)
            }));

        // Fetch all other playlists to check video usage
        const playlistsResponse = await fetch(`${currentDevice.url}/admin/playlists`);
        const playlistsResult = await playlistsResponse.json();
        const allPlaylists = (playlistsResult.data || playlistsResult)
            .filter(p => p.filename !== filename);

        // Build a set of video paths used in OTHER playlists
        const videosUsedElsewhere = new Set();
        for (const playlist of allPlaylists) {
            try {
                const otherResponse = await fetch(`${currentDevice.url}/admin/playlists/${encodeURIComponent(playlist.filename)}`);
                const otherResult = await otherResponse.json();
                const otherData = otherResult.data || otherResult;
                (otherData.items || []).forEach(item => {
                    if (item.source_type === 'local' && item.path) {
                        videosUsedElsewhere.add(normalizeVideoPath(item.path));
                    }
                });
            } catch (e) {
                // Skip problematic playlists
            }
        }

        // Determine orphaned videos (only used in this playlist)
        const orphanedVideos = playlistVideos.filter(v => !videosUsedElsewhere.has(v.normalized));
        const sharedVideos = playlistVideos.filter(v => videosUsedElsewhere.has(v.normalized));

        // Build modal content
        let modalContent = `<p>Delete playlist "<strong>${escapeHtml(filename)}</strong>"?</p>`;

        if (orphanedVideos.length > 0) {
            modalContent += `
                <div style="margin-top: 1rem; padding: 0.75rem; background: var(--surface); border-radius: 4px;">
                    <label style="display: flex; align-items: center; cursor: pointer;">
                        <input type="checkbox" id="deleteVideosCheckbox" style="margin-right: 0.5rem;">
                        <span>Also delete ${orphanedVideos.length} video(s) only used in this playlist</span>
                    </label>
                    <div style="margin-top: 0.5rem; max-height: 100px; overflow-y: auto; font-size: 0.85rem; color: var(--text-secondary);">
                        ${orphanedVideos.map(v => `<div>• ${escapeHtml(v.title)}</div>`).join('')}
                    </div>
                </div>
            `;
        }

        if (sharedVideos.length > 0) {
            modalContent += `
                <div style="margin-top: 0.75rem; font-size: 0.85rem; color: var(--text-secondary);">
                    <em>${sharedVideos.length} video(s) are used in other playlists and will NOT be deleted.</em>
                </div>
            `;
        }

        // Remove loading overlay before showing modal
        document.body.removeChild(loadingOverlay);

        showConfirmModalHtml(
            'Delete Playlist',
            modalContent,
            async () => {
                try {
                    const deleteVideos = document.getElementById('deleteVideosCheckbox')?.checked || false;
                    const url = `${currentDevice.url}/admin/playlists/${encodeURIComponent(filename)}${deleteVideos ? '?delete_videos=true' : ''}`;

                    const response = await fetch(url, {
                        method: 'DELETE',
                        headers: getCsrfHeaders(false)
                    });

                    const result = await response.json();
                    if (result.ok) {
                        const videosDeleted = result.data?.videos_deleted || 0;
                        if (videosDeleted > 0) {
                            showNotification(`Playlist deleted along with ${videosDeleted} video(s)`, 'success');
                        } else {
                            showNotification('Playlist deleted', 'success');
                        }
                    }

                    await loadExistingPlaylists();
                    await loadVideos();  // Refresh All Videos list after deletion
                } catch (e) {
                    console.error('Failed to delete playlist:', e);
                    alert('Failed to delete playlist');
                }
            }
        );
    } catch (e) {
        console.error('Failed to load playlist details:', e);
        // Remove loading overlay on error
        if (loadingOverlay.parentNode) {
            document.body.removeChild(loadingOverlay);
        }
        // Fallback to simple delete
        showConfirmModal(
            'Delete Playlist',
            `Delete playlist "${filename}"?`,
            async () => {
                try {
                    await fetch(`${currentDevice.url}/admin/playlists/${encodeURIComponent(filename)}`, {
                        method: 'DELETE',
                        headers: getCsrfHeaders(false)
                    });
                    await loadExistingPlaylists();
                    await loadVideos();
                } catch (e) {
                    console.error('Failed to delete playlist:', e);
                    alert('Failed to delete playlist');
                }
            }
        );
    }
}

// ===== TOUCH DRAG-AND-DROP SUPPORT =====

/**
 * Wire touch reordering for a playlist editor.
 *
 * This is the ONLY way to reorder on a phone — the rows carry `ondragstart`,
 * and HTML5 drag-and-drop never fires on touch — so with the up/down arrows
 * removed in favour of dragging, this path failing means a playlist cannot be
 * reordered on the device the Content Manager is actually used from.
 *
 * It was failing two ways at once: this function was never called from
 * anywhere, and it queried `.playlist-item`, a class the renderer does not
 * emit (rows are `<tr data-index>`), so even when called it bound nothing.
 *
 * Delegated on the tbody rather than per row: renderPlaylistItems() replaces
 * container.innerHTML on every change, which would drop per-row listeners
 * after the first reorder. The tbody itself survives, so one binding holds.
 *
 * addEventListener, NOT `panel.ontouchstart = ...` as the sibling dragover/drop
 * wiring uses. Touch on* properties are only real event-handler attributes when
 * the UA has touch events enabled; where they are not, the assignment silently
 * creates an ordinary expando — `typeof panel.ontouchstart` reads "function"
 * and nothing ever fires. That is not merely a harness artifact: it makes the
 * binding untestable anywhere without a touch stack, which is how this whole
 * path went unnoticed in the first place.
 *
 * Bound once and latched on a data attribute, because addEventListener does
 * stack: this runs on every render, and without the latch a playlist would
 * accumulate one handler set per re-render and reorder N times per drag.
 *
 * @param {string} type - 'video' or 'game'
 */
function setupPlaylistTouchHandlers(type) {
    const panel = document.getElementById(MEDIA_CONFIG[type].playlistItemsId);
    if (!panel || panel.dataset.touchReorderBound === type) return;

    panel.addEventListener('touchstart', (e) => handlePlaylistTouchStart(e, type), { passive: false });
    panel.addEventListener('touchmove', (e) => handlePlaylistTouchMove(e, type), { passive: false });
    panel.addEventListener('touchend', (e) => handlePlaylistTouchEnd(e, type), { passive: false });
    panel.addEventListener('touchcancel', (e) => handlePlaylistTouchEnd(e, type), { passive: false });

    panel.dataset.touchReorderBound = type;
}

/** The row a touch gesture is acting on, resolved from the event target. */
function playlistRowFromTouch(e) {
    return e.target.closest ? e.target.closest('tr[data-index]') : null;
}

let touchReorderStartIndex = null;
let touchReorderCurrentIndex = null;
let touchReorderType = null;

function handlePlaylistTouchStart(e, type) {
    // Drags start ONLY from the ⋮⋮ handle. The row also holds the title and
    // artist <input>s, and a press-anywhere drag would fight the keyboard —
    // touching a field to type would arm a reorder instead. The handle is also
    // the only element with `touch-action: none` (style.css), so it is the only
    // place the browser will hand us the gesture rather than scrolling.
    if (!e.target.closest || !e.target.closest('.drag-handle')) return;

    const item = playlistRowFromTouch(e);
    if (!item) return;

    const touch = e.touches[0];
    touchStartX = touch.pageX;
    touchStartY = touch.pageY;

    // Start immediately rather than after a long press: the handle is a
    // deliberate affordance, so a press on it is already an unambiguous intent
    // to drag, and touch-action:none means it cannot be the start of a scroll.
    isDragging = true;
    touchDragElement = item;
    touchReorderStartIndex = parseInt(item.dataset.index);
    touchReorderCurrentIndex = touchReorderStartIndex;
    touchReorderType = type;

    item.style.opacity = '0.5';
    item.classList.add('dragging');

    if (navigator.vibrate) {
        navigator.vibrate(50);
    }

    // Floating clone that follows the finger.
    touchDragClone = item.cloneNode(true);
    touchDragClone.style.position = 'fixed';
    touchDragClone.style.pointerEvents = 'none';
    touchDragClone.style.zIndex = '10000';
    touchDragClone.style.opacity = '0.9';
    touchDragClone.style.boxShadow = '0 10px 30px rgba(64, 255, 200, 0.5)';
    // clientX/clientY, NOT pageX/pageY: the clone is position:fixed, so its
    // coordinates are viewport-relative. Page coordinates include scroll
    // offset, which would fling the clone off-screen by exactly scrollY on any
    // scrolled page — and this list is normally reached by scrolling.
    touchDragClone.style.left = (touch.clientX - item.offsetWidth / 2) + 'px';
    touchDragClone.style.top = (touch.clientY - item.offsetHeight / 2) + 'px';
    touchDragClone.style.width = item.offsetWidth + 'px';

    document.body.appendChild(touchDragClone);
}

function handlePlaylistTouchMove(e, type) {
    if (!isDragging || !touchDragClone) return;

    e.preventDefault();

    const touch = e.touches[0];
    touchDragClone.style.left = (touch.clientX - touchDragClone.offsetWidth / 2) + 'px';
    touchDragClone.style.top = (touch.clientY - touchDragClone.offsetHeight / 2) + 'px';

    const panel = document.getElementById(MEDIA_CONFIG[type].playlistItemsId);
    if (!panel) return;

    // clientY against getBoundingClientRect(): both viewport-relative. This
    // compared pageY (document-relative) with rect.top (viewport-relative), so
    // once the page was scrolled at all the hit test pointed at a row scrollY
    // pixels away from the finger — and this list sits well below the fold.
    panel.querySelectorAll('tr[data-index]').forEach(item => {
        const rect = item.getBoundingClientRect();

        if (touch.clientY >= rect.top && touch.clientY <= rect.bottom) {
            const overIndex = parseInt(item.dataset.index);

            if (overIndex !== touchReorderCurrentIndex && overIndex !== touchReorderStartIndex) {
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
            panel.querySelectorAll('tr[data-index]').forEach(item => {
                item.style.opacity = '';
                item.style.borderTop = '';
                item.classList.remove('dragging');
            });
        }
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
    const currentPlaylistItems = AppState.playlists.getItems('video');
    const currentPlaylistPaths = new Set(currentPlaylistItems.map(item => normalizeVideoPath(item.path)));

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
        const normalizedPath = normalizeVideoPath(path);

        // Check current playlist
        const inCurrentPlaylist = currentPlaylistPaths.has(normalizedPath);

        // Check ALL playlists via globalVideoUsageMap
        const inAnyPlaylist = !!globalVideoUsageMap[normalizedPath];

        // Combine both checks - in current playlist OR in any saved playlist
        const isUsed = inCurrentPlaylist;

        // Use clean title if available from backend, otherwise filename
        const name = video.title || video.filename;

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
                <span class="video-icon">${spriteIcon('ic-videos')}</span>
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
        source: 'library',
        // The drop guards (manager.js setupPlaylistDropZone / the touch drop)
        // require draggedItem.playlistType to match the target panel's type,
        // so that a video cannot be dropped into a game playlist. Neither live
        // dragstart handler set it, so the guard compared undefined === 'video'
        // and every library drag silently added nothing.
        playlistType: 'video'
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
        title: titleForLibraryItem(video),
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
 * Filter library based on search and checkbox. Bound to the search input's
 * 'input' event and the hide-used checkbox's 'change' event (see the
 * addEventListener calls in setup) — referenced by name, not called with (),
 * so a paren-grep will not show its callers.
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
    const currentPlaylistPaths = new Set(currentPlaylistItems.map(item => normalizeVideoPath(item.path)));

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
        const normalizedRomPath = normalizeVideoPath(rom.path);
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
                <span class="video-icon">${spriteIcon('ic-games')}</span>
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
        source: 'library',
        playlistType: 'game'   // see handleLibraryDragStart
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

    // Refuse rather than write an item the kiosk will reject at launch.
    const core = coreForSystem(system);
    if (!core) {
        showNotification(
            `No emulator core is configured for "${system}" — cannot add this ROM.`,
            'error');
        return;
    }

    const config = MEDIA_CONFIG['game'];
    const items = config.getItems();

    // Find first empty slot or add to end
    const emptySlotIndex = items.findIndex(item => item.isEmpty);

    const newItem = {
        source_type: 'emulated_game',
        path: rom.path,  // Use actual path from server
        title: titleForLibraryItem(rom),
        emulator_system: system,
        emulator_core: core
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
