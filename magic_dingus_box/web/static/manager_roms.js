// Game ROM library: games-tab navigation, ROM listing, uploads and delete.
//
// Part of the Content Manager front end; see the load-order note at the top
// of manager.js. Loaded after: manager.js, manager_devices.js, manager_media.js.

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

                        const uploadCore = coreForSystem(system);
                        if (autoAddToPlaylist && !uploadCore) {
                            // The ROM itself uploaded fine; only the playlist
                            // auto-add is skipped, so say so rather than
                            // silently dropping it.
                            showNotification(
                                `Uploaded, but no emulator core is configured for "${system}" — not added to the playlist.`,
                                'warning');
                        } else if (autoAddToPlaylist) {
                            const playlistItem = {
                                title: file.name.replace(/\.[^/.]+$/, ""),
                                source_type: 'emulated_game',
                                path: result.path || `data/roms/${system}/${file.name}`,
                                emulator_system: system,
                                emulator_core: uploadCore
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
