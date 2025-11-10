from __future__ import annotations

import io
import json
import socket
import os
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
            # Sanitize filename to prevent path traversal
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])
            
            # Accept JSON or YAML
            if request.is_json:
                data = request.get_json()
                # Convert to clean YAML matching the expected format
                yaml_content = format_playlist_yaml(data)
            else:
                yaml_content = request.get_data(as_text=True)
                # Validate it's valid YAML
                yaml.safe_load(yaml_content)
            
            p = playlists_dir / safe_name
            # Ensure path stays within playlists_dir (resolve to absolute, then check)
            p_resolved = p.resolve()
            playlists_dir_resolved = playlists_dir.resolve()
            if not str(p_resolved).startswith(str(playlists_dir_resolved)):
                return {"error": "Invalid path"}, 400
            
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(yaml_content)
            return {"ok": True, "filename": safe_name}
        except ValueError as e:
            return {"error": str(e)}, 400
        except Exception as e:
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
        """List all media files."""
        files = []
        if not media_dir.exists():
            return jsonify(files)
        
        for ext in ['*.mp4', '*.mkv', '*.avi', '*.mov', '*.webm']:
            files.extend(media_dir.glob(f"**/{ext}"))
        
        return jsonify([{
            'filename': f.name,
            'path': str(f.relative_to(data_dir)),
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
        return {"ok": True, "path": str(out.relative_to(data_dir))}

    @app.delete("/admin/media/<path:filepath>")
    def delete_media(filepath):  # type: ignore[no-redef]
        """Delete a media file."""
        target = media_dir / filepath
        if target.exists() and target.is_relative_to(media_dir):
            target.unlink()
            return {"ok": True}
        return {"error": "not found"}, 404

    # ===== ROM MANAGEMENT =====
    
    @app.get("/admin/roms")
    def list_roms():  # type: ignore[no-redef]
        """List ROMs by system."""
        roms = {}
        if not roms_dir.exists():
            return jsonify(roms)
        
        for system_dir in roms_dir.iterdir():
            if system_dir.is_dir():
                roms[system_dir.name] = [
                    {
                        'filename': f.name,
                        'path': str(f.relative_to(data_dir)),
                        'size': f.stat().st_size
                    }
                    for f in sorted(system_dir.rglob("*"))
                    if f.is_file() and not f.name.startswith('.')
                ]
        return jsonify(roms)

    @app.post("/admin/upload/rom/<system>")
    def upload_rom(system):  # type: ignore[no-redef]
        """Upload ROM for specific system."""
        if "file" not in request.files:
            return {"error": "file field required"}, 400
        f = request.files["file"]
        
        # Sanitize system name and filename to prevent path traversal
        try:
            safe_system = _sanitize_filename(system)
            safe_filename = _sanitize_filename(f.filename)
        except ValueError as e:
            return {"error": str(e)}, 400
        
        out = roms_dir / safe_system / safe_filename
        # Ensure path stays within roms_dir
        out_resolved = out.resolve()
        roms_dir_resolved = roms_dir.resolve()
        if not str(out_resolved).startswith(str(roms_dir_resolved)):
            return {"error": "Invalid path"}, 400
        
        out.parent.mkdir(parents=True, exist_ok=True)
        f.save(str(out))
        return {"ok": True, "path": str(out.relative_to(data_dir))}

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

