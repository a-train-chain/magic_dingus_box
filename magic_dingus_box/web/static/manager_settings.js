// Settings tab: backup/restore, health monitoring, OTA updates (check,
// channel, install, rollback), the Network Doctor, Box health and the
// diagnostics download.
//
// Part of the Content Manager front end; see the load-order note at the top
// of manager.js. Loaded after: manager.js, manager_devices.js, manager_media.js, manager_roms.js, manager_playlists.js, manager_import.js.

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

    // Same reasoning as the package upload: a restore ZIP can carry the whole
    // library, and a phone screen lock suspends the tab's JS mid-transfer.
    await acquireWakeLock();

    // fetch() has no timeout of its own, so a stalled transfer would leave
    // "Restoring backup..." on screen forever with no way out but a reload.
    const timeoutMs = packageUploadTimeoutMs(file.size);
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), timeoutMs);

    try {
        const formData = new FormData();
        formData.append('file', file);

        const response = await fetch(`${currentDevice.url}/admin/restore`, {
            method: 'POST',
            headers: {
                'X-CSRF-Token': csrfToken
            },
            body: formData,
            signal: abort.signal
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
        if (e.name === 'AbortError') {
            showRestoreStatus('error',
                `Restore timed out after ${Math.round(timeoutMs / 60000)} minutes — ` +
                'check WiFi, keep this screen awake, and try again.');
        } else {
            showRestoreStatus('error', `Restore failed: ${e.message}`);
        }
    } finally {
        clearTimeout(timer);
        await releaseWakeLock();
        input.value = '';
    }
}

/**
 * Show restore status message
 * @param {string} type - 'success', 'error', or 'loading'
 * @param {string} message - The message to display
 */
function showRestoreStatus(type, message) {
    const el = document.getElementById('mobileRestoreStatus');
    if (!el) return;

    el.textContent = message;
    el.className = 'restore-status visible ' + type;

    // Auto-hide success/error messages after 5 seconds
    if (type !== 'loading') {
        setTimeout(() => { el.classList.remove('visible'); }, 5000);
    }
}

// ===== HEALTH MONITORING FUNCTIONS =====

/** The System Health panel in the Settings tab. */
function healthElement() {
    return document.getElementById('mobileHealthInfo');
}

/**
 * Refresh and display system health information
 */
async function refreshHealthInfo() {
    if (!currentDevice) return;

    const healthEl = healthElement();
    if (!healthEl) return;

    healthEl.innerHTML = '<span class="loading">Loading...</span>';

    try {
        const data = await apiGet(`${currentDevice.url}/admin/health/detailed`);
        displayHealthInfo(data);
    } catch (e) {
        console.error('Health check failed:', e);
        healthEl.innerHTML = '<span class="loading">Failed to load health info</span>';
    }
}

/**
 * Display system health information
 * @param {object} data - Health data from the API
 */
function displayHealthInfo(data) {
    const healthEl = healthElement();
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
            <span class="health-value">${plural(data.content.playlists, 'playlist')}, ${plural(data.content.videos, 'video')}, ${plural(data.content.roms, 'ROM')}</span>
        </div>`;
    }

    healthEl.innerHTML = html;
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
    loadUpdateChannel();
}

/**
 * Show the box's OTA update channel (stable | beta) and sync the Advanced
 * toggle. A box whose updater predates channels has no endpoint: keep the
 * row and the toggle hidden rather than showing a control that cannot work.
 */
async function loadUpdateChannel() {
    if (!currentDevice) return;
    try {
        const data = await apiGet(`${currentDevice.url}/admin/update/channel`);
        renderUpdateChannel(data.channel);
    } catch (e) {
        console.warn('Update channel unavailable:', e.message || e);
        renderUpdateChannel(null);
    }
}

function renderUpdateChannel(channel) {
    const row = document.getElementById('updateChannelRow');
    const label = document.getElementById('updateChannel');
    const advanced = document.getElementById('updateAdvanced');
    const toggle = document.getElementById('betaChannelToggle');
    const known = channel === 'stable' || channel === 'beta';
    if (row) row.style.display = known ? '' : 'none';
    if (advanced) advanced.style.display = known ? '' : 'none';
    if (!known) return;
    if (label) label.textContent = channel === 'beta' ? 'Beta (early updates)' : 'Stable';
    if (toggle) toggle.checked = channel === 'beta';
    // A box left on beta should be impossible to miss.
    if (advanced && channel === 'beta') advanced.open = true;
}

/**
 * Switch the update channel from the Advanced toggle.
 */
async function setUpdateChannel(channel) {
    const toggle = document.getElementById('betaChannelToggle');
    if (!currentDevice) return;
    if (channel === 'beta' && !confirm(
            'Get early (beta) updates on this box?\n\n' +
            'Beta updates are unfinished test builds that may have bugs. ' +
            'You can turn this off any time; the box then waits for the next regular update.')) {
        if (toggle) toggle.checked = false;
        return;
    }
    if (toggle) toggle.disabled = true;
    try {
        const data = await apiPost(`${currentDevice.url}/admin/update/channel`, { channel });
        renderUpdateChannel(data.channel);
        // The available update may differ on the new channel.
        const availableEl = document.getElementById('updateAvailable');
        const installBtn = document.getElementById('installUpdateBtn');
        if (availableEl) availableEl.style.display = 'none';
        if (installBtn) installBtn.style.display = 'none';
        updateData = null;
    } catch (e) {
        alert(`Could not change the update channel: ${e.message}`);
        loadUpdateChannel();
    } finally {
        if (toggle) toggle.disabled = false;
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
    if (iconEl) iconEl.innerHTML = spriteIcon('ic-search');
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
        if (data.channel) renderUpdateChannel(data.channel);

        if (data.update_available) {
            // Show update available
            if (statusEl) statusEl.style.display = 'none';
            if (availableEl) availableEl.style.display = 'block';
            if (installBtn) installBtn.style.display = 'block';

            const newVersionLabel = document.getElementById('newVersionLabel');
            const releaseNotes = document.getElementById('releaseNotes');

            if (newVersionLabel) newVersionLabel.textContent = `v${data.latest_version}` +
                (String(data.latest_version).includes('-beta.') ? ' (beta)' : '');
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
            'web_server_dep': 'Checking Content Manager server...',
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

// ===== NETWORK DOCTOR =====
//
// One tap -> the box probes its own connectivity ladder (join, DHCP,
// router, IPv6 posture, DNS via router, DNS direct, HTTPS, VPN stack)
// and returns a single plain-English verdict. Exists because one hostile
// home router once presented as four unrelated symptoms across four
// screens, and diagnosing it took days of text messages instead of one
// glance at one card.
async function runNetworkDoctor() {
    const btn = document.getElementById('netDoctorBtn');
    const verdictEl = document.getElementById('netDoctorVerdict');
    const checksEl = document.getElementById('netDoctorChecks');
    if (!currentDevice || !verdictEl) return;

    if (btn) { btn.disabled = true; btn.textContent = 'Testing… (~15s)'; }
    verdictEl.style.display = 'block';
    verdictEl.classList.remove('complete', 'error');
    verdictEl.textContent = 'Running connectivity tests…';
    if (checksEl) { checksEl.style.display = 'none'; checksEl.innerHTML = ''; }

    try {
        const resp = await fetch(`${currentDevice.url}/admin/network/doctor`);
        const data = await resp.json();
        if (!data || data.ok !== true) {
            throw new Error((data && data.error && data.error.message) || 'test failed to run');
        }
        const allOk = (data.checks || []).every(c => c.ok);
        verdictEl.classList.add(allOk ? 'complete' : 'error');
        verdictEl.innerHTML =
            `<strong>${escapeHtml(data.verdict || 'Done')}</strong><br>` +
            `${escapeHtml(data.advice || '')}`;
        if (checksEl && Array.isArray(data.checks)) {
            checksEl.style.display = 'block';
            checksEl.innerHTML = data.checks.map(c =>
                `<div style="padding: 0.15rem 0;">` +
                `<span style="color: ${c.ok ? 'var(--success, #4caf50)' : 'var(--error, #f44336)'};">` +
                `${c.ok ? '✓' : '✗'}</span> ` +
                `${escapeHtml(c.detail)}</div>`
            ).join('');
        }
    } catch (e) {
        verdictEl.classList.add('error');
        verdictEl.textContent = `Could not run the network test: ${e.message}`;
    } finally {
        if (btn) { btn.disabled = false; btn.textContent = 'Run Network Test'; }
    }
}

// ===== BOX HEALTH + DIAGNOSTICS =====
//
// Runs the box's own pre-ship acceptance test (verify_box.sh) in the
// background and shows its verdict in plain language, with each section
// expandable. The last result is cached on the box, so the card shows it
// again after a reload without re-running anything.
let _boxHealthPoll = null;

function _boxHealthAge(iso) {
    if (!iso) return '';
    const t = Date.parse(iso);
    if (isNaN(t)) return '';
    const mins = Math.round((Date.now() - t) / 60000);
    if (mins < 1) return 'just now';
    if (mins < 60) return `${mins} min ago`;
    const hrs = Math.round(mins / 60);
    if (hrs < 48) return `${hrs} h ago`;
    return new Date(t).toLocaleDateString();
}

function _renderBoxHealth(status) {
    const verdictEl = document.getElementById('boxHealthVerdict');
    const detailsEl = document.getElementById('boxHealthDetails');
    const btn = document.getElementById('boxHealthBtn');
    const svcRow = document.getElementById('boxHealthServicesRow');
    if (!verdictEl || !detailsEl) return;
    if (svcRow) svcRow.style.display = status.services_check_available ? 'block' : 'none';

    if (status.running) {
        if (btn) { btn.disabled = true; btn.textContent = 'Checking… (~30s)'; }
        verdictEl.style.display = 'block';
        verdictEl.className = 'restore-status loading';
        verdictEl.textContent = status.running_with_services
            ? 'Checking the box and its Movies services — this can take a few minutes…'
            : 'Checking the box…';
        return;
    }
    if (btn) { btn.disabled = false; btn.textContent = 'Run Health Check'; }

    const r = status.result;
    if (!r) {
        verdictEl.style.display = 'none';
        detailsEl.style.display = 'none';
        return;
    }
    const bad = r.error || r.failed > 0;
    verdictEl.style.display = 'block';
    verdictEl.className = 'restore-status ' + (bad ? 'error' : 'success');
    const age = _boxHealthAge(r.finished_at);
    let sub = '';
    if (r.error) {
        sub = escapeHtml(r.error);
    } else if (r.failed > 0) {
        sub = 'Open the sections marked ✗ below to see what needs attention.';
    } else if (r.warnings > 0) {
        sub = 'Notes (!) are usually expected — for example no movie drive plugged in.';
    }
    verdictEl.innerHTML =
        `<strong>${escapeHtml(r.headline || '')}</strong>` +
        (sub ? `<br>${sub}` : '') +
        (age ? `<br><span style="opacity: 0.8; font-size: 0.8rem;">Checked ${escapeHtml(age)}` +
               `${r.with_services ? ' (including Movies services)' : ''}</span>` : '');

    const sections = Array.isArray(r.sections) ? r.sections : [];
    if (!sections.length) { detailsEl.style.display = 'none'; return; }
    const icon = { pass: '✓', fail: '✗', warn: '!' };
    const color = { pass: 'var(--success)', fail: 'var(--error)', warn: 'var(--accent)' };
    detailsEl.style.display = 'block';
    detailsEl.innerHTML = sections.map(s => {
        const checks = s.checks || [];
        const fails = checks.filter(c => c.level === 'fail').length;
        const warns = checks.filter(c => c.level === 'warn').length;
        const mark = fails ? 'fail' : (warns ? 'warn' : 'pass');
        const tally = fails ? `${fails} problem${fails > 1 ? 's' : ''}`
                    : warns ? `${warns} note${warns > 1 ? 's' : ''}` : 'all good';
        const rows = checks.map(c =>
            `<div style="padding: 0.15rem 0 0.15rem 1rem;">` +
            `<span style="color: ${color[c.level] || 'inherit'};">${icon[c.level] || '•'}</span> ` +
            `${escapeHtml(c.text)}` +
            (c.details && c.details.length
                ? `<pre style="margin: 0.25rem 0 0 1rem; font-size: 0.75rem; white-space: pre-wrap;">${escapeHtml(c.details.join('\n'))}</pre>`
                : '') +
            `</div>`).join('');
        // A VPN tunnel note is the one WARN an owner can act on (and the
        // headline names it), so show its numbers without a click.
        const open = fails || (warns && s.name === 'VPN tunnel');
        return `<details style="padding: 0.2rem 0;"${open ? ' open' : ''}>` +
               `<summary style="cursor: pointer;">` +
               `<span style="color: ${color[mark]};">${icon[mark]}</span> ` +
               `${escapeHtml(s.name)} <span style="opacity: 0.75;">— ${tally}</span></summary>` +
               rows + `</details>`;
    }).join('');
}

async function loadBoxHealth() {
    if (!currentDevice) return;
    try {
        const status = await apiGet(`${currentDevice.url}/admin/health/status`);
        _renderBoxHealth(status);
        if (status.running && !_boxHealthPoll) {
            _boxHealthPoll = setInterval(_pollBoxHealth, 2000);
        }
    } catch (e) {
        console.warn('Box health status unavailable:', e.message || e);
    }
}

async function _pollBoxHealth() {
    if (!currentDevice) { clearInterval(_boxHealthPoll); _boxHealthPoll = null; return; }
    try {
        const status = await apiGet(`${currentDevice.url}/admin/health/status`);
        _renderBoxHealth(status);
        if (!status.running) { clearInterval(_boxHealthPoll); _boxHealthPoll = null; }
    } catch (e) {
        // Transient (e.g. the web service restarting) — keep polling.
    }
}

async function runBoxHealth() {
    if (!currentDevice) return;
    const verdictEl = document.getElementById('boxHealthVerdict');
    const withSvc = document.getElementById('boxHealthWithServices');
    try {
        if (!csrfToken) await fetchCsrfToken();
        const status = await apiPost(`${currentDevice.url}/admin/health/run`,
            { with_services: !!(withSvc && withSvc.checked &&
                                 withSvc.closest('label').style.display !== 'none') });
        _renderBoxHealth(status);
        if (!_boxHealthPoll) _boxHealthPoll = setInterval(_pollBoxHealth, 2000);
    } catch (e) {
        if (verdictEl) {
            verdictEl.style.display = 'block';
            verdictEl.className = 'restore-status error';
            verdictEl.textContent = `Could not start the health check: ${e.message}`;
        }
    }
}

function downloadDiagnostics() {
    if (!currentDevice) return;
    const btn = document.getElementById('diagBundleBtn');
    if (btn) {
        // The box takes ~10 s to gather everything before the download
        // starts; say so instead of looking dead.
        btn.disabled = true;
        btn.textContent = 'Preparing… (~10s)';
        setTimeout(() => { btn.disabled = false; btn.textContent = 'Download Diagnostics'; }, 12000);
    }
    const link = document.createElement('a');
    link.href = `${currentDevice.url}/admin/diagnostics/bundle`;
    link.download = '';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}
