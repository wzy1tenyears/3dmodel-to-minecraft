import argparse
import json
import math
from pathlib import Path
from PIL import Image


def parse_args():
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description='Generate the local Minecraft block atlas.')
    parser.add_argument(
        '--assets-root',
        type=Path,
        default=script_dir / 'assets',
        help='Root containing minecraft/models and minecraft/textures',
    )
    parser.add_argument(
        '--vendor-root',
        type=Path,
        default=script_dir.parents[1] / 'vendor',
        help='Destination vendor directory (default: cpp/vendor)',
    )
    parser.add_argument(
        '--ignore-list',
        type=Path,
        help='Optional file containing model filenames to skip, one per line',
    )
    return parser.parse_args()


args = parse_args()
vendor_root = args.vendor_root
models_dir = args.assets_root / 'minecraft/models/block'
textures_dir = args.assets_root / 'minecraft/textures/block'
out_atlas_dir = vendor_root / 'ots/res/atlases'

ignore = set()
if args.ignore_list:
    ignore = {
        line.strip()
        for line in args.ignore_list.read_text(encoding='utf-8').splitlines()
        if line.strip()
    }
SUPPORTED = {
    'minecraft:block/cube_column_horizontal': lambda t: {'up': t['side'], 'down': t['side'], 'north': t['end'], 'south': t['end'], 'east': t['side'], 'west': t['side']},
    'minecraft:block/cube_all': lambda t: {'up': t['all'], 'down': t['all'], 'north': t['all'], 'south': t['all'], 'east': t['all'], 'west': t['all']},
    'minecraft:block/cube_column': lambda t: {'up': t['end'], 'down': t['end'], 'north': t['side'], 'south': t['side'], 'east': t['side'], 'west': t['side']},
    'minecraft:block/cube_bottom_top': lambda t: {'up': t['top'], 'down': t['bottom'], 'north': t['side'], 'south': t['side'], 'east': t['side'], 'west': t['side']},
    'minecraft:block/cube': lambda t: {'up': t['up'], 'down': t['down'], 'north': t['north'], 'south': t['south'], 'east': t['east'], 'west': t['west']},
    'minecraft:block/template_single_face': lambda t: {'up': t['texture'], 'down': t['texture'], 'north': t['texture'], 'south': t['texture'], 'east': t['texture'], 'west': t['texture']},
    'minecraft:block/template_glazed_terracotta': lambda t: {'up': t['pattern'], 'down': t['pattern'], 'north': t['pattern'], 'south': t['pattern'], 'east': t['pattern'], 'west': t['pattern']},
    'minecraft:block/leaves': lambda t: {'up': t['all'], 'down': t['all'], 'north': t['all'], 'south': t['all'], 'east': t['all'], 'west': t['all']},
}

def normalize_texture_value(textures, value):
    if isinstance(value, str):
        if value.startswith('#'):
            return normalize_texture_value(textures, textures[value[1:]])
        return value
    if isinstance(value, dict):
        if 'sprite' in value:
            return normalize_texture_value(textures, value['sprite'])
        if 'texture' in value:
            return normalize_texture_value(textures, value['texture'])
    raise ValueError(f'Unsupported texture value: {value!r}')

def resolve_faces(model):
    fn = SUPPORTED.get(model.get('parent'))
    if fn is None:
        return None
    raw = fn(model['textures'])
    return {k: normalize_texture_value(model['textures'], v) for k, v in raw.items()}

def texture_file_for(key: str) -> Path:
    if ':block/' in key:
        short = key.split(':block/', 1)[1]
    elif key.startswith('block/'):
        short = key.split('block/', 1)[1]
    else:
        raise KeyError(key)
    return textures_dir / f'{short}.png'

