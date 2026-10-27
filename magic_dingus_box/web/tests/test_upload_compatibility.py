"""Uploads must produce masters the Pi 4 hardware decoder can play."""
import io
import json
import shutil
import subprocess
import time
from fractions import Fraction

import pytest

import admin
from admin import create_app
from .test_transcode_staging import stub_bin, _run_job


@pytest.fixture
def real_ffmpeg():
    if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
        pytest.skip('FFmpeg and ffprobe required for real upload conversion')


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


@pytest.mark.parametrize('resolution,size', [
    ('crt', (640, 480)), ('crt_hd', (960, 720)),
    ('crt_fhd', (1440, 1080)), ('modern', (1280, 720)),
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
    assert output['pix_fmt'] in ('yuv420p', 'yuvj420p')
    assert output['profile'] in ('Constrained Baseline', 'Main', 'High')
    assert output['level'] <= 41
    assert Fraction(output['avg_frame_rate']) == 30


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
