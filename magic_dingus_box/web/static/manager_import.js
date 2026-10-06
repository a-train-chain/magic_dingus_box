// Playlist import: single YAML files and playlist packages (ZIP), including
// reading the package title client-side before uploading it.
//
// Part of the Content Manager front end; see the load-order note at the top
// of manager.js. Loaded after: manager.js, manager_devices.js, manager_media.js, manager_roms.js, manager_playlists.js.

// ===== PLAYLIST IMPORT =====

// TEMPORARY COPY — an import writes the playlist to disk, but the kiosk only
// re-reads playlists at startup, so nothing shows up on the TV until the box
// restarts. Nothing in this UI said so. Once a box ships the kiosk change that
// watches for the playlists_reload_request marker, set this to '' (one edit)
// and every import message drops the sentence.
// The server now writes a playlists_reload_request marker on every import, and
// a current kiosk build picks it up within about a second. Boxes still running
// an older build ignore the marker and keep showing the previous playlists
// until they restart, so the copy has to be true for both — saying "restart
// the box" outright would be wrong on an updated box, and saying nothing at
// all leaves an older box looking like the import silently failed.
// ONE-LINE CLEANUP: once every fielded box runs a kiosk build with the
// playlists_reload_request poll, replace this with '' and the sentence
// disappears from every import toast.
const IMPORT_TV_RESTART_NOTE =
    ' It should appear on the TV within a few seconds' +
    ' — if the box is on an older build, restart it.';

const BYTES_PER_GB = 1024 * 1024 * 1024;

/**
 * "5.2 GB" / "412 MB" for a number of gigabytes, or "an unknown amount" when
 * the box didn't say. Drops to MB below 1 GB — a small test package rendered
 * as "0.0 GB" otherwise, which reads like a bug.
 */
function formatGb(gb) {
    const v = Number(gb);
    if (!Number.isFinite(v)) return 'an unknown amount';
    if (v >= 1) return `${v.toFixed(1)} GB`;
    const mb = v * 1024;
    return `${mb >= 1 ? Math.round(mb) : mb.toFixed(1)} MB`;
}

// ===== READING THE PACKAGE WITHOUT UPLOADING IT =====
//
// The import endpoint derives the destination playlist filename from the
// `title` inside the ZIP's playlist.yaml, so that title is the only thing that
// can answer "would this clobber something?" before the bytes move. Everything
// below reads the ZIP's central directory and that one small entry via
// File.slice(), so the browser never touches the other 5 GB.

const ZIP_EOCD_SIG = 0x06054b50;
const ZIP64_LOCATOR_SIG = 0x07064b50;
const ZIP64_EOCD_SIG = 0x06064b50;
const ZIP_CD_ENTRY_SIG = 0x02014b50;
const ZIP_LOCAL_SIG = 0x04034b50;
const ZIP_U32_MAX = 0xffffffff;

async function _zipView(file, start, end) {
    return new DataView(await file.slice(start, end).arrayBuffer());
}

/**
 * Parse a ZIP's central directory into {name, method, compressedSize, localOffset}.
 * @returns {Promise<Array|null>} null if this isn't a ZIP we can read
 */
