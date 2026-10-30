# Flummi recursive workloads

These workloads are adapted from the examples in
[Flummi](https://github.com/DBatUTuebingen/flummi) at commit
`5d979a526f65fcaf838003b5b7b5c189d35325f4`.

Flummi compiles imperative programs to SQL by representing control flow as a
recursive CTE. The generated query carries the program counter, control-flow
label, emitted values, and live program variables through the recursive state.
This exercises a different recursive plan shape from the algorithm-specific
`USING KEY` workloads elsewhere in this directory.

The adapted generated SQL is checked in under `queries/smoke` and
`queries/performance`, so running the DuckDB tests does not require the Flummi
compiler. The pinned upstream revision retains the original inputs. The
adaptations replace random inputs with deterministic data, reduce oversized
examples to CI-suitable scales, and return compact results suitable for answer
validation.

The ray-tracing benchmark retains shadows, reflection depth, and the original
spheres scene, but renders at 384 by 240 pixels instead of 3480 by 2160.
The Release benchmark validates the image length and checksum. The generated
ray query is intentionally not part of the RelDebug SQLLogicTest: binding and
optimizing its 5,800-line plan takes roughly two minutes even when the rendered
image is reduced further. The other selected programs have scaled correctness
coverage in `test/sql/cte/recursive_cte_flummi.test_slow`.

## Trampoline formulations

Selected workloads have a `_trampoline` twin produced by
`translate_trampoline.py`, a mechanical label-per-branch translation of the
compiled recursive CTE into `WITH ... USING TRAMPOLINE`. A compiled `SYNC`
barrier becomes a `frontier` read with `MERGE` branches. Each twin validates
against the same answer as its recursive formulation.

The full ray program translates to roughly 17,000 lines of generated SQL, too
large to commit, so the trampoline twin for ray is `flummi_ray_reduced`: a
reduced ray program (`ray_reduced.fl`, analytic sphere intersection, one bounce
loop with reflection and shadow rays, a `SYNC` barrier before image assembly,
96 by 64 pixels) over the unchanged spheres scene. The reduced pair preserves
the structure the trampoline execution exercises: several per-label `MERGE`
branches, barrier rounds, a frontier read, and a wide carried payload. The
trampoline file duplicates the member per branch by construction, which puts
its size floor near 1,500 lines at this payload width.

Regenerate with the pinned Flummi revision:

```bash
flummi -d <db with load/ray.sql> ray_reduced.fl flummi_ray_reduced.sql
# replace the trailing SELECT with the image_bytes/image_md5 validation form
python3 translate_trampoline.py flummi_ray_reduced.sql flummi_ray_reduced_trampoline.sql
```
