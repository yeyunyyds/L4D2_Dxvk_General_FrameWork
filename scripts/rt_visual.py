"""Independent CPU checks of the known diagnostic triangle, using only stdlib.

These checks validate final-color images, never a debug view. They do not prove
L4D2 geometry conversion, game-thread safety, or performance.
"""
import math
import json
from pathlib import Path

WIDTH, HEIGHT = 640, 360
NORMAL_CASES = ('baseline', 'explicit_blend', 'fixed_exposure', 'high_emission')
LEGACY_CASES = NORMAL_CASES + ('albedo', 'emission', 'translated')
DEBUG_CASES = ('albedo', 'processed_albedo', 'emission', 'invalid_values')
DARK_CASES = ('light_off', 'light_off_after', 'invalid_values')
LIGHT_CASES = ('light_off', 'light_on', 'light_translated', 'light_off_after')
ALL_CASES = NORMAL_CASES + DEBUG_CASES + LIGHT_CASES + ('translated',)
SHARP_OPTIONS = ('rtx.bloom.enable', 'rtx.postfx.enable', 'rtx.postfx.enableMotionBlur',
                 'rtx.postfx.enableChromaticAberration', 'rtx.postfx.enableVignette')


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


def inspect_dark(path):
    try:
        data = read_ppm(path)
        maximum = max(data)
        return {'pass': maximum <= 24, 'maximum': maximum, 'scope': 'expected dark control, not a visible-image pass'}
    except (OSError, ValueError) as error:
        return {'pass': False, 'error': str(error)}


def inspect_inputs(directory):
    try:
        data = json.loads((directory / 'geometry-input.json').read_text())
        uv, normals = data['uvs'], data['normals']
        if len(uv) != 3 or any(len(v) != 2 for v in uv) or len(normals) != 3 or any(len(n) != 3 for n in normals):
            raise ValueError('Expected three UV pairs and three normals')
        if not all(math.isfinite(x) for v in uv + normals for x in v):
            raise ValueError('Nonfinite mesh input')
        a, b, c = uv
        determinant = (a[0]-c[0])*(b[1]-c[1]) - (a[1]-c[1])*(b[0]-c[0])
        valid = abs(determinant) >= 1e-6 and all(abs(sum(x*x for x in n)-1) <= 1e-5 for n in normals)
        return {'pass': valid, 'uv_determinant_recomputed': determinant, 'input': data}
    except (OSError, ValueError, KeyError, TypeError) as error:
        return {'pass': False, 'error': str(error)}


def inspect_materials(directory):
    try:
        data = json.loads((directory / 'material-input.json').read_text())
        materials = data['materials']
        if len(materials) != 3:
            raise ValueError('Expected three fixed-scene materials')
        expected_emission = (4, 64, 0)
        valid = all(m['variant'] == i and m['alpha_test_type'] == 7
                    and m['use_draw_call_alpha_state'] is False and m['opacity'] == 1
                    and m['blend_override'] is False and m['emissive_intensity'] == expected_emission[i]
                    for i, m in enumerate(materials))
        return {'pass': valid, 'input': data,
                'contract': 'pinned AlphaTestType 7=ALWAYS, 0=NEVER; fixed opaque material owns alpha state'}
    except (OSError, ValueError, KeyError, TypeError) as error:
        return {'pass': False, 'error': str(error)}


def inspect_render_config(directory):
    # These are actual SetConfigVariable requests/results, not a readback of
    # runtime internals. The independent pixel checks remain mandatory.
    try:
        records = {(case, option): [] for case in ALL_CASES for option in SHARP_OPTIONS}
        with (directory / 'events.jsonl').open() as events:
            for line in events:
                event = json.loads(line)
                if event.get('stage') == 'render_config':
                    key = (event.get('case'), event.get('option'))
                    if key in records:
                        records[key].append((event.get('value'), event.get('code')))
        bad = [case + ':' + option for (case, option), calls in records.items()
               if calls != [('False', 0)]]
        return {'pass': not bad, 'expected_case_option_pairs': len(records), 'invalid_or_missing': bad,
                'scope': 'successful per-case requests; effective output checked separately from pixels'}
    except (OSError, ValueError, TypeError) as error:
        return {'pass': False, 'error': str(error)}