async function _zipReadCentralDirectory(file) {
    // The end-of-central-directory record is the last 22 bytes plus up to 64 KB
    // of trailing archive comment, so scan backwards through that window.
    const tailLen = Math.min(file.size, 22 + 65535);
    const tail = await _zipView(file, file.size - tailLen, file.size);

    let eocd = -1;
    for (let i = tail.byteLength - 22; i >= 0; i--) {
        if (tail.getUint32(i, true) === ZIP_EOCD_SIG) { eocd = i; break; }
    }
    if (eocd < 0) return null;

    let cdSize = tail.getUint32(eocd + 12, true);
    let cdOffset = tail.getUint32(eocd + 16, true);

    // ZIP64. Anything past 4 GB parks 0xFFFFFFFF in the classic fields and puts
    // the real 64-bit ones in a separate record — and the two packages this
    // whole change exists for (Cartoons 5.2 GB, Atmosphere 4.1 GB) are exactly
    // the ones that go down this branch.
    if (cdSize === ZIP_U32_MAX || cdOffset === ZIP_U32_MAX) {
        const loc = eocd - 20;
        if (loc < 0 || tail.getUint32(loc, true) !== ZIP64_LOCATOR_SIG) return null;
        const z64Offset = Number(tail.getBigUint64(loc + 8, true));
        const z64 = await _zipView(file, z64Offset, z64Offset + 56);
        if (z64.getUint32(0, true) !== ZIP64_EOCD_SIG) return null;
        cdSize = Number(z64.getBigUint64(40, true));
        cdOffset = Number(z64.getBigUint64(48, true));
    }

    const cd = await _zipView(file, cdOffset, cdOffset + cdSize);
    const decoder = new TextDecoder('utf-8');
    const entries = [];
    let p = 0;

    while (p + 46 <= cd.byteLength && cd.getUint32(p, true) === ZIP_CD_ENTRY_SIG) {
        const method = cd.getUint16(p + 10, true);
        let compressedSize = cd.getUint32(p + 20, true);
        let uncompressedSize = cd.getUint32(p + 24, true);
        const nameLen = cd.getUint16(p + 28, true);
        const extraLen = cd.getUint16(p + 30, true);
        const commentLen = cd.getUint16(p + 32, true);
        let localOffset = cd.getUint32(p + 42, true);
        const name = decoder.decode(new Uint8Array(cd.buffer, cd.byteOffset + p + 46, nameLen));

        // The ZIP64 extra field carries ONLY the fields that overflowed, in a
        // fixed order, so which 8-byte values are present depends on which
        // 0xFFFFFFFF sentinels we just read.
        if (uncompressedSize === ZIP_U32_MAX || compressedSize === ZIP_U32_MAX ||
                localOffset === ZIP_U32_MAX) {
            let e = p + 46 + nameLen;
            const extraEnd = e + extraLen;
            while (e + 4 <= extraEnd) {
                const id = cd.getUint16(e, true);
                const size = cd.getUint16(e + 2, true);
                if (id === 0x0001) {
                    let q = e + 4;
                    if (uncompressedSize === ZIP_U32_MAX) { uncompressedSize = Number(cd.getBigUint64(q, true)); q += 8; }
                    if (compressedSize === ZIP_U32_MAX) { compressedSize = Number(cd.getBigUint64(q, true)); q += 8; }
                    if (localOffset === ZIP_U32_MAX) { localOffset = Number(cd.getBigUint64(q, true)); q += 8; }
                    break;
                }
                e += 4 + size;
            }
        }

        entries.push({ name, method, compressedSize, uncompressedSize, localOffset });
        p += 46 + nameLen + extraLen + commentLen;
    }

    return entries;
}

/** Decompress one small ZIP entry to text. Returns null for anything exotic. */
async function _zipReadEntryText(file, entry) {
    // The local header repeats the name and extra fields at its OWN lengths;
    // the payload starts after those, not after the central directory's copy.
    const head = await _zipView(file, entry.localOffset, entry.localOffset + 30);
    if (head.getUint32(0, true) !== ZIP_LOCAL_SIG) return null;
    const dataStart = entry.localOffset + 30 + head.getUint16(26, true) + head.getUint16(28, true);
    const blob = file.slice(dataStart, dataStart + entry.compressedSize);

    if (entry.method === 0) {
        return new TextDecoder('utf-8').decode(new Uint8Array(await blob.arrayBuffer()));
    }
    // Stored and deflate are the only methods Python's zipfile (which builds
    // every package we import) ever writes.
    if (entry.method !== 8 || typeof DecompressionStream === 'undefined') return null;
    const stream = blob.stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new TextDecoder('utf-8').decode(new Uint8Array(await new Response(stream).arrayBuffer()));
}

/**
 * Pull the top-level `title:` out of a playlist YAML.
 *
 * Deliberately not a YAML parser: the file is machine-written, we need exactly
 * one scalar, and a wrong answer costs nothing — the import falls back to
 * letting the server decide. Item titles are indented, so anchoring to column 0
 * is what keeps this from picking up the first track's name.
 */
