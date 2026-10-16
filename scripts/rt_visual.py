"""Independent CPU checks of the known diagnostic triangle, using only stdlib.

These checks validate final-color images, never a debug view. They do not prove
L4D2 geometry conversion, game-thread safety, or performance.
"""
import math
import json
from pathlib import Path

WIDTH, HEIGHT = 640, 360
NORMAL_CASES = ('baseline', 'explicit_blend', 'fixed_exposure', 'high_emission')
ALL_CASES = NORMAL_CASES + ('albedo', 'emission', 'translated')


def read_ppm(path):
    # The owned writer has a fixed P6 header. Reject malformed/truncated/oversize
    # data rather than letting a parser reinterpret pixel bytes as whitespace.
    header = b'P6\n640 360\n255\n'
    with Path(path).open('rb') as source:
        data = source.read(len(header) + WIDTH * HEIGHT * 3 + 1)
    if not data.startswith(header) or len(data) != len(header) + WIDTH * HEIGHT * 3:
        raise ValueError('Expected exact 640x360 RGB8 P6 diagnostic image')
    return data[len(header):]


def warm(r, g, b):
    # Near-black dither (including the observed 1..5/255), isolated hot pixels,
    # and white/gray output cannot satisfy the expected orange material.
    return r >= 32 and r >= g + 12 and g >= b + 3


def region(x, y, offset):
    # Invert the documented +Z camera projection, then test three barycentric
    # coordinates in the z=3 plane. The margins exclude antialiased edges.
    scale = 2 * 3 * math.tan(math.radians(60) / 2) / HEIGHT
    wx, wy = (x + 0.5 - WIDTH / 2) * scale - offset, (HEIGHT / 2 - y - 0.5) * scale
    top = (wy + 1) / 3
    right = (1 - top) / 2 + wx / 4
    left = 1 - top - right
    low = min(top, left, right)
    return 1 if low >= 0.1 else (-1 if low <= -0.1 else 0)


def inspect(path, offset=0.0):
    try:
        data = read_ppm(path)
    except (OSError, ValueError) as error:
        return {'pass': False, 'error': str(error)}
    counts, colored, reds = [0, 0], [0, 0], [0, 0]
    for y in range(HEIGHT):
        for x in range(WIDTH):
            area = region(x, y, offset)
            if not area:
                continue
            slot = 0 if area == 1 else 1
            at = (y * WIDTH + x) * 3
            r, g, b = data[at:at + 3]
            counts[slot] += 1
            colored[slot] += warm(r, g, b)
            reds[slot] += r
    inside, outside = (colored[i] / counts[i] for i in range(2))
    contrast = reds[0] / counts[0] - reds[1] / counts[1]
    return {'pass': inside >= 0.65 and outside <= 0.05 and contrast >= 20,
            'interior_orange_fraction': inside, 'exterior_orange_fraction': outside,
            'red_contrast': contrast, 'interior_pixels': counts[0], 'exterior_pixels': counts[1],
            'minimum': min(data), 'maximum': max(data)}


def moved(original, translated):
    try:
        before, after = read_ppm(original), read_ppm(translated)
    except (OSError, ValueError) as error:
        return {'pass': False, 'error': str(error)}
    # Test only pixels well inside one position and well outside the other.
    # A static screenshot, noise, or a full-screen orange clear cannot pass.
    counts, changed = [0, 0], [0, 0]
    for y in range(HEIGHT):
        for x in range(WIDTH):
            a, b = region(x, y, 0.0), region(x, y, 1.0)
            if (a, b) not in ((1, -1), (-1, 1)):
                continue
            slot = 0 if a == 1 else 1
            at = (y * WIDTH + x) * 3
            was, now = warm(*before[at:at + 3]), warm(*after[at:at + 3])
            counts[slot] += 1
            changed[slot] += (was and not now) if slot == 0 else (now and not was)
    fractions = [changed[i] / counts[i] if counts[i] else 0 for i in range(2)]
    return {'pass': min(fractions) >= 0.60, 'cleared_fraction': fractions[0],
            'newly_covered_fraction': fractions[1], 'tested_pixels': counts}


def evaluate_matrix(directory):
    directory = Path(directory)
    evidence = {'schema': 1, 'pass': False, 'cases': {}, 'selected_profile': None,
                'scope': 'standalone static RT final color and translated geometry; game RT unrun',
                'debug_views_count_as_acceptance': False}
    for name in ALL_CASES:
        case = directory / name
        try:
            status = json.loads((case / 'status.json').read_text())
        except (OSError, ValueError) as error:
            status = {'error': str(error)}
        offset = 1.0 if name == 'translated' else 0.0
        images = {part: inspect(case / (part + '-color.ppm'), offset) for part in ('early', 'final')}
        valid = (status.get('result') == 'static_visible_triangle_readback'
                 and status.get('cleanup') == 'complete' and status.get('pick_hit') is True
                 and status.get('stable_image_signal') is True and status.get('readback_successes', 0) >= 2
                 and all(item['pass'] for item in images.values()))
        evidence['cases'][name] = {'pass': valid, 'role': 'debug' if name in ('albedo', 'emission') else 'final_color',
                                    'status': status, 'images': images}
    try:
        matrix = json.loads((directory / 'matrix.json').read_text())
        selected = matrix['selected_profile']
        if selected not in NORMAL_CASES or matrix.get('translated_offset_x') != 1.0:
            raise ValueError('Invalid normal profile or translated control')
        evidence['selected_profile'] = selected
        evidence['movement'] = moved(directory / selected / 'final-color.ppm',
                                     directory / 'translated/final-color.ppm')
        complete = all(item['status'].get('cleanup') == 'complete' for item in evidence['cases'].values())
        evidence['matrix_complete'] = complete
        evidence['pass'] = (complete and evidence['cases'][selected]['pass']
                            and evidence['cases']['translated']['pass'] and evidence['movement']['pass'])
    except (OSError, ValueError, KeyError) as error:
        evidence['error'] = str(error)
    # These are comparison-based leads, not claims of a proven root cause.
    leads = ('normal_output_visible', 'compare_default_and_explicit_blend',
             'compare_local_auto_and_global_fixed_exposure', 'compare_emission_scale')
    evidence['next_focus'] = next((lead for name, lead in zip(NORMAL_CASES, leads)
                                   if evidence['cases'][name]['pass']),
                                  'inspect_material_emission_and_composite_debug_images')
    return evidence
