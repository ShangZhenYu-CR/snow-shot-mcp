"""Windows deployment smoke test; pass a disposable extracted portable directory.

Starts the real application, briefly opens/cancels a screenshot selection, and
writes configuration/logs only in that portable directory. Does not save captures.
Modified distribution test, 2026-11-04; GPL-3.0-or-later.
"""

import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import winreg

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'snow_shot/tests'))
from mcp_fixture_tests import Client

stage = Path(sys.argv[1]).resolve()
exe = stage / 'bin/snow_shot.exe'
directory = Path(tempfile.mkdtemp(prefix='snow-portable-validation-'))
descriptor = directory / 'descriptor.json'
environment = dict(os.environ)
for key in tuple(environment):
    if key.startswith(('QT_', 'QML_', 'SNOW_SHOT_')) or key in ('QTDIR', 'Qt6_DIR'):
        del environment[key]
environment['PATH'] = os.environ['SystemRoot'] + '/System32;' + os.environ['SystemRoot']
environment['SNOW_SHOT_MCP_DESCRIPTOR'] = str(descriptor)
environment['QT_QPA_PLATFORM'] = 'windows'

def registry_state():
    results = {}
    def walk(path):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
                values = []
                i = 0
                while True:
                    try:
                        values.append(winreg.EnumValue(key, i)); i += 1
                    except OSError:
                        break
                results[path] = repr(sorted(values))
                i = 0
                while True:
                    try:
                        child = winreg.EnumKey(key, i); i += 1
                    except OSError:
                        break
                    walk(path + '\\' + child)
        except FileNotFoundError:
            results[path] = None
    walk(r'Software\Microsoft\Windows\CurrentVersion\Run')
    for kind in ('image', '.png', '.jpg', '.jpeg', '.webp', '.bmp', '.gif', '.tif', '.tiff', '.ico', '.svg'):
        walk('Software\\Classes\\SystemFileAssociations\\' + kind + '\\shell\\SnowShot.Edit')
    return hashlib.sha256(json.dumps(results, sort_keys=True).encode()).hexdigest()

before = registry_state()
for args in (['--distribution-probe'], ['--update-probe', '0.1.0-preview.1']):
    result = subprocess.run([str(exe), *args], env=environment, capture_output=True, timeout=45)
    assert result.returncode == 0, (args, result.returncode, result.stderr.decode(errors='replace'))
assert registry_state() == before, 'Probe changed startup/shell registration'
print('Clean-environment distribution/update probes passed; registration unchanged.', flush=True)

worker = stage / 'bin/assets/ocr/runtimes/1.2.1/windows-x64/snow-ocr-process-1.2.1-windows-x64.exe'
result = subprocess.run([str(worker), '--version'], env=environment, capture_output=True, timeout=30)
assert result.returncode == 0, (result.returncode, result.stderr)
print('OCR worker:', result.stdout.decode().strip(), flush=True)

client = None
with (directory / 'application.log').open('w', encoding='utf-8') as log:
    app = subprocess.Popen([str(exe), '--e2e-instance-id=portable-release-check', '--skip-startup-update'],
                           env=environment, stdout=log, stderr=log, cwd=directory)
    try:
        deadline = time.monotonic() + 45
        while not descriptor.exists() and app.poll() is None and time.monotonic() < deadline:
            time.sleep(.1)
        assert descriptor.exists(), f'Application failed: {app.poll()}, log {directory}'
        os.environ.clear(); os.environ.update(environment)
        client = Client(stage / 'bin/snow-shot-mcp.exe', descriptor)
        catalog = client.tool('snow_shot_app_actions', {'locale': 'zh_CN', 'include_icons': True})['result']
        actions = catalog['actions']
        assert len(actions) == 38, len(actions)
        for action in actions:
            assert type(action['requires_capture']) is bool
            assert action['category_name'] and action['icon']
            assert isinstance(action['prerequisites'], list)
            assert base64.b64decode(action['icon']['data_base64']).startswith(b'\x89PNG\r\n\x1a\n')
        begin = client.tool('snow_shot_screenshot_begin', {
            'selection_mode': 'interactive', 'control_mode': 'client',
            'idempotency_key': 'portable-release-check'})['result']
        session = begin['session_id']
        state = client.tool('snow_shot_screenshot_state', {'session_id': session})['result']
        assert state['active'], state
        client.tool('snow_shot_screenshot_cancel', {'session_id': session})
        client.tool('snow_shot_app_action', {'action': 'quit'})
        client.close(); client = None
        assert app.wait(timeout=20) == 0
        assert registry_state() == before, 'Portable startup changed registration'
        print('Clean-environment native startup, 38 action records, screenshot begin/state/cancel, quit passed.', flush=True)
        print('Startup/shell registry snapshot unchanged.', flush=True)
    finally:
        if client:
            client.close()
        if app.poll() is None:
            app.terminate(); app.wait(timeout=10)
print(json.dumps({'passed': True, 'private_test_log': str(directory)}))