function _yamlTopLevelTitle(text) {
    const m = text.match(/^title:[ \t]*(.*)$/m);
    if (!m) return null;
    let v = m[1].trim();
    if (v.length >= 2 && v.startsWith("'") && v.endsWith("'")) {
        v = v.slice(1, -1).replace(/''/g, "'");
    } else if (v.length >= 2 && v.startsWith('"') && v.endsWith('"')) {
        v = v.slice(1, -1).replace(/\\(["\\])/g, '$1');
    }
    return v.trim() || null;
}

/**
 * Read the playlist title out of a package ZIP without uploading it.
 * @param {File} file
 * @returns {Promise<string|null>} the title, or null if it can't be read
 */
async function readPackagePlaylistTitle(file) {
    try {
        const entries = await _zipReadCentralDirectory(file);
        if (!entries) return null;
        const entry = entries.find(e => {
            const base = e.name.split('/').pop().toLowerCase();
            return base === 'playlist.yaml' || base === 'playlist.yml';
        });
        if (!entry) return null;
        const yamlText = await _zipReadEntryText(file, entry);
        return yamlText ? _yamlTopLevelTitle(yamlText) : null;
    } catch (e) {
        console.warn('Could not read the playlist title out of the package:', e);
        return null;
    }
}

/**
 * Last-resort title guess from the package filename, for browsers too old for
 * DecompressionStream. retro_ripper names its exports
 * MDB_<title with spaces as underscores>_<YYYY-MM-DD>.zip, and the import
 * endpoint's own sanitiser also maps spaces to underscores, so the guess
 * usually resolves to the same playlist file even though it is not the
 * literal title.
 */
function packageTitleFromFilename(name) {
    const base = name
        .replace(/\.zip$/i, '')
        .replace(/^MDB_/i, '')
        .replace(/_\d{4}-\d{2}-\d{2}$/, '')
        .trim();
    return base || null;
}

/**
 * Ask the box what this import would do BEFORE sending the bytes.
 * @returns {Promise<object|null>} null when the box is too old to answer —
 *          callers must read that as "no information", not "no conflict".
 */
async function precheckPackageImport(title, sizeBytes) {
    if (!title) return null;
    try {
        if (!csrfToken) await fetchCsrfToken();
        return await apiPost(
            `${currentDevice.url}/admin/playlists/import-package/precheck`,
            { title: title, size_bytes: sizeBytes }
        );
    } catch (e) {
        console.warn('Package precheck unavailable:', e.message || e);
        return null;
    }
}

/**
 * How long to let a package upload run before giving up.
 *
 * The flat 15-minute UPLOAD_CONFIG.TIMEOUT_MS is sized for a single video; a
 * 5 GB package over WiFi plus the Pi's extract-and-fsync pass is an hour and a
 * half. Budget ~1 MB/s end to end plus ten minutes of processing slack, and
 * never go below the standard allowance.
 */
function packageUploadTimeoutMs(sizeBytes) {
    const transferMs = Math.ceil((sizeBytes / (1024 * 1024)) * 1000);
    return Math.max(UPLOAD_CONFIG.TIMEOUT_MS, transferMs + 10 * 60 * 1000);
}

/**
 * Success copy for an import.
 *
 * The old toast reported only what was imported, so re-importing an unchanged
 * package read "0 videos uploaded" with nothing to explain it — the files were
 * already there and the server said so in videos_skipped/roms_skipped, which
 * this used to throw away.
 */
function packageImportSummary(data) {
    const parts = [plural(data.item_count, 'item')];
    if (data.videos_imported) parts.push(`${plural(data.videos_imported, 'video')} uploaded`);
    if (data.videos_skipped) parts.push(`${plural(data.videos_skipped, 'video')} already on the box`);
    if (data.roms_imported) parts.push(`${plural(data.roms_imported, 'game')} uploaded`);
    if (data.roms_skipped) parts.push(`${plural(data.roms_skipped, 'game')} already on the box`);
    if (parts.length === 1) parts.push('no new files needed');
    return `Imported "${data.playlist_title}": ${parts.join(', ')}.${IMPORT_TV_RESTART_NOTE}`;
}

/** Refresh the library the package actually belongs to. */
async function refreshAfterPackageImport(type) {
    await loadExistingPlaylists();
    if (type === 'game') {
        await loadROMs();
    } else {
        await loadVideos();
    }
    if (typeof loadMediaList === 'function') {
        await loadMediaList();
    }
}

// ===== PACKAGE IMPORT (ZIP with videos + playlist) =====

function importPackage(type) {
    // Create a hidden file input for ZIP files
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.zip';
    input.style.display = 'none';
    document.body.appendChild(input);

    input.onchange = async () => {
        if (input.files.length > 0) {
            await uploadPackage(input.files[0], type);
        }
        document.body.removeChild(input);
    };

    input.click();
}

async function uploadPackage(file, type) {
    if (!currentDevice) {
        alert('Please select a device first');
        return;
    }

    // Settle the overwrite question BEFORE a single byte moves. import-package
    // only reports a collision once Werkzeug has consumed the entire multipart
    // body, so answering it from the 409 meant pushing Cartoons (5.2 GB) over
    // WiFi twice to import it once.
    const title = (await readPackagePlaylistTitle(file)) || packageTitleFromFilename(file.name);
    const pre = await precheckPackageImport(title, file.size);
    const sizeText = formatGb(file.size / BYTES_PER_GB);
    let overwrite = false;

    if (pre) {
        if (pre.max_upload_mb && file.size > pre.max_upload_mb * 1024 * 1024) {
            alert(
                'This package is bigger than the box will accept.\n\n' +
                `Package: ${sizeText}\n` +
                `Box limit: ${formatGb(pre.max_upload_mb / 1024)}\n\n` +
                'Nothing has been uploaded.'
            );
            return;
        }

        if (pre.enough_space === false) {
            alert(
                'Not enough room on the box.\n\n' +
                `This package is ${sizeText} and the box has only ${formatGb(pre.free_gb)} free.\n\n` +
                'Delete some videos or an old playlist and try again. Nothing has been uploaded.'
            );
            return;
        }

        if (pre.exists) {
            // Name it by title and only add the filename when the two differ,
            // so this never reads "Cartoons.yaml" (Cartoons.yaml)".
            const existing = pre.existing_title || pre.existing_filename || title;
            const where = (pre.existing_filename && pre.existing_filename !== existing)
                ? ` (${pre.existing_filename})` : '';
            const count = (pre.existing_item_count === null || pre.existing_item_count === undefined)
                ? ''
                : `, ${plural(pre.existing_item_count, 'item')}`;
            const confirmed = confirm(
                `"${existing}"${where} is already on the box${count}.\n\n` +
                `Importing "${title}" (${sizeText}) will replace it.\n\n` +
                'OK to replace it, Cancel to stop. Nothing has been uploaded yet.'
            );
            if (!confirmed) return;
            overwrite = true;
        }
    }

    await sendPackageUpload(file, type, overwrite);
}

/**
 * The one place a package ZIP is actually sent.
 *
 * `overwrite` is decided by the caller (from the precheck, or from a 409 on a
 * box too old to have one). There used to be a second near-identical copy of
 * this for the overwrite path, which is how it drifted: it dropped the wake
 * lock, the progress-estimate copy and the `type`-aware refresh.
 */
async function sendPackageUpload(file, type, overwrite) {
    const fileSizeMB = (file.size / (1024 * 1024)).toFixed(1);
    const { MAX_RETRIES, RETRY_DELAY_MS } = UPLOAD_CONFIG;
    const timeoutMs = packageUploadTimeoutMs(file.size);
    const timeoutMins = Math.round(timeoutMs / 60000);

    const overlay = document.createElement('div');
    overlay.className = 'loading-overlay';
    overlay.innerHTML = `
        <div class="loading-spinner"></div>
        <div class="loading-content">
            <div class="loading-title">${overwrite ? 'Replacing Package' : 'Importing Package'}</div>
            <div class="loading-status" id="package-status">Preparing upload...</div>
            <div class="loading-details" id="package-filename">${escapeHtml(file.name)}</div>
            <div class="upload-progress-container">
                <div class="upload-progress-bar">
                    <div class="upload-progress-fill" id="package-progress-fill"></div>
                </div>
                <div class="upload-progress-text">
                    <span id="package-uploaded">0 MB</span>
                    <span class="upload-progress-percent" id="package-percent">0%</span>
                    <span id="package-total">${fileSizeMB} MB</span>
                </div>
            </div>
        </div>
    `;
    document.body.appendChild(overlay);

    // A multi-GB transfer runs for many minutes and a phone screen lock
    // suspends the tab's JS, which stalls or kills it. Held until the request
    // settles — every exit below goes through settle().
    await acquireWakeLock();

    let attempts = 0;

    return new Promise((resolve) => {
        const settle = async (after) => {
            await releaseWakeLock();
            if (document.body.contains(overlay)) document.body.removeChild(overlay);
            try {
                if (after) await after();
            } finally {
                resolve();
            }
        };

        // Transport-level failures only (WiFi drop, a stall long enough to trip
        // the timeout). Same shape as uploadSingleFile()'s retry: linear
        // backoff, capped at UPLOAD_CONFIG.MAX_RETRIES. HTTP responses are
        // answers, not failures, and are never retried here.
        const retryOrFail = (message) => {
            if (attempts < MAX_RETRIES) {
                const statusEl = document.getElementById('package-status');
                const fillEl = document.getElementById('package-progress-fill');
                if (statusEl) statusEl.textContent = `${message} — retrying (${attempts} of ${MAX_RETRIES})...`;
                if (fillEl) { fillEl.classList.remove('indeterminate'); fillEl.style.width = '0%'; }
                setTimeout(attemptUpload, RETRY_DELAY_MS * attempts);
                return;
            }
            settle(async () => {
                alert(`${message}\n\nGave up after ${attempts} attempts. Nothing was imported.`);
            });
        };

        const attemptUpload = async () => {
            attempts++;

            try {
                if (!csrfToken) await fetchCsrfToken();

                const formData = new FormData();
                formData.append('file', file);

                const xhr = new XMLHttpRequest();
                xhr.open('POST', `${currentDevice.url}/admin/playlists/import-package?overwrite=${overwrite ? 'true' : 'false'}`);
                if (csrfToken) xhr.setRequestHeader('X-CSRF-Token', csrfToken);

                // Without a timeout a stalled transfer leaves this overlay up
                // forever with no recovery but a page reload. Scaled by size:
                // the flat 15-minute allowance is not remotely enough for 5 GB.
                xhr.timeout = timeoutMs;

                xhr.upload.onprogress = (e) => {
                    if (!e.lengthComputable) return;
                    const percent = Math.round((e.loaded / e.total) * 100);
                    const uploadedMB = (e.loaded / (1024 * 1024)).toFixed(1);

                    const progressFill = document.getElementById('package-progress-fill');
                    const percentEl = document.getElementById('package-percent');
                    const uploadedEl = document.getElementById('package-uploaded');
                    const statusEl = document.getElementById('package-status');
                    const detailsEl = document.getElementById('package-filename');

                    if (progressFill) progressFill.style.width = `${percent}%`;
                    if (percentEl) percentEl.textContent = `${percent}%`;
                    if (uploadedEl) uploadedEl.textContent = `${uploadedMB} MB`;

                    if (!statusEl) return;
                    if (percent < 100) {
                        statusEl.textContent = 'Uploading...';
                        return;
                    }

                    // At 100% the bytes are sent but the Pi is still extracting.
                    // Assume ~10MB/s processing speed.
                    const totalMB = e.total / (1024 * 1024);
                    const estSeconds = Math.max(10, Math.ceil(totalMB / 10)); // Min 10s
                    const timeStr = estSeconds > 60
                        ? `~${plural(Math.ceil(estSeconds / 60), 'min')}`
                        : `~${plural(estSeconds, 'second')}`;
                    statusEl.textContent = `Processing on device... (Est. time: ${timeStr})`;
                    if (progressFill) progressFill.classList.add('indeterminate');
                    if (detailsEl) detailsEl.textContent = 'Extracting and organizing files... do not close.';
                };

                xhr.onload = async () => {
                    if (xhr.status === 200) {
                        let data = null;
                        try { data = JSON.parse(xhr.responseText).data; } catch (e) { /* handled below */ }
                        await settle(async () => {
                            if (!data) {
                                alert('The import finished but the box sent a response we could not read.');
                                return;
                            }
                            showNotification(packageImportSummary(data), 'success', 9000);
                            await refreshAfterPackageImport(type);
                        });
                        return;
                    }

                    let err = null;
                    try { err = JSON.parse(xhr.responseText); } catch (e) { /* non-JSON body */ }
                    const details = err?.error?.details || {};

                    if (xhr.status === 409) {
                        // The precheck should have caught this. We still land
                        // here on a box that predates the precheck endpoint, or
                        // when the title inside the ZIP could not be read — so
                        // ask with the server's own numbers rather than guess.
                        await settle(async () => {
                            const named = details.existing_title || details.existing_filename;
                            const subject = named ? `"${named}"` : 'A playlist with the same name';
                            const where = (details.existing_filename && details.existing_filename !== named)
                                ? ` (${details.existing_filename})` : '';
                            const incoming = details.incoming_title ? `"${details.incoming_title}"` : 'this package';
                            if (confirm(
                                `${subject}${where} is already on the box.\n\n` +
                                `Importing ${incoming} will replace it. Replace it?`
                            )) {
                                await sendPackageUpload(file, type, true);
                            }
                        });
                        return;
                    }

                    if (xhr.status === 507 || err?.error?.code === 'INSUFFICIENT_STORAGE') {
                        await settle(async () => {
                            alert(
                                'Not enough room on the box.\n\n' +
                                `This import needs about ${formatGb(details.needed_gb)} and only ` +
                                `${formatGb(details.free_gb)} is free.\n\n` +
                                'Delete some videos or an old playlist and try again. Nothing was imported.'
                            );
                        });
                        return;
                    }

                    await settle(async () => {
                        alert(`Import failed: ${err?.error?.message || xhr.statusText || `HTTP ${xhr.status}`}`);
                    });
                };

                xhr.onerror = () => retryOrFail('Network error uploading package');
                xhr.ontimeout = () => retryOrFail(
                    `Upload timed out after ${timeoutMins} minutes — check WiFi and keep this screen awake`
                );
                xhr.onabort = () => settle();

                xhr.send(formData);
            } catch (e) {
                console.error('Package import error:', e);
                retryOrFail('Could not start the upload');
            }
        };

        attemptUpload();
    });
}
