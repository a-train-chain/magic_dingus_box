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
from fractions import Fraction
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
    import media_info
    import media_probe
    from media_record import record_media_info
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import (
        _sanitize_filename, _staged_save_upload, error_response,
        success_response,
    )
    from .admin_security import _is_within
    from . import media_info, media_probe
    from .media_record import record_media_info


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
    _poke_kiosk_reload = ctx._poke_kiosk_reload
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _detect_pi_model = ctx._detect_pi_model

    def _record_media_info(published: Path, fit_mode: Optional[str]) -> None:
        """Record a just-published video in media_info.json (probe, starting
        Crop from the upload's Framing choice), then poke the kiosk once.
        Never raises; see media_record.record_media_info."""
        record_media_info(data_dir, published, fit_mode=fit_mode,
                          poke=lambda: _poke_kiosk_reload("media_info"))

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

        # One read of the shared store for the whole list. A video with no
        # entry (or outside the data dir, like dev_data) has an unknown
        # shape and the default setting: cropped.
        info = media_info.load(data_dir)

        def _entry(f: Path) -> dict:
            key = media_info.media_key(f, data_dir)
            rec = info.get(key, {}) if key else {}
            return {
                'filename': f.name,
                'title': _get_clean_title(f.name),
                'path': str(f.relative_to(data_dir.parent)),  # Relative to parent of data dir
                'size': f.stat().st_size,
                'modified': f.stat().st_mtime,
                'display_aspect': rec.get('display_aspect'),
                'crop': rec.get('crop') is not False,
            }

        media_list = [_entry(f) for f in sorted(files)]

        return success_response(data=media_list)

    @app.post("/admin/media/picture")
    @require_csrf
    def set_media_picture():  # type: ignore[no-redef]
        """Set one video's Crop on/off — the same store the kiosk's Picture
        control writes, so a change on either side shows on the other.
        Body: {"path": "data/media/x.mp4", "crop": false}."""
        body = request.get_json(silent=True)
        if not isinstance(body, dict):
            return error_response("VALIDATION_ERROR", "JSON body required")
        path, crop = body.get("path"), body.get("crop")
        if not isinstance(path, str) or not path or "\0" in path:
            return error_response("VALIDATION_ERROR", "path required")
        if not isinstance(crop, bool):
            return error_response("VALIDATION_ERROR", "crop must be true or false")

        # Only uploaded videos (data/media) have a store entry.
        target = (data_dir.parent / path).resolve()
        if not _is_within(target, media_dir.resolve()):
            return error_response("VALIDATION_ERROR", "Invalid path")
        key = media_info.media_key(target, data_dir.resolve())
        if key is None:
            return error_response("VALIDATION_ERROR", "Invalid path")
        if not target.is_file():
            return error_response("NOT_FOUND", "File not found", status=404)

        try:
            # Ticked = cropped = the default, so it is stored as absence;
            # only a fitted video is written (crop: false).
            media_info.update(data_dir, key, crop=None if crop else False)
        except OSError as e:
            print(f"[media] could not save crop for {key}: {e}", file=sys.stderr)
            return error_response(
                "STORAGE_ERROR",
                "Could not save the setting (the disk may be full or read-only)",
                status=500)
        _poke_kiosk_reload("media_info")
        return success_response(data={"crop": crop})

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
        _record_media_info(out, None)
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
            # Drop its store entry so a later video of the same name does
            # not inherit this one's setting. Best effort: the file is gone.
            try:
                key = media_info.media_key(target, data_dir)
                if key:
                    media_info.remove(data_dir, key)
                    _poke_kiosk_reload("media_info")
            except Exception as e:
                print(f"[media] could not remove media info for "
                      f"{target.name}: {e}", file=sys.stderr)
            return success_response(message="File deleted")
        return error_response("NOT_FOUND", "File not found", status=404)

    # ===== VIDEO TRANSCODING =====

    # Quality presets for transcoding — the 4:3 box a master must cover.
    #
    # These are MASTERS, not display formats: the kiosk scales whatever is
    # stored to whichever display mode is active, so the right strategy is
    # to keep the best master that storage allows and let playback derive
    # the rest. Storing a display-resolution file is lossy in one direction
    # only — you can always go down, never back up.
    #
    # A master keeps the SOURCE's aspect: the kiosk crops or fits each video
    # into its 4:3 area at playback, per video (media_info.json), so the
    # file itself is never cropped or padded. media_probe.master_size sizes
    # it to cover the preset's box (Crop-on playback is not upscaled, unless
    # the source is smaller than the box or the 1080-tall cap binds),
    # never up, never outside 1920x1080. 'crt_hd' (the default) stores a
    # 4:3 source at 960x720 — 1:1 in the 4:3 area at 720p output — and a
    # 16:9 one at 1280x720. 'modern' is an alias of 'crt_hd'.
    TRANSCODE_RESOLUTIONS = media_probe.TRANSCODE_RESOLUTIONS
    DEFAULT_TRANSCODE = media_probe.DEFAULT_TRANSCODE

    # The upload's Framing choice. It no longer changes the file: it is the
    # new video's starting Crop setting (_record_media_info), which the
    # owner can change later per video.
    #   'crop' — fill the 4:3 area, trimming the overflow (the default, and
    #            what a video with no stored setting does).
    #   'fit'  — show the whole frame with black borders (crop: false).
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

    def run_transcode_job(job_id: str, input_path: Path, output_path: Path, resolution: str, normalize_audio: bool = False, fit_mode: str = DEFAULT_FIT_MODE):
        """Background thread function to run FFmpeg transcoding."""
        job = transcode_jobs[job_id]

        # The master keeps the source's displayed shape (media_probe): no
        # crop, no pad, never upscaled. ffmpeg auto-rotates its input before
        # the filters, so a sideways phone clip is sized by its UPRIGHT
        # dimensions, which is what the probe reports. setsar=1: the target
        # size already carries the display aspect, so the pixels are square.
        src = media_probe.probe_source(input_path)
        if src is not None:
            w, h = media_probe.master_size(
                src['width'] * src['sar'], src['height'], resolution)
            vf = f'scale={w}:{h},setsar=1'
        else:
            # Probe failed — let ffmpeg bound it: inside 1920x1080, display
            # shape kept with square pixels (anamorphic sources included),
            # never taller. No preset cover-sizing for a file we couldn't
            # inspect.
            vf = media_probe.PROBE_FAILED_VF

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

        # Build FFmpeg command with the size filter resolved above.
        ffmpeg_cmd = ['ffmpeg', '-y']
        if _PI_MODEL != 'pi5':
            # A one-job semaphore still lets FFmpeg consume every core.
            # Bound decoding, filters and encoding independently so uploads
            # leave CPU/RAM for Pi 4 playback. Pi 5 retains auto threading.
            ffmpeg_cmd.extend(['-threads', '1', '-filter_threads', '1'])
        ffmpeg_cmd.extend(['-i', str(input_path), '-vf', vf])
        if _PI_MODEL != 'pi5':
            ffmpeg_cmd.extend(['-threads:v', '1'])
        
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
            # Shared masters must remain inside the Pi 4 H.264 envelope.
            # fpsmax preserves 24/25/29.97 cadence instead of duplicating
            # every source to 30 fps. VBV also constrains bitrate peaks.
            '-pix_fmt', 'yuv420p',
            '-profile:v', 'high', '-level:v', '4.1',
            '-maxrate', '20M', '-bufsize', '25M',
            '-fpsmax', '30',
            # A keyframe at least every 2 s, whatever the frame rate.
            # The kiosk's Channel Guide joins a channel with a KEY_UNIT
            # seek, which lands on the keyframe BEFORE the target:
            # libx264's default 250-frame GOP (~8.3 s at 30 fps) started a
            # channel up to 8 s behind its schedule. An expression on the
            # frame's time (not -g N frames) holds at 24, 25 and 29.97 fps
            # alike; scene-cut keyframes are kept (the tier's preset
            # decides those).
            '-force_key_frames', 'expr:gte(t,n_forced*2)',
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

            # Video duration for the progress calculation (0 = unknown).
            duration = (src or {}).get('duration_s') or 0

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
                # Before 'complete', so a client that sees the job finish
                # also sees the video's media info.
                _record_media_info(output_path, fit_mode)
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
        try:
            data = media_probe.run_ffprobe(file_path)
            if data is None:
                return {'needs_transcode': True, 'reason': 'Could not probe video'}

            geo = media_probe.geometry(data)
            if geo is None:
                return {'needs_transcode': True, 'reason': 'No video stream found'}

            stream = data['streams'][0]
            width, height = stream.get('width', 0), stream.get('height', 0)
            codec = stream.get('codec_name', '')

            # Extract container format
            format_name = (data.get('format') or {}).get('format_name', '')
            is_mp4 = 'mp4' in format_name or 'mov' in format_name

            # Check if video is already compatible.
            #
            # "Compatible" is the master this upload would be transcoded to
            # anyway (media_probe.master_size: the source's own shape,
            # covering the preset's box, never upscaled, inside 1920x1080),
            # stored upright with square pixels — re-encoding it would only
            # add generation loss. This keeps legacy 640x480 masters, and a
            # 1280x720 widescreen one, byte-identical direct copies under the
            # 960x720 default. A rotate tag or non-square pixels still
            # transcode: ffmpeg bakes the rotation and the pixel shape into
            # the master, so the kiosk never has to honour either.
            master = media_probe.master_size(
                geo['width'] * geo['sar'], geo['height'], target_resolution)
            is_master_size = master == (geo['width'], geo['height'])
            is_upright = geo['rotation'] == 0
            is_square = geo['sar'] == 1.0
            is_correct_resolution = is_master_size and is_upright and is_square
            is_h264 = codec in ('h264', 'libx264')

            # FFprobe names full-range 8-bit 4:2:0 yuvj420p; both are
            # hardware-compatible, including our own full-range encodes.
            is_8bit_420 = stream.get('pix_fmt') in ('yuv420p', 'yuvj420p')
            supported_profile = stream.get('profile') in (
                'Constrained Baseline', 'Baseline', 'Main', 'High')
            level = stream.get('level', 0)
            supported_level = isinstance(level, int) and 0 < level <= 41
            try:
                # Check both rates: a low average alone can hide a higher
                # nominal cadence. Missing/invalid metadata is re-encoded.
                supported_rate = all(
                    0 < Fraction(stream.get(key, '0/1')) <= 30
                    for key in ('avg_frame_rate', 'r_frame_rate'))
            except (ValueError, TypeError, ZeroDivisionError):
                supported_rate = False

            if (is_correct_resolution and is_h264 and is_mp4 and
                    is_8bit_420 and supported_profile and supported_level and supported_rate):
                return {
                    'needs_transcode': False,
                    'width': width,
                    'height': height,
                    'codec': codec,
                    'reason': 'Already compatible'
                }
            else:
                reasons = []
                if not is_upright:
                    reasons.append(f'Video is rotated {geo["rotation"]}°')
                if not is_square:
                    reasons.append('Video has non-square pixels')
                elif not is_master_size:
                    reasons.append(f'Resolution {geo["width"]}x{geo["height"]} '
                                   f'is larger than its {master[0]}x{master[1]} master')
                if not is_h264:
                    reasons.append(f'Codec {codec} is not H.264')
                if not is_mp4:
                    reasons.append('Not MP4 container')
                if not is_8bit_420:
                    reasons.append('Video must be 8-bit YUV 4:2:0')
                if not supported_profile or not supported_level:
                    reasons.append('H.264 profile/level exceeds hardware compatibility or is unknown')
                if not supported_rate:
                    reasons.append('Frame rate exceeds 30 fps or is unknown')

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
            _record_media_info(output_path, fit_mode)

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
