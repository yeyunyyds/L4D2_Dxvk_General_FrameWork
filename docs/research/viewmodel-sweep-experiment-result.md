# Viewmodel Sweep experiment: unsuccessful / withdrawn

The experiment on `experiment/viewmodel-sweep-synthetic-only-v1` did not meet expectations. Its swept A/B mask shader, uniform modifications, regenerated SPIR-V, and dedicated build workflow are **excluded from main**.

The v1.0.1 release-candidate branch state on `main` uses `archive/fpb-protection-baseline-2026-11` as the structural base and preserves the tuned FPB protection settings. Only the independently opt-in `generated_only` setting is carried forward with hot reload. It remains experimental and unvalidated. No tag or Release is created until in-game validation passes.

The abandoned Viewmodel Sweep remains in experiment branch history for reference.
