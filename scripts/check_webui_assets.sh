#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

python3 - <<'PY'
from html.parser import HTMLParser
from pathlib import Path
import re
import sys

ROOT = Path('.')
WWW = ROOT / 'www'
WEB_SERVER = ROOT / 'main' / 'web_server.c'

errors = []


class AssetParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts = []
        self.styles = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'script' and attrs.get('src'):
            self.scripts.append(attrs['src'])
        if tag == 'link' and attrs.get('rel') == 'stylesheet' and attrs.get('href'):
            self.styles.append(attrs['href'])


def normalize_asset_path(ref: str, page: Path) -> Path | None:
    if ref.startswith(('http://', 'https://', 'data:')):
        return None
    if ref.startswith('/'):
        return WWW / ref.lstrip('/')
    return page.parent / ref


def normalize_endpoint(path: str) -> str:
    if path.startswith(('http://', 'https://')):
        return path
    return path if path.startswith('/') else '/' + path


def endpoint_matches(endpoint: str, registered: list[str]) -> bool:
    for pattern in registered:
        if pattern == endpoint:
            return True
        if pattern.endswith('*') and endpoint.startswith(pattern[:-1]):
            return True
    return False


required_files = [
    WWW / 'index.html',
    WWW / 'dashboard.html',
    WWW / 'config.html',
    WWW / 'advanced.html',
    WWW / 'log.html',
    WWW / 'app-lite.css',
    WWW / 'app-lite.js',
    WWW / 'c' / 'api.js',
    WWW / 'c' / 'nav.js',
    WWW / 'c' / 'runtime.js',
    WWW / 'c' / 'dashboard.js',
    WWW / 'c' / 'config.js',
    WWW / 'c' / 'gnss.js',
    WWW / 'c' / 'ntrip.js',
    WWW / 'c' / 'lora.js',
    WWW / 'c' / 'log.js',
    WWW / 'c' / 'advanced.js',
]

for required in required_files:
    if not required.exists():
        errors.append(f'missing required WebUI file: {required}')

pages = {
    'dashboard': WWW / 'dashboard.html',
    'config': WWW / 'config.html',
    'advanced': WWW / 'advanced.html',
    'logs': WWW / 'log.html',
}

for page_name, page in pages.items():
    text = page.read_text(errors='ignore')
    if 'id="top-nav"' not in text:
        errors.append(f'{page}: missing #top-nav container')
    if 'class="page-header"' not in text:
        errors.append(f'{page}: missing shared page-header structure')
    render_single = f"WebUI.nav.render('{page_name}')"
    render_double = f'WebUI.nav.render("{page_name}")'
    if render_single not in text and render_double not in text:
        errors.append(f'{page}: missing WebUI.nav.render({page_name}) call')

page_count = 0
asset_count = 0
for page in sorted(WWW.glob('*.html')):
    page_count += 1
    parser = AssetParser()
    parser.feed(page.read_text(errors='ignore'))
    for ref in parser.scripts + parser.styles:
        asset_count += 1
        resolved = normalize_asset_path(ref, page)
        if resolved is None:
            continue
        if not resolved.exists():
            errors.append(f'{page}: missing asset reference {ref} -> {resolved}')

registered = [
    match.group(1)
    for match in re.finditer(
        r'register_uri_handler(?:_optional)?\(server,\s*"([^"]+)"',
        WEB_SERVER.read_text(errors='ignore'),
    )
]

endpoint_patterns = [
    re.compile(r'WebUI\.api\.(?:get|post)\(\s*["\']([^"\']+)["\']'),
    re.compile(r'(?<![A-Za-z0-9_])api\.(?:get|post)\(\s*["\']([^"\']+)["\']'),
    re.compile(r'fetch\(\s*["\']([^"\']+)["\']'),
]

relative_api_patterns = [
    re.compile(r'WebUI\.api\.(?:get|post)\(\s*["\']((?!/|https?:|data:)[^"\']+)["\']'),
    re.compile(r'(?<![A-Za-z0-9_])api\.(?:get|post)\(\s*["\']((?!/|https?:|data:)[^"\']+)["\']'),
    re.compile(r'fetch\(\s*["\']((?!/|https?:|data:)[^"\']+)["\']'),
]

endpoint_sources = sorted(
    list(WWW.glob('*.html')) +
    [WWW / 'app-lite.js'] +
    list((WWW / 'c').glob('*.js'))
)

endpoint_count = 0
for source in endpoint_sources:
    text = source.read_text(errors='ignore')
    for pattern in relative_api_patterns:
        for raw_endpoint in pattern.findall(text):
            errors.append(f'{source}: relative API path is forbidden: {raw_endpoint}')
    for pattern in endpoint_patterns:
        for raw_endpoint in pattern.findall(text):
            endpoint_count += 1
            endpoint = normalize_endpoint(raw_endpoint)
            if endpoint.startswith(('http://', 'https://')):
                continue
            if not endpoint_matches(endpoint, registered):
                errors.append(f'{source}: unknown endpoint {raw_endpoint} -> {endpoint}')

legacy_patterns = [
    'ConfigPage',
    'autoTab',
    'incarvr6',
    'esp32-ntrip',
    'legacy-ntrip-card',
    'legacy-config-card',
    'NTRIP server A',
    'NTRIP server B',
]

legacy_checks = 0
for source in sorted(list(WWW.glob('*.html')) + list(WWW.glob('*.js')) + list((WWW / 'c').glob('*.js')) + list(WWW.glob('*.css'))):
    text = source.read_text(errors='ignore')
    for pattern in legacy_patterns:
        legacy_checks += 1
        if pattern in text:
            errors.append(f'{source}: forbidden legacy reference found: {pattern}')

if errors:
    print('WebUI asset check failed:', file=sys.stderr)
    for error in errors:
        print(f' - {error}', file=sys.stderr)
    sys.exit(1)

print('WebUI asset check passed.')
print(f'Pages checked: {page_count}')
print(f'Assets checked: {asset_count}')
print(f'Endpoints checked: {endpoint_count}')
print(f'Legacy string checks: {legacy_checks}')
PY
