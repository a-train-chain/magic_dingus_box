"""Video library routes: list/upload/delete media and the transcode
pipeline (upload-and-transcode, smart-upload, transcode-status), plus the
per-board transcode policy (_detect_pi_model, the TEST-ONLY
MDB_PLATFORM_POLICY_OVERRIDE resolution, concurrent-transcode cap).
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path
from typing import Optional

from flask import request

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import (
        _sanitize_filename, _staged_save_upload, error_response,
        success_response,
    )
    from admin_security import _is_within
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import (
        _sanitize_filename, _staged_save_upload, error_response,
        success_response,
    )
    from .admin_security import _is_within


def _max_transcodes_for(pi_model: str, env_value: Optional[str]) -> int:
    """Concurrent-transcode cap: env override, else 2 on Pi 5, else 1."""
    if env_value is not None and str(env_value).strip():
        try:
            return max(1, int(env_value))
        except ValueError:
            pass
    return 2 if pi_model == "pi5" else 1


def _detect_pi_model() -> str:
    """Return 'pi5', 'pi4', or 'unknown' from the device-tree model.

    Python-side mirror of the C++ kiosk's PlatformProfile detection
    (dual-board contract: board differences resolve at RUNTIME, never
    at deploy time — one golden image serves both boards). Module-level
    so tests can stand in for a board."""
    try:
        model = Path('/proc/device-tree/model').read_text(errors='ignore')
    except OSError:
        return 'unknown'
    if 'Raspberry Pi 5' in model:
        return 'pi5'
    if 'Raspberry Pi 4' in model:
        return 'pi4'
    return 'unknown'


# TEST-ONLY pre-release rehearsal knob, mirrored from the kiosk's
# platform::apply_policy_override (platform_profile.h). With
# MDB_PLATFORM_POLICY_OVERRIDE=pi4 on a Pi 5, the web admin applies the
# Pi 4B's SOFTWARE POLICIES (one concurrent transcode, ultrafast/CRF 28
# encoder tier). The kiosk and this service each read their OWN unit
# environment, so a rehearsal sets it in BOTH units' drop-ins.
# verify_box.sh FAILS while either unit carries it — never ship a box with it.
PLATFORM_POLICY_OVERRIDE_ENV = "MDB_PLATFORM_POLICY_OVERRIDE"


def _policy_pi_model(real_model: str,
                     env_value: Optional[str]) -> tuple[str, str]:
    """Resolve the board whose POLICIES the web admin applies.

    Returns (policy_model, log_line). Same contract as the kiosk: only the
    exact value "pi4" is accepted, and only on a real Pi 5; anything else
    leaves `real_model` in force. log_line is "" when the env var is unset,
    otherwise the one loud line to print at startup (active or ignored)."""
    if not env_value:
        return real_model, ""
    if env_value != "pi4":
        reason = f"unrecognized value {env_value!r} (only 'pi4' is accepted)"
    elif real_model == "pi4":
        reason = "board is already a Pi 4B, so the override is a no-op here"
    elif real_model != "pi5":
        reason = "board is not a Pi 5; the override applies only on a Pi 5"
    else:
        return "pi4", (
            "PLATFORM POLICY OVERRIDE ACTIVE: running Pi 4B policies on "
            f"Raspberry Pi 5 ({PLATFORM_POLICY_OVERRIDE_ENV}=pi4 is "
            "TEST-ONLY: never ship; verify_box.sh fails while set)")
    return real_model, (f"{PLATFORM_POLICY_OVERRIDE_ENV} IGNORED: {reason} "
                        "(remove it from the unit environment)")


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    IMPORT_PEAK_MULTIPLIER = ctx.IMPORT_PEAK_MULTIPLIER
    _storage_precondition = ctx._storage_precondition
    data_dir = ctx.data_dir
    media_dir = ctx.media_dir
    require_csrf = ctx.require_csrf
    upload_temp_dir = ctx.upload_temp_dir
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _detect_pi_model = ctx._detect_pi_model

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

        def _get_clean_title(filename: str) -> str:
            """Extract clean title from filename (remove Youtube ID suffix)."""
            # Match "Title [video_id].ext"
            # Allow flexible ID format (anything in brackets at end of name)
            match = re.search(r"^(.*?) \[([^\]]+)\]\.[a-zA-Z0-9]+$", filename)
            if match:
                return match.group(1).strip()
            return filename

        media_list = [{
            'filename': f.name,
            'title': _get_clean_title(f.name),
            'path': str(f.relative_to(data_dir.parent)),  # Relative to parent of data dir
            'size': f.stat().st_size,
            'modified': f.stat().st_mtime
        } for f in sorted(files)]

        return success_response(data=media_list)

    MEDIA_VIDEO_EXTENSIONS = ('.mp4', '.mkv', '.avi', '.mov', '.webm')

    @app.post("/admin/upload")
    @require_csrf
    def upload_media():  # type: ignore[no-redef]
        """Upload video file."""
        # Ahead of request.files for the reason given in _storage_precondition:
        # touching it spools the body to the card. TWO copies here, not one:
        # werkzeug's spool lives until the request ends, and
        # _staged_save_upload copies it into a staging file beside the target
        # before the rename (the rename itself duplicates nothing).
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")
        f = request.files["file"]

        # Sanitize filename to prevent path traversal, and accept only the
        # video containers the media library actually lists (list_media scans
        # exactly these). Anything else landing in data/media was invisible to
        # the operator — unlistable, undeletable from the UI — and served no
        # purpose; the web UI never uses this raw endpoint (it goes through
        # /admin/smart-upload), so this narrows nothing a real flow needs.
        try:
            safe_filename = _sanitize_filename(
                f.filename, allowed_extensions=list(MEDIA_VIDEO_EXTENSIONS))
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        out = media_dir / safe_filename
        # Ensure path stays within media_dir
        out_resolved = out.resolve()
        media_dir_resolved = media_dir.resolve()
        if not _is_within(out_resolved, media_dir_resolved):
            return error_response("VALIDATION_ERROR", "Invalid path")

        _staged_save_upload(f, out)
        return success_response(data={"path": str(out.relative_to(data_dir.parent))}, message="File uploaded")

    @app.delete("/admin/media/<path:filepath>")
    @require_csrf
    def delete_media(filepath):  # type: ignore[no-redef]
        """Delete a media file from either uploaded or dev directories."""
        # Try to find the file in either location
        # filepath is relative to /opt/magic_dingus_box (parent of data_dir)
        target = data_dir.parent / filepath

        # Security check: only allow deletion within media subdirectories
        target_resolved = target.resolve()
        allowed_dirs = [
            (data_dir / "media").resolve(),
            (data_dir.parent / "dev_data" / "media").resolve(),
        ]

        if not any(_is_within(target_resolved, d) for d in allowed_dirs):
            return error_response("VALIDATION_ERROR", "Invalid path")

        if target.exists() and target.is_file():
            target.unlink()
            return success_response(message="File deleted")
        return error_response("NOT_FOUND", "File not found", status=404)

    # ===== VIDEO TRANSCODING =====

    # Resolution presets for transcoding.
    #
    # These are MASTERS, not display formats: the kiosk scales whatever is
    # stored to whichever display mode is active, so the right strategy is
    # to keep the best master that storage allows and let playback derive
    # the rest. Storing a display-resolution file is lossy in one direction
    # only — you can always go down, never back up.
    #
    # Aspect matters as much as resolution. The main kiosk renders playlist
    # video into a 4:3 viewport (gst_renderer letterbox: vp_w = canvas_h*4/3
    # -> 960x720 at 720p, 1440x1080 at 1080p), which is the deliberate
    # CRT look. A 16:9 master gets letterboxed INSIDE that pillarbox and
    # ends up smaller on screen, so 4:3 masters are correct for playlist
    # content even on a widescreen TV.
    #
    # 'crt_hd' is the default: 4:3 at 720p height. It is 2.25x the detail of
    # the old 640x480 default, lands 1:1 in the 4:3 area at 720p output,
    # upscales 1.5x at 1080p, and downscales cleanly for a real CRT through
    # the HDMI->composite converter (which is a downscale either way — the
    # Pi 5 has no composite out, so 640x480 was never a pixel-exact path).
    TRANSCODE_RESOLUTIONS = {
        # 4:3 masters — playlist content, both display modes
        'crt':     {'width': 640,  'height': 480},   # legacy/smallest
        'crt_hd':  {'width': 960,  'height': 720},   # DEFAULT
        'crt_fhd': {'width': 1440, 'height': 1080},  # max detail, ~2.25x the files
        # 16:9 master — only for genuinely widescreen source material
        'modern':  {'width': 1280, 'height': 720},
    }

    # Default master. Changing this affects NEW uploads only; existing
    # files are untouched and cannot regain detail they never had.
    DEFAULT_TRANSCODE = 'crt_hd'

    # Framing policy for transcoded masters.
    #   'crop' — scale to FILL the 4:3 frame and trim the overflow (the
    #            original CRT look; a widescreen or vertical source loses
    #            its edges). Historical behavior, so it stays the default
    #            for API callers that don't send the field.
    #   'fit'  — scale the WHOLE frame inside the 4:3 canvas and pad the
    #            rest with black bars (nothing trimmed — vertical phone
    #            clips get side borders, widescreen gets top/bottom bars).
    # Both modes respect the no-upscale guard in run_transcode_job.
    TRANSCODE_FIT_MODES = ('crop', 'fit')
    DEFAULT_FIT_MODE = 'crop'

    # Store for tracking transcoding jobs (in-memory, cleared on restart)
    transcode_jobs: dict = {}

    # Output-name reservations for in-flight uploads. A transcode's output
    # only appears at its final os.replace — minutes after the request chose
    # the name — so "pick the first name that doesn't exist() yet" let two
    # uploads of the same file (two phones, a double-submit, or a second job
    # queued behind the encoder semaphore) both choose clip.mp4: the second
    # publish silently replaced the first video. A name is free only if it
    # is neither on disk NOR held by a job still running; check-and-reserve
    # happens under one lock. In-memory is enough: a restart kills every
    # in-flight encode, and nothing on disk is ever a placeholder, so the
    # kiosk and the library listing only ever see finished videos.
    _media_name_lock = threading.Lock()
    _reserved_media_names: set = set()

    def _reserve_media_output(preferred_name: str) -> Path:
        """Reserve media_dir/<preferred_name> or the first free <stem>_N.mp4."""
        base = Path(preferred_name).stem
        with _media_name_lock:
            name, counter = preferred_name, 1
            while name in _reserved_media_names or (media_dir / name).exists():
                name = f"{base}_{counter}.mp4"
                counter += 1
            _reserved_media_names.add(name)
        return media_dir / name

    def _release_media_output(path: Path) -> None:
        with _media_name_lock:
            _reserved_media_names.discard(path.name)


    # x264 encoder tier, resolved per-board at runtime.
    #
    # Pi 5: CRF 23 matches the Retro Ripper's visual-quality target
    # (its export pipeline encodes CRF 23), so a master transcoded on the
    # box looks the same as one exported from the desktop tool. veryfast
    # rather than the ripper's desktop 'medium' preset: CRF targets
    # constant quality, so a slower preset mostly buys smaller files, and
    # two semaphore-slots of 'medium' plus the kiosk's own software video
    # decode (~1 core at 1080p) could saturate the 4 cores and stutter
    # playback mid-upload. veryfast keeps that headroom.
    #
    # Pi 4 (and unknown boards, per the "performance envelope is the
    # Pi 4B's" rule): ultrafast/28 — one libx264 encode already saturates
    # a Pi 4B sharing 1.5 GB RAM with the kiosk. Env-overridable for
    # experiments without a release.
    #
    # _PI_MODEL is the POLICY board: the real one, unless the TEST-ONLY
    # MDB_PLATFORM_POLICY_OVERRIDE=pi4 makes a Pi 5 rehearse Pi 4B policy
    # (see _policy_pi_model). Nothing below branches on hardware facts.
    _REAL_PI_MODEL = _detect_pi_model()
    _PI_MODEL, _policy_log_line = _policy_pi_model(
        _REAL_PI_MODEL, os.getenv(PLATFORM_POLICY_OVERRIDE_ENV))
    if _policy_log_line:
        print(f"WARNING: {_policy_log_line}", file=sys.stderr)
    app.config["PLATFORM_POLICY_OVERRIDE"] = (
        _PI_MODEL if _PI_MODEL != _REAL_PI_MODEL else None)

    # Cap concurrent ffmpeg encodes, per board at runtime. One libx264 encode
    # already saturates a Pi 4B (4 cores / 1.5 GB shared with the kiosk and
    # optionally the Media Browser Docker stack), so a second concurrent
    # encode there only thrashes — Pi 4B and unknown boards get 1 (the
    # performance envelope is the Pi 4B's). The Pi 5 has the headroom for 2,
    # letting a second upload start while the first finishes. Excess jobs
    # block in run_transcode_job() and report 'queued' to the UI.
    # MAGIC_MAX_TRANSCODES overrides on any board.
    _max_transcodes = _max_transcodes_for(
        _PI_MODEL, os.getenv("MAGIC_MAX_TRANSCODES"))
    app.config["MAX_TRANSCODES"] = _max_transcodes
    _TRANSCODE_SEMAPHORE = threading.Semaphore(_max_transcodes)
    if _PI_MODEL == 'pi5':
        _tier_preset, _tier_crf = 'veryfast', '23'
    else:
        _tier_preset, _tier_crf = 'ultrafast', '28'
    _X264_PRESET = os.getenv('MAGIC_X264_PRESET', _tier_preset)
    _X264_CRF = os.getenv('MAGIC_X264_CRF', _tier_crf)

    # TTL for completed/errored jobs in any of the three job dicts. Without
    # eviction, a long-running kiosk that sees repeated upload/update/MB
    # operations accumulates entries indefinitely. Pruned opportunistically
    # at the start of each job-creating route — no background thread needed.
    _JOB_RETENTION_SECONDS = 3600  # 1 hour after terminal state

    def _prune_terminal_jobs(jobs_dict: dict) -> None:
        """Remove jobs in a terminal state older than _JOB_RETENTION_SECONDS.

        Uses the `_pruner_ts` field (numeric epoch, set at each job-creation
        site for prune-tracking purposes; intentionally separate from the
        existing user-facing `started_at` strings so we don't break their
        ISO-8601 contract). Jobs without `_pruner_ts` are kept forever — by
        design, since adding the field is opt-in at each call site."""
        cutoff = time.time() - _JOB_RETENTION_SECONDS
        terminal_states = {"complete", "completed", "success", "error", "failed", "cancelled", "canceled"}
        stale = []
        for jid, job in jobs_dict.items():
            if not isinstance(job, dict):
                continue
            status = str(job.get("status", "")).lower()
            if status not in terminal_states:
                continue
            ts = job.get("_pruner_ts")
            if isinstance(ts, (int, float)) and ts < cutoff:
                stale.append(jid)
        for jid in stale:
            jobs_dict.pop(jid, None)

    def _probe_source_dimensions(file_path: Path):
        """Return (width, height) of the first video stream, or None.

        Best-effort: any probe failure returns None and the caller falls
        back to the preset dimensions unchanged (pre-guard behavior)."""
        try:
            result = subprocess.run([
                'ffprobe', '-v', 'error', '-select_streams', 'v:0',
                '-show_entries', 'stream=width,height', '-of', 'csv=p=0',
                str(file_path)
            ], capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                return None
            parts = result.stdout.strip().split(',')
            w, h = int(parts[0]), int(parts[1])
            return (w, h) if w > 0 and h > 0 else None
        except Exception:
            return None

    def run_transcode_job(job_id: str, input_path: Path, output_path: Path, resolution: str, normalize_audio: bool = False, fit_mode: str = DEFAULT_FIT_MODE):
        """Background thread function to run FFmpeg transcoding."""
        job = transcode_jobs[job_id]
        res = TRANSCODE_RESOLUTIONS.get(resolution, TRANSCODE_RESOLUTIONS[DEFAULT_TRANSCODE])
        width, height = res['width'], res['height']

        # Never upscale. A preset is a MAXIMUM master size, not a canvas to
        # inflate into: encoding a 640x480 source at 960x720 adds zero detail,
        # doubles the file, and bakes in a lossy re-encode of an upscale.
        # Dimensions floor to even for libx264.
        src_dims = _probe_source_dimensions(input_path)

        if fit_mode == 'fit':
            # Whole frame INSIDE the 4:3 canvas, black bars fill the rest.
            # The canvas stays at the chosen preset size (a consistent 4:3
            # master); the CONTENT is scaled by min(fit, 1.0) — never
            # enlarged. A source smaller than the canvas sits centered at
            # its native size.
            if src_dims:
                src_w, src_h = src_dims
                content_scale = min(width / src_w, height / src_h, 1.0)
                content_w = max(2, int(src_w * content_scale) & ~1)
                content_h = max(2, int(src_h * content_scale) & ~1)
                vf = (f'scale={content_w}:{content_h},'
                      f'pad={width}:{height}:(ow-iw)/2:(oh-ih)/2')
            else:
                # Probe failed — let ffmpeg compute the fit. May upscale a
                # tiny source up to the canvas; acceptable fallback for a
                # file we couldn't inspect.
                vf = (f'scale={width}:{height}:force_original_aspect_ratio=decrease,'
                      f'pad={width}:{height}:(ow-iw)/2:(oh-ih)/2')
        else:
            # 'crop': scale to FILL the 4:3 frame, center-crop the overflow
            # (no bars, no distortion). Never upscale: shrink the target box
            # to fit inside the source while preserving the preset's aspect
            # policy, so the crop semantics still apply to sources whose
            # aspect differs.
            if src_dims:
                src_w, src_h = src_dims
                if src_w < width or src_h < height:
                    shrink = min(src_w / width, src_h / height)
                    width = max(2, int(width * shrink) & ~1)
                    height = max(2, int(height * shrink) & ~1)
            vf = (f'scale={width}:{height}:force_original_aspect_ratio=increase,'
                  f'crop={width}:{height}')

        # Bound concurrent encodes. Each transcode is a full libx264 encode;
        # on a Pi 4 sharing RAM/CPU with the kiosk (and possibly the Media
        # Browser Docker stack), several phones uploading at once would spawn
        # unbounded parallel ffmpeg processes and thrash/OOM. Block here until
        # a slot frees — the job sits in 'queued' rather than competing.
        job['status'] = 'queued'
        job['message'] = 'Queued — waiting for an encoder slot...'
        _TRANSCODE_SEMAPHORE.acquire()
        stderr_file = None

        # Encode into a staging name BESIDE the final path, and os.replace()
        # onto it only when ffmpeg exits 0. Encoding straight into
        # media_dir/<name>.mp4 meant the growing, moov-less (+faststart
        # writes moov at finalize) file was listed by /admin/media and
        # addable to a playlist for the whole multi-minute encode — and a
        # service restart or power cut mid-encode stranded it there forever,
        # indistinguishable from a real video. The .part suffix keeps it out
        # of every *.mp4 glob; same-directory staging guarantees os.replace
        # is an atomic same-filesystem rename wherever media_dir lives.
        # Crashed leftovers are swept at startup alongside upload_temp.
        # The job id in the staging name keeps two encoders from ever sharing
        # one .part (one's error path used to unlink the other's encode);
        # the name still ends in .part, so the sweep and globs are unchanged.
        staging_path = output_path.with_name(
            f"{output_path.name}.{job_id.replace('-', '')[:12]}.part")

        # Build FFmpeg command with the framing filter resolved above
        # (crop = fill the frame, fit = whole frame + black bars).
        ffmpeg_cmd = [
            'ffmpeg', '-y',
            '-i', str(input_path),
            '-vf', vf,
        ]
        
        # Audio normalization via FFmpeg's loudnorm filter (EBU R128).
        # Matches the Retro Ripper companion tool's settings exactly so a
        # video uploaded directly via the Content Manager sounds the same
        # as one ripped externally and dropped in:
        #   I=-16  → integrated loudness target -16 LUFS (YouTube-like;
        #            warmer/louder than the -23 LUFS broadcast standard
        #            we used to default to here)
        #   TP=-1  → max true peak -1 dBTP (small headroom for resampler
        #            ringing; matches Retro Ripper)
        #   LRA=11 → loudness range 11 LU (allows reasonable dynamics
        #            instead of squashing to 7 LU)
        # Reference: retro_ripper/config/config.py:AUDIO_TARGET_LUFS,
        # AUDIO_TRUE_PEAK, AUDIO_LOUDNESS_RANGE.
        if normalize_audio:
            ffmpeg_cmd.extend(['-af', 'loudnorm=I=-16:TP=-1:LRA=11'])
        
        ffmpeg_cmd.extend([
            '-c:v', 'libx264',
            # Per-board tier (see _X264_PRESET/_X264_CRF above): Pi 5 gets
            # the Retro Ripper's CRF 23 quality target, Pi 4 keeps
            # ultrafast/28.
            '-preset', _X264_PRESET,
            '-crf', _X264_CRF,
            '-c:a', 'aac',
            # 192k matches the Retro Ripper's AAC bitrate (was 128k here —
            # the one audio setting that diverged between the pipelines).
            '-b:a', '192k',
            '-ar', '48000',
            '-movflags', '+faststart',
            '-progress', 'pipe:1',
            '-nostats',
            # Explicit muxer: ffmpeg normally infers it from the output
            # extension, and the .part staging name would defeat that.
            '-f', 'mp4',
            str(staging_path)
        ])

        try:
            job['status'] = 'transcoding'
            job['message'] = 'Starting FFmpeg...'

            # Get video duration for progress calculation
            probe_cmd = ['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                        '-of', 'default=noprint_wrappers=1:nokey=1', str(input_path)]
            try:
                duration_result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=30)
                duration = float(duration_result.stdout.strip())
            except Exception:
                duration = 0

            # Run FFmpeg. Progress goes to stdout (`-progress pipe:1`), which
            # we parse below; the ffmpeg banner + decoder warnings/errors go
            # to stderr. We redirect stderr to a TEMP FILE rather than a pipe:
            # with a stderr PIPE that we only read after stdout drains, a
            # chatty stderr (iPhone HEVC/.MOV decode warnings routinely exceed
            # the ~64KB pipe buffer) blocks ffmpeg's write(), stdout stops
            # advancing, our readline() blocks, and the job hangs forever with
            # ffmpeg orphaned. A file has no such buffer limit, so it can't
            # deadlock; we read it back only if ffmpeg fails.
            stderr_file = tempfile.NamedTemporaryFile(
                mode='w+', suffix='.ffmpeg-stderr', delete=False)
            process = subprocess.Popen(
                ffmpeg_cmd,
                stdout=subprocess.PIPE,
                stderr=stderr_file,
                text=True
            )

            # Watchdog: kill a hung ffmpeg so it can't hold its encoder slot
            # (the _TRANSCODE_SEMAPHORE below) forever. A stalled decode on a
            # corrupt/partial upload would otherwise block the readline() loop
            # indefinitely — two such stalls exhaust the default 2-slot pool
            # and every later upload sits 'queued' until a service restart.
            # SIGKILL after the cap unblocks readline() (stdout closes) → the
            # job falls through to the non-zero-returncode error path.
            transcode_timeout_s = int(os.getenv("MAGIC_TRANSCODE_TIMEOUT_S", "3600"))
            watchdog = threading.Timer(transcode_timeout_s, process.kill)
            watchdog.daemon = True
            watchdog.start()

            try:
                # Parse progress from stdout
                current_time = 0
                while True:
                    line = process.stdout.readline()
                    if not line and process.poll() is not None:
                        break

                    # Parse out_time_ms from progress output
                    if line.startswith('out_time_ms='):
                        try:
                            time_ms = int(line.split('=')[1].strip())
                            current_time = time_ms / 1000000  # Convert to seconds
                            if duration > 0:
                                job['progress'] = min(99, int((current_time / duration) * 100))
                                job['message'] = f'Transcoding: {job["progress"]}%'
                        except (ValueError, IndexError):
                            pass

                process.wait()  # ensure returncode is set
            finally:
                watchdog.cancel()

            # Check result
            if process.returncode == 0:
                # Atomic publish: the finished encode appears in the media
                # library all at once, or not at all.
                os.replace(staging_path, output_path)
                job['status'] = 'complete'
                job['progress'] = 100
                job['message'] = 'Transcoding complete!'
                job['output_path'] = str(output_path.relative_to(data_dir.parent))

                # Clean up input temp file
                try:
                    input_path.unlink()
                except Exception:
                    pass
            else:
                # Read ffmpeg's error output back from the stderr temp file.
                stderr_output = ''
                try:
                    stderr_file.seek(0)
                    stderr_output = stderr_file.read()
                except Exception:
                    pass
                job['status'] = 'error'
                job['message'] = f'FFmpeg failed: {stderr_output[-200:]}'

                # Clean up on error
                try:
                    input_path.unlink()
                except Exception:
                    pass
                try:
                    staging_path.unlink()
                except Exception:
                    pass

        except Exception as e:
            job['status'] = 'error'
            job['message'] = str(e)
            try:
                staging_path.unlink()
            except Exception:
                pass
        finally:
            # Always remove the stderr temp file.
            if stderr_file is not None:
                try:
                    stderr_file.close()
                    os.unlink(stderr_file.name)
                except Exception:
                    pass
            _TRANSCODE_SEMAPHORE.release()
            # After the publish (if any): from here the name is protected by
            # the file existing, or free again because the encode failed.
            _release_media_output(output_path)

    @app.post("/admin/upload-and-transcode")
    @require_csrf
    def upload_and_transcode():  # type: ignore[no-redef]
        """Upload video file and transcode it on the Pi."""
        # Two copies: werkzeug's spool + the saved original during the
        # request; then the original sits in upload_temp for the whole job
        # while ffmpeg writes the .part next to the media library.
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")

        f = request.files["file"]
        resolution = request.form.get("resolution", DEFAULT_TRANSCODE)
        fit_mode = request.form.get("fit_mode", DEFAULT_FIT_MODE)
        # `normalize_audio` arrives as the string "true"/"false" (FormData
        # POST). Coerce to bool. Default ON because phone-uploaded clips
        # almost always have inconsistent levels — we'd rather opt-out
        # than opt-in for the typical user flow.
        normalize_audio = (request.form.get("normalize_audio", "true").lower()
                           == "true")

        if resolution not in TRANSCODE_RESOLUTIONS:
            return error_response("VALIDATION_ERROR", f"Invalid resolution: {resolution}")
        if fit_mode not in TRANSCODE_FIT_MODES:
            return error_response("VALIDATION_ERROR", f"Invalid fit_mode: {fit_mode}")

        # Create unique job ID
        job_id = str(uuid.uuid4())

        # Sanitize filename
        try:
            original_name = _sanitize_filename(f.filename)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        # Save to temp location
        temp_input = upload_temp_dir / f"transcode_input_{job_id}_{original_name}"

        # Unique output filename — reserved against in-flight jobs too, not
        # just files already on disk (see _reserve_media_output).
        output_path = _reserve_media_output(Path(original_name).stem + ".mp4")
        output_name = output_path.name

        # Save uploaded file
        try:
            f.save(str(temp_input))
        except BaseException:
            _release_media_output(output_path)
            raise

        # Initialize job (prune stale terminal-state entries first)
        _prune_terminal_jobs(transcode_jobs)
        transcode_jobs[job_id] = {
            'status': 'pending',
            'progress': 0,
            'message': 'Upload complete, starting transcode...',
            'output_path': None,
            'output_filename': output_name,
            '_pruner_ts': time.time(),
        }

        # Start transcoding in background thread. Pass normalize_audio
        # through — without this, the upload UI's "Normalize audio
        # volume" checkbox was being silently ignored because the
        # request handler dropped the form field on the floor.
        thread = threading.Thread(
            target=run_transcode_job,
            args=(job_id, temp_input, output_path, resolution, normalize_audio,
                  fit_mode)
        )
        thread.daemon = True
        thread.start()

        return success_response(data={'job_id': job_id}, message="Transcoding started")

    @app.get("/admin/transcode-status/<job_id>")
    def transcode_status(job_id):  # type: ignore[no-redef]
        """Get status of a transcoding job.

        Jobs are in-memory on purpose: the ffmpeg encode runs inside this
        service and dies with it (and its .part is swept at startup), so a
        job cannot outlive a restart. The 404 is therefore final, and says
        so — manager.js turns it into "upload it again" rather than polling.
        """
        if job_id not in transcode_jobs:
            return error_response(
                "NOT_FOUND",
                "Job not found — the box may have restarted while converting. "
                "Please upload the file again.",
                status=404)

        job = transcode_jobs[job_id]
        return success_response(data={
            'status': job['status'],
            'progress': job['progress'],
            'message': job['message'],
            'output_path': job.get('output_path'),
            'output_filename': job.get('output_filename'),
        })

    def probe_video(file_path: Path, target_resolution: str) -> dict:
        """Probe video file to check if it needs transcoding."""
        target = TRANSCODE_RESOLUTIONS.get(target_resolution, TRANSCODE_RESOLUTIONS[DEFAULT_TRANSCODE])
        target_w, target_h = target['width'], target['height']

        try:
            # Run ffprobe to get video info
            probe_cmd = [
                'ffprobe', '-v', 'error',
                '-select_streams', 'v:0',
                '-show_entries', 'stream=width,height,codec_name',
                '-show_entries', 'format=format_name',
                '-of', 'json',
                str(file_path)
            ]
            result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=30)

            if result.returncode != 0:
                return {'needs_transcode': True, 'reason': 'Could not probe video'}

            import json as json_module
            data = json_module.loads(result.stdout)

            # Extract video stream info
            streams = data.get('streams', [])
            if not streams:
                return {'needs_transcode': True, 'reason': 'No video stream found'}

            stream = streams[0]
            width = stream.get('width', 0)
            height = stream.get('height', 0)
            codec = stream.get('codec_name', '')

            # Extract container format
            format_name = data.get('format', {}).get('format_name', '')
            is_mp4 = 'mp4' in format_name or 'mov' in format_name

            # Check if video is already compatible.
            #
            # "Compatible" is NOT an exact dimension match: presets are
            # MAXIMUM master sizes (see TRANSCODE_RESOLUTIONS), so a
            # smaller-or-equal source with the same aspect ratio has nothing
            # to gain from re-encoding — transcoding would only add
            # generation loss. This keeps legacy 640x480 masters re-uploaded
            # under the 960x720 default as byte-identical direct copies.
            # A source with a DIFFERENT aspect still transcodes so the
            # preset's center-crop policy applies (capped at source size by
            # run_transcode_job's no-upscale guard).
            fits_target = (0 < width <= target_w and 0 < height <= target_h)
            same_aspect = (height > 0 and
                           abs(width / height - target_w / target_h) < 0.02)
            is_correct_resolution = fits_target and same_aspect
            is_h264 = codec in ('h264', 'libx264')

            if is_correct_resolution and is_h264 and is_mp4:
                return {
                    'needs_transcode': False,
                    'width': width,
                    'height': height,
                    'codec': codec,
                    'reason': 'Already compatible'
                }
            else:
                reasons = []
                if not fits_target:
                    reasons.append(f'Resolution {width}x{height} exceeds {target_w}x{target_h}')
                elif not same_aspect:
                    reasons.append(f'Aspect of {width}x{height} differs from {target_w}x{target_h}')
                if not is_h264:
                    reasons.append(f'Codec {codec} is not H.264')
                if not is_mp4:
                    reasons.append('Not MP4 container')

                return {
                    'needs_transcode': True,
                    'width': width,
                    'height': height,
                    'codec': codec,
                    'reason': '; '.join(reasons)
                }

        except Exception as e:
            return {'needs_transcode': True, 'reason': f'Probe error: {str(e)}'}

    @app.post("/admin/smart-upload")
    @require_csrf
    def smart_upload():  # type: ignore[no-redef]
        """Smart upload: probe video and decide whether to transcode or direct upload."""
        # Two copies on every branch: werkzeug's spool + the probe copy during
        # the request, then the original + ffmpeg's .part if it transcodes
        # (the direct branch renames the probe copy, adding nothing).
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")

        f = request.files["file"]
        resolution = request.form.get("resolution", DEFAULT_TRANSCODE)
        fit_mode = request.form.get("fit_mode", DEFAULT_FIT_MODE)
        # See upload_and_transcode for normalize_audio default rationale.
        # Default ON here too so a phone upload of an already-720p clip
        # still gets its audio levels fixed even when the video itself
        # doesn't need re-encoding.
        normalize_audio = (request.form.get("normalize_audio", "true").lower()
                           == "true")

        if resolution not in TRANSCODE_RESOLUTIONS:
            return error_response("VALIDATION_ERROR", f"Invalid resolution: {resolution}")
        if fit_mode not in TRANSCODE_FIT_MODES:
            return error_response("VALIDATION_ERROR", f"Invalid fit_mode: {fit_mode}")

        # Sanitize filename
        try:
            original_name = _sanitize_filename(f.filename)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        # Save to temp location for probing
        job_id = str(uuid.uuid4())
        temp_input = upload_temp_dir / f"probe_{job_id}_{original_name}"
        f.save(str(temp_input))

        # Probe the video
        probe_result = probe_video(temp_input, resolution)

        # If the video is already at target resolution+codec but the
        # user wants audio normalized, we can't take the "direct copy"
        # shortcut — flip needs_transcode on so the loudnorm pass
        # runs. The transcode is fast in this case (still has to
        # re-encode video but the loudnorm filter can be heavy on
        # long clips, so this is the right tradeoff).
        if normalize_audio and not probe_result['needs_transcode']:
            probe_result['needs_transcode'] = True
            probe_result['reason'] = (
                'Audio normalization requested — re-encoding to apply '
                'loudnorm filter even though video already matches target.'
            )

        if not probe_result['needs_transcode']:
            # Already compatible - move directly to media folder
            output_name = Path(original_name).stem + ".mp4"
            # If original is already .mp4, keep it
            if original_name.lower().endswith('.mp4'):
                output_name = original_name
            # Unique filename — reserved against in-flight transcodes too
            # (see _reserve_media_output); released once the move has
            # published it, after which the file itself holds the name.
            output_path = _reserve_media_output(output_name)
            output_name = output_path.name

            # Move file to media folder
            try:
                shutil.move(str(temp_input), str(output_path))
            finally:
                _release_media_output(output_path)

            return success_response(data={
                'action': 'direct',
                'needs_transcode': False,
                'output_path': str(output_path.relative_to(data_dir.parent)),
                'output_filename': output_name,
                'probe': probe_result
            }, message="File uploaded directly (already compatible)")

        else:
            # Needs transcoding - start transcode job. Unique output name,
            # reserved until the job finishes (see _reserve_media_output).
            output_path = _reserve_media_output(Path(original_name).stem + ".mp4")
            output_name = output_path.name

            # Initialize job (prune stale terminal-state entries first)
            _prune_terminal_jobs(transcode_jobs)
            transcode_jobs[job_id] = {
                'status': 'pending',
                'progress': 0,
                'message': 'Starting transcode...',
                'output_path': None,
                'output_filename': output_name,
                '_pruner_ts': time.time(),
            }

            # Start transcoding in background thread. normalize_audio
            # propagates from the form here as well — without this the
            # loudnorm filter never fires regardless of UI checkbox.
            thread = threading.Thread(
                target=run_transcode_job,
                args=(job_id, temp_input, output_path, resolution,
                      normalize_audio, fit_mode)
            )
            thread.daemon = True
            thread.start()

            return success_response(data={
                'action': 'transcode',
                'needs_transcode': True,
                'job_id': job_id,
                'probe': probe_result
            }, message="Transcoding started")

    # Published for the route modules registered after this one
    ctx._prune_terminal_jobs = _prune_terminal_jobs
