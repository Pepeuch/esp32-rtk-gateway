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
    if not path.startswith('/'):
        return '/' + path
    return path


def endpoint_matches(endpoint: str, registered: list[str]) -> bool:
    for pattern in registered:
        if pattern == endpoint:
            return True
        if pattern.endswith('*') and endpoint.startswith(pattern[:-1]):
            return True
    return False


pages = {
    'dashboard': WWW / 'dashboard.html',
    'config': WWW / 'config.html',
    'advanced': WWW / 'advanced.html',
    'log': WWW / 'log.html',
}

for page_name, page in pages.items():
    text = page.read_text(errors='ignore')
    if 'id="top-nav"' not in text:
        errors.append(f'{page}: missing #top-nav container')
    if 'c/nav.js' not in text:
        errors.append(f'{page}: missing c/nav.js include')
    render_nav_single = f"renderNav('{page_name}')"
    render_nav_double = f'renderNav("{page_name}")'
    if render_nav_single not in text and render_nav_double not in text:
        errors.append(f'{page}: missing renderNav({page_name}) call')

for page in sorted(WWW.glob('*.html')):
    parser = AssetParser()
    parser.feed(page.read_text(errors='ignore'))
    for ref in parser.scripts + parser.styles:
        resolved = normalize_asset_path(ref, page)
        if resolved is None:
            continue
        if not resolved.exists():
            errors.append(f'{page}: missing asset reference {ref} -> {resolved}')

endpoint_patterns = [
    re.compile(r"\$\.getJSON\(\s*['\"]([^'\"]+)['\"]"),
    re.compile(r"\$\.post\(\s*['\"]([^'\"]+)['\"]"),
    re.compile(r"url:\s*['\"]([^'\"]+)['\"]"),
    re.compile(r"fetch\(\s*['\"]([^'\"]+)['\"]"),
]

registered = [
    match.group(1)
    for match in re.finditer(
        r'register_uri_handler(?:_optional)?\(server,\s*"([^"]+)"',
        WEB_SERVER.read_text(errors='ignore'),
    )
]

endpoint_sources = sorted(list(WWW.glob('*.html')) + list((WWW / 'c').glob('*.js')))
for source in endpoint_sources:
    text = source.read_text(errors='ignore')
    for pattern in endpoint_patterns:
        for raw_endpoint in pattern.findall(text):
            endpoint = normalize_endpoint(raw_endpoint)
            if endpoint.startswith(('http://', 'https://')):
                continue
            if not endpoint_matches(endpoint, registered):
                errors.append(f'{source}: unknown endpoint {raw_endpoint} -> {endpoint}')

legacy_patterns = [
    'incarvr6',
    'esp32-ntrip',
    'releases_api_url',
    'releases_html_url',
    'ConfigPage.initNav',
]

for source in sorted(list(WWW.glob('*.html')) + list((WWW / 'c').glob('*.js')) + list(WWW.glob('*.css'))):
    text = source.read_text(errors='ignore')
    for pattern in legacy_patterns:
        if pattern in text:
            errors.append(f'{source}: legacy reference found: {pattern}')

if errors:
    print('WebUI asset check failed:', file=sys.stderr)
    for error in errors:
        print(f' - {error}', file=sys.stderr)
    sys.exit(1)

print('WebUI asset check passed.')
PY
