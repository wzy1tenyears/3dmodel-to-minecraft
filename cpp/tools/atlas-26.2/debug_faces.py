import argparse
import json
from pathlib import Path


def parse_args():
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description='Find unsupported block model face values.')
    parser.add_argument(
        '--assets-root',
        type=Path,
        default=script_dir / 'assets',
        help='Root containing minecraft/models/block (default: tools/atlas-26.2/assets)',
    )
    parser.add_argument(
        '--ignore-list',
        type=Path,
        help='Optional file containing model filenames to skip, one per line',
    )
    return parser.parse_args()


args = parse_args()
models_dir = args.assets_root / 'minecraft/models/block'
if not models_dir.is_dir():
    raise SystemExit(f'Missing local Minecraft block models under: {args.assets_root}')
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
for model_path in sorted(models_dir.glob('*.json')):
    if model_path.name in ignore:
        continue
    model = json.loads(model_path.read_text(encoding='utf-8'))
    fn = SUPPORTED.get(model.get('parent'))
    if fn is None:
        continue
    faces = fn(model['textures'])
    try:
        sorted(set(faces.values()))
    except TypeError:
        print(model_path.name)
        print(faces)
        break
