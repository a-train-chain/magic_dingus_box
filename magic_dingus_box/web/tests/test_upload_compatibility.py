"""Uploads must produce masters the Pi 4 hardware decoder can play."""
import io
import json
import subprocess
import time
from fractions import Fraction

import pytest

import admin
import media_info
from admin import create_app
from ._deps import need_tool
from .test_transcode_staging import stub_bin, _run_job


@pytest.fixture
def real_ffmpeg():
    # FFmpeg and ffprobe are required for real upload conversion.
    need_tool('ffmpeg', 'ffprobe')


def _probe(path):
    return json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-show_streams', '-of', 'json', str(path)]))['streams'][0]


@pytest.mark.parametrize('pixel_format,rate,level,needs_transcode', [
    ('yuv420p10le', '24', '4.1', True),
    ('yuv420p10le', '24000/1001', '4.1', True),
    ('yuv420p10le', '30000/1001', '4.1', True),
    ('yuv422p', '25', '4.1', True),
    ('yuv444p', '30', '4.1', True),
    ('yuv420p', '60', '4.1', True),
    ('yuv420p', '30', '5.1', True),
    ('yuv420p', '24000/1001', '4.1', False),
    ('yuv420p', '30', '4.1', False),
])
def test_smart_upload_requires_hardware_compatible_stream(
        client, temp_data_dir, real_ffmpeg, pixel_format, rate, level, needs_transcode):
    source = temp_data_dir.parent / 'source.mp4'
    subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        f'testsrc2=size=640x480:rate={rate}', '-t', '0.5',
        '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', pixel_format,
        '-level:v', level, str(source)], check=True)
    response = client.post('/admin/smart-upload', data={
        'file': (io.BytesIO(source.read_bytes()), 'source.mp4'),
        'resolution': 'crt', 'normalize_audio': 'false',
    }, content_type='multipart/form-data')
    assert response.status_code == 200, response.get_json()
    data = response.get_json()['data']
    # Always wait for workers before the fixture deletes their directory.
    if data['needs_transcode']:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status = client.get('/admin/transcode-status/' + data['job_id']).get_json()['data']
            if status['status'] in ('complete', 'error'):
                break
            time.sleep(0.05)
        assert status['status'] == 'complete', status
        output = _probe(temp_data_dir / 'media' / status['output_filename'])
        assert output['pix_fmt'] in ('yuv420p', 'yuvj420p')
        assert output['profile'] in ('Constrained Baseline', 'Main', 'High')
        assert output['level'] <= 41
        assert Fraction(output['avg_frame_rate']) <= 30
        assert Fraction(output['avg_frame_rate']) == min(Fraction(rate), 30)
    assert data['needs_transcode'] is needs_transcode
    if not needs_transcode:
        assert (temp_data_dir / 'media' / 'source.mp4').read_bytes() == source.read_bytes()


@pytest.mark.parametrize('board,override,bounded', [
    ('pi4', None, True), ('unknown', None, True),
    ('pi5', None, False), ('pi5', 'pi4', True),
])
def test_ffmpeg_thread_budget_follows_runtime_policy(
        temp_data_dir, stub_bin, monkeypatch, board, override, bounded):
    monkeypatch.setattr(admin, '_detect_pi_model', lambda: board)
    monkeypatch.setenv('MAGIC_DISABLE_CSRF', '1')
    if override:
        monkeypatch.setenv('MDB_PLATFORM_POLICY_OVERRIDE', override)
    else:
        monkeypatch.delenv('MDB_PLATFORM_POLICY_OVERRIDE', raising=False)
    app = create_app(temp_data_dir, config={'TESTING': True})
    assert _run_job(app.test_client())['status'] == 'complete'
    argv = stub_bin.read_text().splitlines()
    input_index = argv.index('-i')
    if bounded:
        assert argv[argv.index('-threads') + 1] == '1'
        assert argv.index('-threads') < input_index
        assert argv[argv.index('-threads:v') + 1] == '1'
        assert argv.index('-threads:v') > input_index
        assert argv[argv.index('-filter_threads') + 1] == '1'
    else:
        assert '-threads' not in argv
        assert '-threads:v' not in argv
        assert '-filter_threads' not in argv


