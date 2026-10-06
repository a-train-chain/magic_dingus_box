// Video library: listing, direct/smart uploads (probe + transcode), the
// library filter, file drag-and-drop onto upload inputs, and video delete.
//
// Part of the Content Manager front end; see the load-order note at the top
// of manager.js. Loaded after: manager.js, manager_devices.js.

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
        // Use AppState directly to ensure we check the latest data
        if (AppState.media.videos.length === 0) {
            container.innerHTML = '<p style="color: var(--text-secondary); padding: 1rem;">No videos uploaded yet</p>';
            return;
        }

        // Load all playlists to check video usage
        // Validate currentDevice exists before fetch
        if (!currentDevice || !currentDevice.url) return;

        const playlistsResponse = await fetch(`${currentDevice.url}/admin/playlists`);
        const playlistsResult = await playlistsResponse.json();
        const allPlaylists = playlistsResult.data || playlistsResult;

        // Build a map of video paths to playlists that use them
        const videoUsageMap = {};
        for (const playlist of allPlaylists) {
            try {
                const detailResponse = await fetch(`${currentDevice.url}/admin/playlists/${encodeURIComponent(playlist.filename)}`);
                const detailResult = await detailResponse.json();
                const fullData = detailResult.data || detailResult;
                const items = Array.isArray(fullData.items) ? fullData.items : [];
                items.forEach(item => {
                    if (item && (item.source_type === 'local' || item.source_type === 'emulated_game')) {
                        const normalizedPath = normalizeVideoPath(item.path);
                        if (!normalizedPath) return; // Skip invalid paths

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
                console.warn(`Skipping playlist ${playlist.filename}:`, e);
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
            const videoPath = video.path || `media/${video.filename || ''}`;
            const normalizedVideoPath = normalizeVideoPath(videoPath);
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

        container.innerHTML = createTable(AppState.media.videos);

        // CRITICAL: Update playlist builder's available videos after loading
        renderVideoPlaylistAvailable();
    } catch (e) {
        console.error('Failed to load videos:', e);
        // Only alert if it's not a simple fetch abort/offline issue
        if (e.name !== 'AbortError') {
            console.error('Video load error stack:', e.stack);
        }
    }
}

async function handleDirectUpload(input) {
    if (input.files.length === 0) return;

    const files = Array.from(input.files);
    const targetResolution = document.getElementById('targetResolution')?.value || UPLOAD_CONFIG.TRANSCODE.DEFAULT_RESOLUTION;
    const framingMode = document.getElementById('framingMode')?.value || UPLOAD_CONFIG.TRANSCODE.DEFAULT_FIT_MODE;
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
    if (iconEl) iconEl.innerHTML = spriteIcon('ic-upload');
    if (labelEl) labelEl.textContent = `Uploading ${files.length} file${files.length > 1 ? 's' : ''}`;
    if (progressEl) progressEl.style.width = '0%';
    if (detailsEl) detailsEl.textContent = 'Preparing upload...';
    statusEl?.classList.add('loading');

    // Keep the screen awake for the whole upload+transcode run.
    await acquireWakeLock();
    // Names of files that failed, surfaced to the user at the end instead of
    // just an opaque "N failed" count.
    const failedNames = [];

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
            formData.append('fit_mode', framingMode);
            formData.append('normalize_audio', normalizeAudio ? 'true' : 'false');

            const uploadResponse = await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.open('POST', `${currentDevice.url}/admin/smart-upload`);

                if (csrfToken) {
                    xhr.setRequestHeader('X-CSRF-Token', csrfToken);
                }

                // Hard timeout so a stalled transfer (WiFi hiccup, phone
                // screen-lock suspending the tab mid-upload) can't leave this
                // Promise pending forever with the UI frozen on "Uploading…
                // NNN MB". Without it there was NO recovery but a page reload.
                xhr.timeout = UPLOAD_CONFIG.TIMEOUT_MS;

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
                xhr.ontimeout = () => reject(new Error('Upload timed out — check WiFi and keep this screen awake'));
                xhr.onabort = () => reject(new Error('Upload cancelled'));
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
            failedNames.push(`${truncatedName} (${error.message || 'failed'})`);
        }

        // Update overall progress
        const overallProgress = Math.round(((completed + pendingJobs.length + errors) / files.length) * 50);
        if (progressEl) progressEl.style.width = `${overallProgress}%`;
    }

    // Now poll for pending transcoding jobs
    if (pendingJobs.length > 0) {
        // Change icon and label for transcoding phase
        if (iconEl) iconEl.innerHTML = spriteIcon('ic-hourglass');
        if (labelEl) labelEl.textContent = `Transcoding ${pendingJobs.length} file${pendingJobs.length > 1 ? 's' : ''}`;

        while (pendingJobs.length > 0) {
            await new Promise(resolve => setTimeout(resolve, 1000)); // Poll every second

            for (let i = pendingJobs.length - 1; i >= 0; i--) {
                const job = pendingJobs[i];

                try {
                    const statusResponse = await fetch(`${currentDevice.url}/admin/transcode-status/${job.jobId}`);
                    const statusData = await statusResponse.json();

                    // 404 = the box no longer knows this job. Transcode jobs
                    // live in the web service's memory and its encoder dies
                    // with it, so after a restart (update, power blip, crash)
                    // the job is simply gone — it will never finish. This
                    // used to fall into the "still running" branch below
                    // (statusData.data is undefined on an error body) and
                    // poll forever with the progress bar frozen.
                    if (statusResponse.status === 404 || statusData.error?.code === 'NOT_FOUND') {
                        console.error(`[SmartUpload] ${job.fileName}: job lost (box restarted?)`);
                        errors++;
                        failedNames.push(`${job.truncatedName} (the box restarted while converting it — please upload it again)`);
                        pendingJobs.splice(i, 1);
                        continue;
                    }
                    // Any other error body has no .data either — count it as
                    // a failed poll (capped below), never as "still running".
                    if (!statusResponse.ok || !statusData.data) {
                        throw new Error(`status ${statusResponse.status}`);
                    }

                    if (statusData.data?.status === 'complete') {
                        console.log(`[SmartUpload] ${job.fileName}: Transcode complete`);
                        completed++;
                        pendingJobs.splice(i, 1);
                    } else if (statusData.data?.status === 'error') {
                        console.error(`[SmartUpload] ${job.fileName}: Transcode failed`);
                        errors++;
                        failedNames.push(`${job.truncatedName} (${statusData.data?.message || 'transcode failed'})`);
                        pendingJobs.splice(i, 1);
                    } else {
                        // Still running - update status
                        if (detailsEl && pendingJobs.length === 1) {
                            detailsEl.textContent = statusData.data?.message || `Transcoding: ${job.truncatedName}`;
                        }
                    }
                    job.fetchErrors = 0;
                } catch (e) {
                    console.warn(`[SmartUpload] Error polling job ${job.jobId}:`, e);
                    // Tolerate Wi-Fi blips, but not a box that never answers
                    // again (~2 min at the 1 s poll) — same policy as the
                    // per-file uploader's poller.
                    job.fetchErrors = (job.fetchErrors || 0) + 1;
                    if (job.fetchErrors >= 120) {
                        errors++;
                        failedNames.push(`${job.truncatedName} (lost contact with the box)`);
                        pendingJobs.splice(i, 1);
                    }
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

    // All done! — the screen can sleep again.
    await releaseWakeLock();
    if (progressEl) progressEl.style.width = '100%';
    statusEl?.classList.remove('loading');
    statusEl?.classList.add(errors > 0 ? 'error' : 'complete');

    // Build completion message
    const parts = [];
    if (skipped > 0) parts.push(`${skipped} already compatible`);
    if (completed - skipped > 0) parts.push(`${completed - skipped} transcoded`);
    if (errors > 0) parts.push(`${errors} failed`);

    if (labelEl) labelEl.textContent = errors > 0 ? 'Finished with errors' : 'Complete!';
    if (detailsEl) {
        // Name the failures instead of only a count, so the user knows what
        // to retry. Keep the panel up longer when something failed.
        detailsEl.textContent = failedNames.length
            ? `${parts.join(', ')} — ${failedNames.join('; ')}`
            : (parts.join(', ') || `${plural(completed, 'file')} processed`);
    }

    // Wait a moment then hide — longer when there were errors to read.
    await new Promise(resolve => setTimeout(resolve, errors > 0 ? 8000 : 2500));
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
    // Goes through /admin/smart-upload — the SAME probe-and-transcode flow
    // as the library uploader — never the raw /admin/upload endpoint. This
    // function used to post the raw file, which meant every path that
    // reached it (the drag-and-drop handler on the upload box, the retry
    // button, playlist-editor auto-add) put the user's original phone video
    // straight into data/media/: an iPhone HEVC/4K clip the kiosk cannot
    // decode, playing as a black screen. Meanwhile CLICKING the same upload
    // box went through handleDirectUpload's smart flow and worked — the
    // input method silently decided whether the video would play.
    //
    // Upload maps to 0-40% of the bar and the transcode to 40-100%, so the
    // bar is monotonic across both phases. The playlist auto-add uses the
    // FINAL output path from the server: the transcoded file is .mp4 and
    // may be renamed for uniqueness, so the original filename is wrong to
    // assume.
    return new Promise((resolve) => {
        const { MAX_RETRIES, RETRY_DELAY_MS, TIMEOUT_MS } = UPLOAD_CONFIG;
        let attempts = 0;

        const autoAddFinal = (finalPath) => {
            if (!autoAddToPlaylist) return;
            const videoItem = {
                title: file.name.replace(/\.[^/.]+$/, ""), // Remove extension
                artist: '',
                source_type: 'local',
                path: finalPath
            };

            const config = MEDIA_CONFIG['video'];
            const items = config.getItems();
            items.push(videoItem);
            config.setItems(items);
            renderPlaylistItems('video');

            setTimeout(() => {
                switchSourceTab('library');
            }, 1000);
        };

        // Poll the transcode job to completion. Transcode failures do NOT
        // retry the upload: the server already has the file, and re-sending
        // it would just fail the same encode again. Transient status-fetch
        // errors are tolerated (Wi-Fi blips shouldn't orphan a running job).
        const pollTranscode = async (jobId) => {
            const POLL_MS = 2000;
            const MAX_CONSECUTIVE_FETCH_ERRORS = 30;
            let fetchErrors = 0;
            for (;;) {
                await new Promise(r => setTimeout(r, POLL_MS));
                let data;
                try {
                    const resp = await fetch(`${currentDevice.url}/admin/transcode-status/${jobId}`);
                    // 404 is an ANSWER, not a blip: the box restarted and the
                    // in-memory job (and its encoder) is gone for good. Fail
                    // now with a message the user can act on, instead of
                    // counting it as 30 "transient" errors first.
                    if (resp.status === 404) {
                        const msg = 'The box restarted while converting this video — please upload it again';
                        progressBar.error(msg);
                        resolve({ success: false, error: msg });
                        return;
                    }
                    if (!resp.ok) throw new Error(`status ${resp.status}`);
                    data = (await resp.json()).data;
                    fetchErrors = 0;
                } catch (e) {
                    if (++fetchErrors >= MAX_CONSECUTIVE_FETCH_ERRORS) {
                        progressBar.error('Lost contact with transcode job');
                        resolve({ success: false, error: 'Lost contact with transcode job' });
                        return;
                    }
                    continue;
                }
                // The server's terminal success status is 'complete'
                // (run_transcode_job in admin_media.py; the click path at
                // handleDirectUpload tests the same string). This poller
                // shipped in v1.9.5 checking 'completed', which never
                // matched — the transcode finished server-side but the bar
                // sat at 99%, the promise never resolved, and the upload
                // queue stalled. 'completed' is kept as a defensive alias.
                if (data.status === 'complete' || data.status === 'completed') {
                    const finalPath = data.output_path || `data/media/${data.output_filename}`;
                    progressBar.complete();
                    autoAddFinal(finalPath);
                    resolve({ success: true });
                    return;
                }
                if (data.status === 'failed' || data.status === 'error') {
                    const msg = data.message || 'Transcode failed';
                    progressBar.error(msg);
                    resolve({ success: false, error: msg });
                    return;
                }
                const pct = Math.max(0, Math.min(100, data.progress || 0));
                progressBar.update(40 + Math.round(pct * 0.6));
            }
        };

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
                    const percent = Math.round((e.loaded / e.total) * 40);
                    progressBar.update(percent);
                }
            });

            xhr.addEventListener('load', () => {
                clearTimeout(timeoutId);

                if (xhr.status === 200) {
                    let response;
                    try {
                        response = JSON.parse(xhr.responseText);
                    } catch (e) {
                        console.error('Error parsing upload response:', e);
                        progressBar.error('Bad server response');
                        resolve({ success: false, error: 'Bad server response' });
                        return;
                    }

                    const data = response.data || {};
                    if (data.action === 'transcode' && data.job_id) {
                        progressBar.update(40);
                        pollTranscode(data.job_id);
                    } else {
                        // 'direct' — already kiosk-compatible, moved into place.
                        progressBar.complete();
                        autoAddFinal(data.output_path || `data/media/${data.output_filename || file.name}`);
                        resolve({ success: true });
                    }
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
                xhr.open('POST', `${currentDevice.url}/admin/smart-upload`);
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

function filterMainLibrary(input) {
    const query = input.value.toLowerCase();
    // Scoped to the input's own tab, NOT the whole document. Both the video
    // library (#videoList, manager.js:1502) and the ROM library (#romsList,
    // manager.js:2291) render `<table class="video-library-table">`, so a
    // document-wide query let the All Videos box hide every ROM row too. The
    // ROM Library has no search of its own to clear it, so switching to
    // ROMs & Games afterwards showed column headers and nothing else, with no
    // visible cause.
    const scope = input.closest('.tab-content') || document;
    const rows = scope.querySelectorAll('.video-library-table tbody tr');

    rows.forEach(row => {
        const text = row.textContent.toLowerCase();
        // '' (remove the inline value), NOT 'table-row'. Behaviour is
        // identical — matching rows show, non-matching hide — but hardcoding
        // 'table-row' pinned every row to table layout and made the responsive
        // card layout impossible: any CSS `display` for narrow screens lost to
        // this inline style. Clearing it hands the decision back to the
        // stylesheet, so rows are table-row on desktop and grid cards on a
        // phone. The alternative was `display: grid !important` in CSS, which
        // would have beaten this line and broken filtering outright.
        row.style.display = text.includes(query) ? '' : 'none';
    });
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
