"""Render the project's geometric SVG into a Windows ICO. Requires Pillow only for regeneration."""
from pathlib import Path
import argparse
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def render(source: Path) -> Image.Image:
    root = ET.parse(source).getroot()
    assert root.attrib['viewBox'] == '0 0 256 256'
    scale = 4
    image = Image.new('RGBA', (256 * scale, 256 * scale))
    draw = ImageDraw.Draw(image)
    for node in root:
        kind = node.tag.rsplit('}', 1)[-1]
        if kind == 'rect':
            x, y, width, height, radius = [float(node.attrib[key]) * scale for key in ('x', 'y', 'width', 'height', 'rx')]
            draw.rounded_rectangle((x, y, x + width, y + height), radius,
                                   fill=node.attrib['fill'], outline=node.attrib.get('stroke'),
                                   width=int(float(node.attrib.get('stroke-width', 0)) * scale))
        elif kind == 'polygon':
            points = [tuple(float(v) * scale for v in point.split(',')) for point in node.attrib['points'].split()]
            draw.polygon(points, fill=node.attrib['fill'])
        else:
            raise ValueError(f'Unsupported SVG element: {kind}')
    return image.resize((256, 256), Image.Resampling.LANCZOS)


def save_icon(image: Image.Image, target) -> None:
    frames = []
    for size in SIZES:
        frame = image.resize((size, size), Image.Resampling.LANCZOS)
        # Resampling can leave faint alpha at the four transparent corners.
        for corner in ((0, 0), (size - 1, 0), (0, size - 1), (size - 1, size - 1)):
            frame.putpixel(corner, (0, 0, 0, 0))
        frames.append(frame)
    frames[-1].save(target, format='ICO', sizes=[(s, s) for s in SIZES], append_images=frames[:-1])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preview', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    icon = root / 'resources/app-icon.ico'
    image = render(root / 'resources/app-icon.svg')
    save_icon(image, icon)
    with Image.open(icon) as saved:
        assert saved.ico.sizes() == {(size, size) for size in SIZES}
        for size in SIZES:
            frame = saved.ico.getimage((size, size))
            assert frame.mode == 'RGBA' and frame.getextrema()[3][0] == 0 and frame.getextrema()[3][1] == 255
    if args.preview:
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        image.save(args.preview)
    print(f'Validated {len(SIZES)} icon sizes: {icon}')
