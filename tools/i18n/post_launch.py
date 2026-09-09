#!/usr/bin/env python3
"""Read-only post-launch CI boundary; never grants release authorization."""
from pathlib import Path
import re
import generate_release_notes as notes

ROOT = Path(__file__).resolve().parents[2]


def validate_version(version: str, manifest: dict) -> None:
    launch = manifest.get('localization_launch') or {}
    if (launch.get('status') != 'released' or launch.get('designated_by') != 'owner'
            or launch.get('localization_policy') != 'complete' or not launch.get('date')):
        raise ValueError('post-launch mode requires a completed owner-designated launch')
    def parts(value: str) -> tuple[int, ...]:
        if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', value):
            raise ValueError('expected a three-component release version')
        return tuple(map(int, value.split('.')))
    if parts(version) <= parts(launch['version']):
        raise ValueError('post-launch VERSION must be newer than the localization launch')
    releases = manifest.get('releases', [])
    if (not releases or releases[0].get('version') != version
            or sum(r.get('version') == version for r in releases) != 1):
        raise ValueError('VERSION must match the newest unique release-history entry')


def main() -> int:
    try:
        source = ROOT / 'assets/release-notes'
        manifest, locales, _ = notes.load_contract(source)
        validate_version((ROOT / 'VERSION').read_text(encoding='utf-8').strip(), manifest)
        launch = notes.launch_readiness(source, manifest, locales)
        if not launch.get('release_ready'):
            raise ValueError('preserved launch is invalid: ' + '; '.join(launch['release_blockers']))
    except (ValueError, OSError, notes.ReleaseNotesError) as error:
        print(f'post-launch localization error: {error}')
        return 1
    print('Post-launch version and preserved launch verified; publication authorization not assessed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
