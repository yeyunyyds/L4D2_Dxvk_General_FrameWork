"""Arm the active RT performance capture after a short return-to-game delay."""
from pathlib import Path
import time


def main():
    root = Path(__file__).resolve().parents[1]
    active = [p for p in (root / 'results').glob('rt-performance-*')
              if p.is_dir() and (p / 'progress.log').is_file() and not (p / 'summary.json').exists()]
    if len(active) != 1:
        raise SystemExit(f'Expected one active performance session; found {len(active)}')
    marker = active[0] / 'capture.trigger'
    if marker.exists():
        raise SystemExit('Capture is already armed')
    print('Return to the game now. Capture will start in 5 seconds.', flush=True)
    time.sleep(5)
    marker.write_text('trigger\n', encoding='ascii')
    print('Capture triggered. Wait for run-performance.cmd to finish.', flush=True)


if __name__ == '__main__':
    main()