def bitmap_stats(img: Image.Image):
    rgba = img.convert('RGBA')
    pixels = list(rgba.getdata())
    sum_r = sum_g = sum_b = sum_a = 0.0
    lums = []
    for r, g, b, a in pixels:
        if a <= 0:
            continue
        w = a / 255.0
        sum_r += r * w
        sum_g += g * w
        sum_b += b * w
        sum_a += w
        lums.append(((r / 255.0) + (g / 255.0) + (b / 255.0)) / 3.0)
    if sum_a <= 0:
        return {'colour': {'r': 0.0, 'g': 0.0, 'b': 0.0, 'a': 0.0}, 'std': 0.0}
    mean = sum(lums) / max(1, len(lums))
    var = sum((x - mean) ** 2 for x in lums) / max(1, len(lums))
    return {
        'colour': {'r': sum_r / sum_a / 255.0, 'g': sum_g / sum_a / 255.0, 'b': sum_b / sum_a / 255.0, 'a': 1.0},
        'std': math.sqrt(var),
    }

if not models_dir.is_dir() or not textures_dir.is_dir():
    raise SystemExit(f'Missing local Minecraft assets under: {args.assets_root}')

current_path = out_atlas_dir / 'vanilla.atlas'
current = None
if current_path.is_file():
    current = json.loads(current_path.read_text(encoding='utf-8'))

blocks = []
textures = {}
images = {}
for model_path in sorted(models_dir.glob('*.json')):
    if model_path.name in ignore:
        continue
    model = json.loads(model_path.read_text(encoding='utf-8'))
    faces = resolve_faces(model)
    if faces is None:
        continue
    unique_faces = sorted(set(faces.values()))
    for key in unique_faces:
        if key not in textures:
            tex_path = texture_file_for(key)
            if not tex_path.exists():
                continue
            img = Image.open(tex_path)
            images[key] = img.copy().convert('RGBA')
            img.close()
            textures[key] = {'atlasColumn': 0, 'atlasRow': 0, **bitmap_stats(images[key])}
    colour_faces = [textures[key]['colour'] for key in unique_faces if key in textures]
    if colour_faces:
        count = len(colour_faces)
        colour = {
            'r': sum(c['r'] for c in colour_faces) / count,
            'g': sum(c['g'] for c in colour_faces) / count,
            'b': sum(c['b'] for c in colour_faces) / count,
            'a': sum(c['a'] for c in colour_faces) / count,
        }
    else:
        colour = {'r': 0.0, 'g': 0.0, 'b': 0.0, 'a': 0.0}
    blocks.append({'name': f'minecraft:{model_path.stem}', 'faces': faces, 'colour': colour})

sorted_texture_keys = sorted(textures)
if not sorted_texture_keys:
    raise SystemExit(f'No supported block textures found under: {args.assets_root}')
atlas_size = math.ceil(math.sqrt(len(sorted_texture_keys)))
for idx, key in enumerate(sorted_texture_keys):
    textures[key]['atlasColumn'] = idx % atlas_size
    textures[key]['atlasRow'] = idx // atlas_size

atlas = {
    'formatVersion': 3,
    'atlasSize': atlas_size,
    'textures': {key: textures[key] for key in sorted_texture_keys},
    'blocks': blocks,
}

out_atlas_dir.mkdir(parents=True, exist_ok=True)
(out_atlas_dir / 'vanilla.atlas').write_text(json.dumps(atlas, indent=4, ensure_ascii=False), encoding='utf-8')
canvas = Image.new('RGBA', (atlas_size * 16, atlas_size * 16), (0, 0, 0, 0))
for key in sorted_texture_keys:
    img = images[key].resize((16, 16))
    slot = textures[key]
    canvas.paste(img, (slot['atlasColumn'] * 16, slot['atlasRow'] * 16))
canvas.save(out_atlas_dir / 'vanilla.png')
current = current or {'blocks': [], 'textures': {}}
current_names = {b['name'] for b in current['blocks']}
new_names = {b['name'] for b in atlas['blocks']}
missing = sorted(new_names - current_names)
print(json.dumps({
    'currentBlocks': len(current['blocks']),
    'newBlocks': len(atlas['blocks']),
    'currentTextures': len(current['textures']),
    'newTextures': len(atlas['textures']),
    'addedBlocks': len(missing),
    'missingSample': missing[:80],
}, ensure_ascii=False, indent=2))