# A 16:9 source keeps its shape: the master covers the preset's 4:3 box
# (media_probe.master_size), and 'modern' is an alias of 'crt_hd'.
@pytest.mark.parametrize('resolution,size', [
    ('crt', (852, 480)), ('crt_hd', (1280, 720)),
    ('crt_fhd', (1920, 1080)), ('modern', (1280, 720)),
])
def test_each_preset_caps_large_high_rate_upload(
        client, temp_data_dir, real_ffmpeg, resolution, size):
    source = temp_data_dir.parent / 'large.mp4'
    subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc2=size=1920x1080:rate=60', '-t', '0.5',
        '-c:v', 'libx264', '-preset', 'ultrafast', str(source)], check=True)
    response = client.post('/admin/upload-and-transcode', data={
        'file': (io.BytesIO(source.read_bytes()), 'large.mp4'),
        'resolution': resolution, 'normalize_audio': 'false',
    }, content_type='multipart/form-data')
    assert response.status_code == 200, response.get_json()
    job_id = response.get_json()['data']['job_id']
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        status = client.get('/admin/transcode-status/' + job_id).get_json()['data']
        if status['status'] in ('complete', 'error'):
            break
        time.sleep(0.05)
    assert status['status'] == 'complete', status
    output = _probe(temp_data_dir / 'media' / status['output_filename'])
    assert (output['width'], output['height']) == size
    assert output.get('sample_aspect_ratio') in ('1:1', None)
    assert output['pix_fmt'] in ('yuv420p', 'yuvj420p')
    assert output['profile'] in ('Constrained Baseline', 'Main', 'High')
    assert output['level'] <= 41
    assert Fraction(output['avg_frame_rate']) == 30
    # The published master's real probe reached the kiosk's store.
    info = media_info.load(temp_data_dir)['media/' + status['output_filename']]
    assert info['display_aspect'] == pytest.approx(size[0] / size[1], abs=1e-3)
    assert info['duration_s'] == pytest.approx(0.5, abs=0.1)
    assert 'crop' not in info


def test_unprobeable_source_fallback_keeps_square_pixels(
        client, temp_data_dir, real_ffmpeg, monkeypatch):
    # With no probe the job bounds the clip with a bare ffmpeg filter. A tall
    # 1080x1920 clip used to come out 608x1080 with SAR 1215:1216.
    import media_probe
    monkeypatch.setattr(media_probe, 'probe_source', lambda path: None)
    source = temp_data_dir.parent / 'tall.mp4'
    subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc2=size=1080x1920:rate=30', '-t', '0.5',
        '-c:v', 'libx264', '-preset', 'ultrafast', str(source)], check=True)
    response = client.post('/admin/upload-and-transcode', data={
        'file': (io.BytesIO(source.read_bytes()), 'tall.mp4'),
        'normalize_audio': 'false',
    }, content_type='multipart/form-data')
    assert response.status_code == 200, response.get_json()
    job_id = response.get_json()['data']['job_id']
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        status = client.get('/admin/transcode-status/' + job_id).get_json()['data']
        if status['status'] in ('complete', 'error'):
            break
        time.sleep(0.05)
    assert status['status'] == 'complete', status
    output = _probe(temp_data_dir / 'media' / status['output_filename'])
    assert (output['width'], output['height']) == (608, 1080)
    assert output.get('sample_aspect_ratio') in ('1:1', None)


def test_sideways_phone_clip_is_stored_upright(client, temp_data_dir, real_ffmpeg):
    # A portrait phone clip: 1280x720 frames with a 90-degree display
    # rotation. The master must be upright (ffmpeg auto-rotates) and sized
    # by the upright 720x1280 shape: inside 1080 tall, so 608x1080.
    source = temp_data_dir.parent / 'portrait.mp4'
    plain = temp_data_dir.parent / 'landscape.mp4'
    subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc2=size=1280x720:rate=30', '-t', '0.5',
        '-c:v', 'libx264', '-preset', 'ultrafast', str(plain)], check=True)
    made = subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-display_rotation', '90',
        '-i', str(plain), '-c', 'copy', str(source)], capture_output=True)
    if made.returncode != 0:
        pytest.skip('this ffmpeg cannot write a display rotation')
    response = client.post('/admin/upload-and-transcode', data={
        'file': (io.BytesIO(source.read_bytes()), 'portrait.mp4'),
        'resolution': 'crt_hd', 'fit_mode': 'fit', 'normalize_audio': 'false',
    }, content_type='multipart/form-data')
    assert response.status_code == 200, response.get_json()
    job_id = response.get_json()['data']['job_id']
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        status = client.get('/admin/transcode-status/' + job_id).get_json()['data']
        if status['status'] in ('complete', 'error'):
            break
        time.sleep(0.05)
    assert status['status'] == 'complete', status
    output = _probe(temp_data_dir / 'media' / status['output_filename'])
    assert (output['width'], output['height']) == (608, 1080)
    assert not any('rotation' in sd for sd in output.get('side_data_list', []))
    info = media_info.load(temp_data_dir)['media/portrait.mp4']
    assert info['display_aspect'] == pytest.approx(608 / 1080, abs=1e-3)
    assert info['crop'] is False