def evaluate_matrix(directory):
    directory = Path(directory)
    evidence = {'schema': 2, 'pass': False, 'cases': {}, 'selected_profile': None,
                'scope': 'standalone static RT final color and translated geometry; game RT unrun',
                'debug_views_count_as_acceptance': False}
    try:
        matrix = json.loads((directory / 'matrix.json').read_text())
        if matrix.get('schema') not in (1, 2, 3, 4):
            raise ValueError('Unknown case matrix version')
    except (OSError, ValueError) as error:
        evidence['error'] = str(error)
        return evidence
    modern = matrix['schema'] >= 2
    names = ALL_CASES if modern else LEGACY_CASES
    for name in names:
        case = directory / name
        try:
            status = json.loads((case / 'status.json').read_text())
        except (OSError, ValueError) as error:
            status = {'error': str(error)}
        offset = 1.0 if name in ('translated', 'light_translated') else 0.0
        dark = name in DARK_CASES
        images = {part: (inspect_dark(case / (part + '-color.ppm')) if dark
                         else inspect(case / (part + '-color.ppm'), offset)) for part in ('early', 'final')}
        expected_result = 'static_expected_dark_readback' if dark else 'static_visible_triangle_readback'
        valid = (status.get('result') == expected_result
                 and status.get('cleanup') == 'complete' and status.get('pick_hit') is True
                 and status.get('stable_image_signal') is True and status.get('readback_successes', 0) >= 2
                 and all(item['pass'] for item in images.values()))
        role = 'debug' if name in DEBUG_CASES else ('dark_control' if dark else 'final_color')
        evidence['cases'][name] = {'pass': valid, 'role': role, 'status': status, 'images': images}
    try:
        selected = matrix['selected_profile']
        if selected not in NORMAL_CASES or matrix.get('translated_offset_x') != 1.0:
            raise ValueError('Invalid normal profile or translated control')
        evidence['selected_profile'] = selected
        evidence['movement'] = moved(directory / selected / 'final-color.ppm',
                                     directory / 'translated/final-color.ppm')
        complete = all(item['status'].get('cleanup') == 'complete' for item in evidence['cases'].values())
        evidence['matrix_complete'] = complete
        emission_visible = (complete and evidence['cases'][selected]['pass']
                            and evidence['cases']['translated']['pass'] and evidence['movement']['pass'])
        evidence['pass'] = emission_visible
        if modern:
            evidence['mesh_inputs'] = inspect_inputs(directory)
            material_valid = True
            if matrix['schema'] >= 3:
                evidence['material_inputs'] = inspect_materials(directory)
                material_valid = evidence['material_inputs']['pass']
            evidence['emission_path'] = {'pass': (emission_visible and evidence['cases']['emission']['pass']
                                                   and evidence['mesh_inputs']['pass'] and material_valid)}
            lighting_move = moved(directory / 'light_on/final-color.ppm', directory / 'light_translated/final-color.ppm')
            evidence['lighting_path'] = {'pass': (complete and evidence['mesh_inputs']['pass'] and material_valid
                                                  and all(evidence['cases'][n]['pass'] for n in LIGHT_CASES)
                                                  and lighting_move['pass']),
                                         'movement': lighting_move,
                                         'scope': 'separate nonemissive material, light off/on/moved/off; cannot replace emission acceptance'}
            evidence['pass'] = emission_visible and evidence['emission_path']['pass'] and evidence['mesh_inputs']['pass'] and material_valid
            if matrix['schema'] >= 3:
                evidence['pass'] = evidence['pass'] and evidence['lighting_path']['pass']
            if matrix['schema'] >= 4:
                evidence['render_config'] = inspect_render_config(directory)
                configured = evidence['render_config']['pass']
                evidence['emission_path']['pass'] = evidence['emission_path']['pass'] and configured
                evidence['lighting_path']['pass'] = evidence['lighting_path']['pass'] and configured
                evidence['pass'] = evidence['pass'] and configured
    except (OSError, ValueError, KeyError) as error:
        evidence['error'] = str(error)
    leads = ('normal_output_visible', 'compare_default_and_explicit_blend',
             'compare_local_auto_and_global_fixed_exposure', 'compare_emission_scale')
    evidence['next_focus'] = next((lead for name, lead in zip(NORMAL_CASES, leads)
                                   if evidence['cases'][name]['pass']),
                                  'trace_emission_material_and_shader_values')
    if modern and not evidence.get('emission_path', {}).get('pass'):
        if evidence['cases']['processed_albedo']['pass']:
            evidence['next_focus'] = 'processed_color_visible_trace_emission_enable_intensity_and_color'
        elif evidence['cases']['albedo']['pass']:
            evidence['next_focus'] = 'raw_color_visible_inspect_processed_material_and_invalid_value_view'
        if any(c['images']['final'].get('interior_orange_fraction', 0) >= 0.65
               and c['images']['final'].get('exterior_orange_fraction', 0) > 0.05
               and c['images']['final'].get('red_contrast', 0) >= 20
               for n, c in evidence['cases'].items() if n in NORMAL_CASES):
            evidence['next_focus'] = 'final_color_visible_verify_silhouette_and_post_processing'
    return evidence
