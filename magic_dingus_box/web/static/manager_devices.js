// Device discovery: mDNS/LAN probing, the device list, manual connect, and
// loading a device's content once connected.
//
// Part of the Content Manager front end; see the load-order note at the top
// of manager.js. Loaded after: manager.js.

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
                <h4>${spriteIcon('ic-crt')} ${escapeHtml(device.device_name)}</h4>
                <div class="meta">
                    ${escapeHtml(device.local_ip)} • ${escapeHtml(device.hostname)}
                </div>
            </div>
            <div class="device-stats">
                <div>${plural(device.stats?.playlists, 'playlist')}</div>
                <div>${plural(device.stats?.videos, 'video')}</div>
                <div>${plural(device.stats?.roms, 'ROM')}</div>
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
        // Determine connection type based on IP address. The USB gadget
        // currently assigns 10.55.0.1 via usb-gadget-network.service; older
        // setups used 192.168.7.1. Both are USB gadget subnets — check for
        // the IPs directly and either of the /24 networks in case the
        // service ever assigns a different host address.
        const deviceUrl = currentDevice.url || '';
        const isUSB = (
            deviceUrl.includes('192.168.7.1') ||
            deviceUrl.includes('10.55.0.1') ||
            /\/\/192\.168\.7\.\d+[:\/]/.test(deviceUrl) ||
            /\/\/10\.55\.0\.\d+[:\/]/.test(deviceUrl)
        );
        const connectionType = isUSB ? 'USB' : 'WiFi';
        // Sprite markup, not an emoji glyph. index.html:78 ships
        // <span class="badge-icon"><svg class="ic"><use href="#ic-bolt"/></svg></span>,
        // and writing .textContent below deletes that <svg> outright — so the
        // header chip, which is visible on every tab, lost its sprite the moment
        // updateConnectionBadge() first ran and showed a colour emoji instead.
        const icon = isUSB ? 'ic-plug' : 'ic-antenna';

        // Update badge classes
        badge.classList.remove('disconnected');
        badge.classList.add('connected');
        badge.classList.remove('wifi', 'usb');
        badge.classList.add(isUSB ? 'usb' : 'wifi');

        // Update badge content
        badge.querySelector('.badge-icon').innerHTML = spriteIcon(icon);
        badgeText.textContent = `${currentDevice.device_name} • ${connectionType}`;
    } else {
        // Not connected
        badge.classList.add('disconnected');
        badge.classList.remove('connected', 'wifi', 'usb');
        badge.querySelector('.badge-icon').innerHTML = spriteIcon('ic-bolt');
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