def _keyframe_times(path):
    out = subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-skip_frame', 'nokey', '-show_entries', 'frame=pts_time',
        '-of', 'csv=p=0', str(path)], text=True)
    return [float(line.strip().rstrip(',')) for line in out.splitlines()
            if line.strip()]


def _wait_for_job(client, job_id, timeout_s=60):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        status = client.get('/admin/transcode-status/' + job_id).get_json()['data']
        if status['status'] in ('complete', 'error'):
            return status
        time.sleep(0.05)
    return status


# A Channel Guide tune seeks with KEY_UNIT|SNAP_NEAREST, which lands on the
# keyframe BEFORE the target: libx264's default 250-frame GOP (~8.3 s at
# 30 fps) let a channel start up to 8 s behind its schedule. Every master
# the box encodes carries a keyframe at least every 2 s, whatever its frame
# rate, so a join lands at most 2 s early.
@pytest.mark.parametrize('route,rate,pixel_format', [
    ('/admin/upload-and-transcode', '30', 'yuv420p'),
    ('/admin/smart-upload', '24000/1001', 'yuv422p'),
])
def test_every_transcode_has_a_keyframe_every_two_seconds(
        client, temp_data_dir, real_ffmpeg, route, rate, pixel_format):
    source = temp_data_dir.parent / 'long_gop.mp4'
    # A 12 s source with ONE keyframe: the output's keyframes must come from
    # the encode, not the source.
    subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        f'testsrc2=size=640x480:rate={rate}', '-t', '12',
        '-c:v', 'libx264', '-preset', 'ultrafast', '-g', '1000',
        '-pix_fmt', pixel_format, str(source)], check=True)
    response = client.post(route, data={
        'file': (io.BytesIO(source.read_bytes()), 'long_gop.mp4'),
        'resolution': 'crt', 'normalize_audio': 'false',
    }, content_type='multipart/form-data')
    assert response.status_code == 200, response.get_json()
    data = response.get_json()['data']
    assert 'job_id' in data, data
    status = _wait_for_job(client, data['job_id'])
    assert status['status'] == 'complete', status
    master = temp_data_dir / 'media' / status['output_filename']
    times = _keyframe_times(master)
    duration = float(json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-show_entries', 'format=duration',
        '-of', 'json', str(master)]))['format']['duration'])
    assert duration == pytest.approx(12, abs=0.2)
    assert times and times[0] == pytest.approx(0, abs=0.05)
    gaps = [b - a for a, b in zip(times, times[1:])] + [duration - times[-1]]
    assert max(gaps) <= 2.1, (times, duration)


@pytest.mark.parametrize('force_conversion', [False, True])
def test_full_range_420_upload_and_converted_master_are_reusable(
        client, temp_data_dir, real_ffmpeg, force_conversion):
    source = temp_data_dir.parent / 'full_range.mp4'
    subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc2=size=640x480:rate=30', '-t', '0.5',
        '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuvj420p',
        '-color_range', 'pc', '-level:v', '4.1', str(source)], check=True)
    assert _probe(source)['pix_fmt'] == 'yuvj420p'

    def upload(payload, name, normalize):
        response = client.post('/admin/smart-upload', data={
            'file': (io.BytesIO(payload), name), 'resolution': 'crt',
            'normalize_audio': 'true' if normalize else 'false',
        }, content_type='multipart/form-data')
        assert response.status_code == 200, response.get_json()
        data = response.get_json()['data']
        output_name = name
        if data['needs_transcode']:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                status = client.get('/admin/transcode-status/' + data['job_id']).get_json()['data']
                if status['status'] in ('complete', 'error'):
                    break
                time.sleep(0.05)
            assert status['status'] == 'complete', status
            output_name = status['output_filename']
        return data, temp_data_dir / 'media' / output_name

    data, master = upload(source.read_bytes(), 'first.mp4', force_conversion)
    assert data['needs_transcode'] is force_conversion
    stream = _probe(master)
    assert stream['pix_fmt'] == 'yuvj420p'
    assert stream['color_range'] == 'pc'
    if not force_conversion:
        assert master.read_bytes() == source.read_bytes()

    again, copied_master = upload(master.read_bytes(), 'again.mp4', False)
    assert again['needs_transcode'] is False
    assert copied_master.read_bytes() == master.read_bytes()
