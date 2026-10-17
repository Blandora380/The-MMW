"""Generate all iPhone/iPad icon sizes on the macOS build host using sips."""
import json
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parent
catalog = root / 'Assets.xcassets'
icons = catalog / 'AppIcon.appiconset'
icons.mkdir(parents=True, exist_ok=True)
(catalog / 'Contents.json').write_text(json.dumps({'info': {'author': 'xcode', 'version': 1}}))
images = []
for idiom, sizes, scales in [
    ('iphone', [20, 29, 40, 60], [2, 3]),
    ('ipad', [20, 29, 40, 76], [1, 2]),
    ('ipad', [83.5], [2]),
    ('ios-marketing', [1024], [1]),
]:
    for size in sizes:
        for scale in scales:
            pixels = round(size * scale)
            filename = f'icon-{pixels}.png'
            target = icons / filename
            if not target.exists():
                subprocess.run(['sips', '-z', str(pixels), str(pixels),
                    str(root / 'IconSource.png'), '--out', str(target)], check=True)
            images.append({'idiom': idiom, 'size': f'{size}x{size}',
                'scale': f'{scale}x', 'filename': filename})
(icons / 'Contents.json').write_text(json.dumps({'images': images,
    'info': {'author': 'xcode', 'version': 1}}, indent=2))
