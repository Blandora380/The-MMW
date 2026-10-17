"""Unpack the artwork shipped with the public cloud source snapshot."""
from pathlib import Path
import shutil
import zipfile

root = Path(__file__).resolve().parent.parent
parts = sorted((root / 'ios/assets').glob('editor-assets.zip.part*'))
combined = root / 'ios/editor-assets.zip'
if parts:
    with combined.open('wb') as output:
        for part in parts:
            with part.open('rb') as source:
                shutil.copyfileobj(source, output)
archives = ([combined] if combined.exists() else []) + sorted((root / 'ios/assets').glob('*.zip'))
for archive in archives:
    with zipfile.ZipFile(archive) as bundle:
        for item in bundle.infolist():
            (root / item.filename).resolve().relative_to(root)
        bundle.extractall(root)
print('Artwork and note sounds unpacked. The workflow downloads the Noto font separately.')
