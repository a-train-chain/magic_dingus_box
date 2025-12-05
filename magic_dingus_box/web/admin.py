from __future__ import annotations

import io
import json
import socket
import os
import sys
from pathlib import Path
from typing import Optional

import yaml
from flask import Flask, jsonify, request, send_file


def _sanitize_filename(name: str, allowed_extensions: Optional[list[str]] = None) -> str:
    """Sanitize filename to prevent path traversal attacks.
    
    Args:
        name: Original filename
        allowed_extensions: Optional list of allowed extensions (e.g., ['.yaml', '.yml'])
        
    Returns:
        Sanitized filename (basename only, no path separators)
        
    Raises:
        ValueError: If filename contains path separators or invalid characters
    """
    # Get basename to remove any path components
    basename = os.path.basename(name)
    
    # Reject if still contains path separators (shouldn't happen after basename, but be safe)
    if '/' in basename or '\\' in basename or '..' in basename:
        raise ValueError("Filename contains invalid path characters")
    
    # Validate extension if required
    if allowed_extensions:
        ext = os.path.splitext(basename)[1].lower()
        if ext not in allowed_extensions:
            raise ValueError(f"Filename must have one of these extensions: {', '.join(allowed_extensions)}")
    
    return basename


def get_local_ip() -> str:
    """Get local IP address of this device."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "unknown"


def format_playlist_yaml(data: dict) -> str:
    """Format playlist data as clean YAML matching the expected format.
    
    This ensures the YAML output matches the format that PlaylistLibrary expects,
    with consistent structure and blank fields where no data exists.
    """
    lines = []
    
    # Top-level fields in expected order (always include for consistency)
    lines.append(f"title: {data.get('title', 'Untitled')}")
    lines.append(f"curator: {data.get('curator', 'Unknown')}")
    
    # Always include description field (blank if empty, for consistency)
    description = data.get('description', '')
    if description:
        # If description has a value, include it
        lines.append(f"description: {description}")
    else:
        # If empty, include as empty string for consistency
        lines.append("description: ''")
    
    # Playlist type (video or game)
    lines.append(f"playlist_type: {data.get('playlist_type', 'video')}")
    
    # Loop as lowercase boolean
    loop_value = 'true' if data.get('loop', False) else 'false'
    lines.append(f"loop: {loop_value}")
    
    # Items list
    lines.append("items:")
    
    items = data.get('items', [])
    for item in items:
        # Each item starts with "  - title:"
        lines.append(f"  - title: {item.get('title', 'Untitled')}")
        
        # Artist field (right after title, for music videos)
        artist = item.get('artist', '')
        if artist:
            lines.append(f"    artist: {artist}")
        else:
            lines.append("    artist: ''")
        
        lines.append(f"    source_type: {item.get('source_type', 'local')}")
        
        # Path is required for local/emulated_game types
        if item.get('path'):
            lines.append(f"    path: {item['path']}")
        
        # Optional fields - only include if present
        if item.get('url'):
            lines.append(f"    url: {item['url']}")
        
        if item.get('start') is not None:
            lines.append(f"    start: {item['start']}")
        
        if item.get('end') is not None:
            lines.append(f"    end: {item['end']}")
        
        if item.get('tags'):
            # Format tags as YAML list
            lines.append("    tags:")
            for tag in item['tags']:
                lines.append(f"      - {tag}")
        
        # Emulator fields for games
        if item.get('emulator_core'):
            lines.append(f"    emulator_core: {item['emulator_core']}")
        
        if item.get('emulator_system'):
            lines.append(f"    emulator_system: {item['emulator_system']}")
        
        # Add blank line between items for readability
        if item != items[-1]:  # Not the last item
            lines.append("")
    
    return '\n'.join(lines) + '\n'


def create_app(data_dir: Path, config=None) -> Flask:
    app = Flask(__name__)
    
    # Limit upload sizes; default 2GB (can override via MAGIC_MAX_UPLOAD_MB)
    try:
        max_mb = int(os.getenv("MAGIC_MAX_UPLOAD_MB", "2048"))
        app.config["MAX_CONTENT_LENGTH"] = max_mb * 1024 * 1024
    except Exception:
        app.config["MAX_CONTENT_LENGTH"] = 2 * 1024 * 1024 * 1024
    
    # Optional simple token auth for admin APIs (disabled by default)
    _admin_token = os.getenv("MAGIC_ADMIN_TOKEN")
    if _admin_token:
        @app.before_request
        def _require_token():  # type: ignore[no-redef]
            # Allow static assets without token
            if request.path.startswith("/static/"):
                return None
            if request.headers.get("X-Magic-Token") != _admin_token:
                return {"error": "unauthorized"}, 401
    playlists_dir = data_dir / "playlists"
    media_dir = data_dir / "media"
    roms_dir = data_dir / "roms"
    device_info_file = data_dir / "device_info.json"

    def get_device_info() -> dict:
        """Get device identity and stats."""
        try:
            if device_info_file.exists():
                info = json.loads(device_info_file.read_text())
            else:
                info = {
                    'device_id': 'unknown',
                    'device_name': 'Magic Dingus Box'
                }
            
            # Add runtime info
            info['hostname'] = socket.gethostname()
            info['local_ip'] = get_local_ip()
            
            # Add content stats
            info['stats'] = {
                'playlists': len(list(playlists_dir.glob("*.y*ml"))) if playlists_dir.exists() else 0,
                'videos': len(list(media_dir.rglob("*.mp4"))) if media_dir.exists() else 0,
                'roms': sum(1 for _ in roms_dir.rglob("*") if _.is_file()) if roms_dir.exists() else 0,
            }
            
            return info
        except Exception as e:
            return {'error': str(e), 'device_name': 'Unknown Device'}

    # ===== DEVICE MANAGEMENT =====
    
    @app.get("/admin/device/info")
    def device_info():  # type: ignore[no-redef]
        """Get this device's identity and stats."""
        return jsonify(get_device_info())
    
    @app.post("/admin/device/name")
    def set_device_name():  # type: ignore[no-redef]
        """Set/update device name."""
        data = request.get_json()
        new_name = data.get('name', 'Magic Dingus Box')
        
        try:
            if device_info_file.exists():
                info = json.loads(device_info_file.read_text())
            else:
                import uuid
                info = {'device_id': str(uuid.uuid4())}
            
            info['device_name'] = new_name
            device_info_file.write_text(json.dumps(info, indent=2))
            return {"ok": True, "device_name": new_name}
        except Exception as e:
            return {"error": str(e)}, 500

    # ===== HEALTH CHECK =====
    
    @app.get("/admin/health")
    def health():  # type: ignore[no-redef]
        """Health check endpoint."""
        return {"ok": True}

    # ===== PLAYLIST MANAGEMENT =====
    
    @app.get("/admin/playlists")
    def list_playlists():  # type: ignore[no-redef]
        """List all playlists with metadata."""
        playlists = []
        if not playlists_dir.exists():
            return jsonify(playlists)
        
        for p in sorted(playlists_dir.glob("*.y*ml")):
            try:
                data = yaml.safe_load(p.read_text())
                playlists.append({
                    'filename': p.name,
                    'title': data.get('title', p.stem),
                    'curator': data.get('curator', ''),
                    'description': data.get('description', ''),
                    'item_count': len(data.get('items', [])),
                    'loop': data.get('loop', False),
                    'playlist_type': data.get('playlist_type', 'video'),
                })
            except Exception:
                playlists.append({
                    'filename': p.name,
                    'title': p.stem,
                    'error': 'Failed to parse'
                })
        
        return jsonify(playlists)

    @app.get("/admin/playlists/<name>")
    def get_playlist(name):  # type: ignore[no-redef]
        """Get full playlist content for editing."""
        p = playlists_dir / name
        if not p.exists():
            return {"error": "not found"}, 404
        
        try:
            data = yaml.safe_load(p.read_text())
            return jsonify(data)
        except Exception as e:
            return {"error": str(e)}, 500

    @app.post("/admin/playlists/<name>")
    def put_playlist(name):  # type: ignore[no-redef]
        """Create or update a playlist."""
        try:
            print(f"Saving playlist: {name}", file=sys.stderr)
            # Sanitize filename to prevent path traversal
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])
            
            # Accept JSON or YAML
            if request.is_json:
                data = request.get_json()
                if not data:
                    return {"error": "Invalid JSON body"}, 400
                # Convert to clean YAML matching the expected format
                yaml_content = format_playlist_yaml(data)
            else:
                yaml_content = request.get_data(as_text=True)
                # Validate it's valid YAML
                if not yaml_content.strip():
                     return {"error": "Empty content"}, 400
                yaml.safe_load(yaml_content)
            
            if not yaml_content.strip():
                return {"error": "Generated YAML is empty"}, 400

            p = playlists_dir / safe_name
            # Ensure path stays within playlists_dir (resolve to absolute, then check)
            p_resolved = p.resolve()
            playlists_dir_resolved = playlists_dir.resolve()
            # Note: p might not exist yet, so resolve() might not work as expected for the file itself if it doesn't exist on some python versions/OS, 
            # but usually it resolves the parent. 
            # Safer check:
            if '..' in safe_name or '/' in safe_name or '\\' in safe_name:
                 return {"error": "Invalid filename"}, 400

            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(yaml_content)
            print(f"Successfully saved playlist: {safe_name} ({len(yaml_content)} bytes)", file=sys.stderr)
            return {"ok": True, "filename": safe_name}
        except ValueError as e:
            print(f"ValueError saving playlist {name}: {e}", file=sys.stderr)
            return {"error": str(e)}, 400
        except Exception as e:
            print(f"Error saving playlist {name}: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            return {"error": str(e)}, 400

    @app.delete("/admin/playlists/<name>")
    def delete_playlist(name):  # type: ignore[no-redef]
        """Delete a playlist."""
        try:
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])
        except ValueError as e:
            return {"error": str(e)}, 400
        
        p = playlists_dir / safe_name
        # Ensure path stays within playlists_dir
        p_resolved = p.resolve()
        playlists_dir_resolved = playlists_dir.resolve()
        if not str(p_resolved).startswith(str(playlists_dir_resolved)):
            return {"error": "Invalid path"}, 400
        
        if p.exists():
            p.unlink()
            return {"ok": True}
        return {"error": "not found"}, 404

    # ===== MEDIA MANAGEMENT =====
    
    @app.get("/admin/media")
    def list_media():  # type: ignore[no-redef]
        """List all media files from both uploaded and dev directories."""
        files = []
        
        # Scan the main media directory (uploaded videos)
        if media_dir.exists():
            for ext in ['*.mp4', '*.mkv', '*.avi', '*.mov', '*.webm']:
                files.extend(media_dir.glob(f"**/{ext}"))
        
        # Also scan dev_data/media (existing videos)
        dev_media_dir = data_dir.parent / "dev_data" / "media"
        if dev_media_dir.exists():
            for ext in ['*.mp4', '*.mkv', '*.avi', '*.mov', '*.webm']:
                files.extend(dev_media_dir.glob(f"**/{ext}"))
        
        return jsonify([{
            'filename': f.name,
            'path': str(f.relative_to(data_dir.parent)),  # Relative to parent of data dir
            'size': f.stat().st_size,
            'modified': f.stat().st_mtime
        } for f in sorted(files)])

    @app.post("/admin/upload")
    def upload_media():  # type: ignore[no-redef]
        """Upload video file."""
        if "file" not in request.files:
            return {"error": "file field required"}, 400
        f = request.files["file"]
        
        # Sanitize filename to prevent path traversal
        try:
            safe_filename = _sanitize_filename(f.filename)
        except ValueError as e:
            return {"error": str(e)}, 400
        
        out = media_dir / safe_filename
        # Ensure path stays within media_dir
        out_resolved = out.resolve()
        media_dir_resolved = media_dir.resolve()
        if not str(out_resolved).startswith(str(media_dir_resolved)):
            return {"error": "Invalid path"}, 400
        
        out.parent.mkdir(parents=True, exist_ok=True)
        f.save(str(out))
        return {"ok": True, "path": str(out.relative_to(data_dir.parent))}

    @app.delete("/admin/media/<path:filepath>")
    def delete_media(filepath):  # type: ignore[no-redef]
        """Delete a media file from either uploaded or dev directories."""
        # Try to find the file in either location
        # filepath is relative to /opt/magic_dingus_box (parent of data_dir)
        target = data_dir.parent / filepath
        
        # Security check: ensure path is within allowed directories
        target_resolved = target.resolve()
        data_dir_resolved = data_dir.parent.resolve()
        
        # Check if path is within the parent directory
        if not str(target_resolved).startswith(str(data_dir_resolved)):
            return {"error": "invalid path"}, 400
        
        if target.exists() and target.is_file():
            target.unlink()
            return {"ok": True}
        return {"error": "not found"}, 404

    # ===== ROM MANAGEMENT =====
    
    @app.get("/admin/roms")
    def list_roms():  # type: ignore[no-redef]
        """List ROMs by system."""
        roms = {}
        
        # Helper to scan a directory and add to roms dict
        def scan_dir(base_dir: Path):
            if not base_dir.exists():
                return
            for system_dir in base_dir.iterdir():
                if system_dir.is_dir() and not system_dir.name.startswith('.'):
                    print(f"DEBUG: Scanning system dir: {system_dir}")
                    if system_dir.name not in roms:
                        roms[system_dir.name] = []
                    
                    files = [
                        {
                            'filename': f.name,
                            'path': str(f.relative_to(data_dir.parent)), # Always relative to app root
                            'size': f.stat().st_size
                        }
                        for f in sorted(system_dir.rglob("*"))
                        if f.is_file() and not f.name.startswith('.')
                    ]
                    print(f"DEBUG: Found {len(files)} files in {system_dir.name}")
                    roms[system_dir.name].extend(files)

        # 1. Scan uploaded ROMs
        with open("/tmp/debug_roms.log", "a") as log:
            log.write(f"DEBUG: Scanning uploaded ROMs in {roms_dir}\n")
            scan_dir(roms_dir)
            
            # 2. Scan dev/pre-loaded ROMs
            dev_roms_dir = data_dir.parent / "dev_data" / "roms"
            log.write(f"DEBUG: Scanning dev ROMs in {dev_roms_dir}\n")
            if dev_roms_dir.exists():
                 log.write(f"DEBUG: dev_roms_dir exists. Contents: {[x.name for x in dev_roms_dir.iterdir()]}\n")
            else:
                 log.write(f"DEBUG: dev_roms_dir does NOT exist\n")
            scan_dir(dev_roms_dir)

            log.write(f"DEBUG: Found ROMs: {list(roms.keys())}\n")
        return jsonify(roms)

    @app.post("/admin/upload/rom/<system>")
    def upload_rom(system):  # type: ignore[no-redef]
        """Upload ROM for specific system."""
        if "file" not in request.files:
            print("DEBUG: No file field in request")
            return {"error": "file field required"}, 400
        f = request.files["file"]
        print(f"DEBUG: Uploading file: {f.filename} for system: {system}")
        
        # Sanitize system name and filename to prevent path traversal
        try:
            safe_system = _sanitize_filename(system)
            safe_filename = _sanitize_filename(f.filename)
        except ValueError as e:
            print(f"DEBUG: Sanitization error: {e}")
            return {"error": str(e)}, 400
        
        out = roms_dir / safe_system / safe_filename
        # Ensure path stays within roms_dir
        out_resolved = out.resolve()
        roms_dir_resolved = roms_dir.resolve()
        if not str(out_resolved).startswith(str(roms_dir_resolved)):
            return {"error": "Invalid path"}, 400
        
        out.parent.mkdir(parents=True, exist_ok=True)
        f.save(str(out))
        return {"ok": True, "path": str(out.relative_to(data_dir.parent))}

    @app.delete("/admin/roms/<path:filepath>")
    def delete_rom(filepath):  # type: ignore[no-redef]
        """Delete a ROM file."""
        # filepath is relative to /opt/magic_dingus_box (parent of data_dir)
        target = data_dir.parent / filepath
        
        # Security check: ensure path is within allowed directories
        target_resolved = target.resolve()
        data_dir_resolved = data_dir.parent.resolve()
        
        # Check if path is within the parent directory
        if not str(target_resolved).startswith(str(data_dir_resolved)):
            return {"error": "invalid path"}, 400
            
        # Extra check: must be in roms dir (either data/roms or dev_data/roms)
        is_data_rom = str(target_resolved).startswith(str(roms_dir.resolve()))
        dev_roms_dir = data_dir.parent / "dev_data" / "roms"
        is_dev_rom = dev_roms_dir.exists() and str(target_resolved).startswith(str(dev_roms_dir.resolve()))
        
        if not (is_data_rom or is_dev_rom):
             return {"error": "file is not a ROM"}, 400
        
        if target.exists() and target.is_file():
            target.unlink()
            return {"ok": True}
        return {"error": "not found"}, 404

    # ===== SERVE WEB INTERFACE =====
    
    @app.get("/")
    @app.get("/admin")
    def admin_interface():  # type: ignore[no-redef]
        """Serve the web interface."""
        static_dir = Path(__file__).parent / "static"
        return send_file(static_dir / "index.html")

    @app.route("/static/<path:filename>")
    def serve_static(filename):  # type: ignore[no-redef]
        """Serve static assets."""
        static_dir = Path(__file__).parent / "static"
        return send_file(static_dir / filename)

    return app

