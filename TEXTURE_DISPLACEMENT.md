# Texture Displacement — Technical Notes

Branch: `feature/texture_displacement`. This document is a knowledge dump of the whole feature as
it stands: architecture, file map, algorithms, known bugs found and fixed (with root causes worth
remembering), and what's still deferred. Written so a fresh session (or a fresh pair of eyes) can
pick this up without re-deriving everything from scratch.

## What it does

A paint-style gizmo (`GLGizmoTextureDisplacement`) that lets you:
- Paint one or more "layers" onto a model's surface, each a height-map texture with its own
  depth/tiling/rotation/offset/invert/tile-mode/projection-mode/blend-mode.
- Pick a texture from a shipped library (`resources/textures/displacement/`) or import your own
  (saved into `<data_dir>/textures/displacement/`, kept separate so app updates can't clobber it).
- Combine overlapping layers with image-editor-style blend modes (Add/Subtract/Multiply/Divide).
- Preview the true displaced result live, before baking (background job, not on the UI thread).
- Optionally preview via a fast GPU bump-map shader instead (no real geometry movement, just
  shading) for a lighter-weight alternative.
- Bake into real mesh geometry on demand, restricted to the painted area only.
- Subdivide a low-poly model first so there are enough vertices to show fine detail.
- Unwrap a painted patch with a real CGAL LSCM parameterization and view it in a dedicated,
  dockable 2D "UV Editor" pane.

## Architecture

### Data model (per `ModelVolume`)

Each of up to `TEXTURE_DISPLACEMENT_MAX_LAYERS` (8) layers gets its **own independent
`FacetsAnnotation`** paint mask — the exact same `TriangleSelector`/`FacetsAnnotation` machinery
every other paint gizmo (FdmSupports, Seam, MMU, FuzzySkin) already uses, just one full instance
per layer slot instead of one per volume. This is what makes "layered/blended" painting work for
free: the same triangle can be `ENFORCER` in layer 2's mask and layer 5's mask simultaneously, and
at bake/preview time each layer displaces the surface left by the previous one (image-editor-layer
semantics).

**Important gotcha**: `ModelVolume` does **not** hold `std::array<FacetsAnnotation, 8>`. It holds
8 individually-named fields (`texture_displacement_facets_0` .. `_7`) plus a
`texture_displacement_facet(int slot)` accessor. Reason: `FacetsAnnotation`'s ctor is private,
friended only to `ModelVolume`; `std::array`'s own implicitly-generated special member functions
are generated with **`std::array`'s** access rights, not the enclosing class's, so friendship does
not propagate through the array wrapper. This is a real MSVC C2280 if you try it — confirmed by
attempting it. `TextureDisplacementFacetsData` (a `std::array<TriangleSelector::TriangleSplittingData, 8>`,
used to carry paint-mask *data* around, e.g. into the bake job) is fine as a real `std::array`
since `TriangleSplittingData` has an ordinary public ctor — only the `FacetsAnnotation` object
itself has the friend-ctor problem.

Plus `std::vector<TextureDisplacementLayer> texture_displacement_layers;` — the plain-data layer
definitions (texture bytes + params), ordinary public ctor, safe in a vector.

Touch points that had to mirror the existing `FacetsAnnotation` pattern (see `supported_facets` for
the template): all constructors' asserts, copy ctors' init lists, the `-1`-id deserialization ctor,
`set_new_unique_id()`, cereal `save`/`load`, `is_texture_displacement_painted()`, and
`reset_extra_facets()` (called whenever a topology-changing op like Simplify or subdivision
replaces the mesh — this is what drops any unbaked texture-displacement paint, since there's no
remap-across-topology-change support for it yet, see Limitations).

### Bake algorithm (`libslic3r/TextureDisplacement.cpp`)

`build_texture_displacement(base_mesh, layers, facets_data)` is **accumulate-then-displace, and
topology-preserving**: the returned mesh has exactly the input's vertices and triangles, in the same
order — only the positions of displaced vertices differ.

1. `its_compactify_vertices()` on a copy of the input. In practice a no-op (it only drops
   *unreferenced* vertices, and preserves the order and indices of the rest). It is there to
   guarantee the index alignment step 3 depends on.
2. Area-weighted vertex normals of the **undisplaced** mesh, computed once. Every layer both
   projects and displaces along these, so a vertex covered by several layers moves along one single
   well-defined direction.
3. For each layer in slot order: deserialize its stored paint mask into a `TriangleSelector` against
   the **base mesh** (never against a previous layer's output), then
   `selector.get_facets_strict(ENFORCER)` → the painted patch. Two facts are exploited:
   - `get_facets_strict()` returns the mesh's **entire** referenced vertex array regardless of which
     state was asked for — only `.indices` is filtered by state. So `get_facets_strict(ENFORCER)`
     and `get_facets_strict(NONE)` share identical vertex indexing, which is what lets boundary
     detection be a plain index check instead of a position-hash lookup.
   - The selector's vertex array *starts with* the mesh's own vertices (extra ones created where a
     brush stroke split a triangle are appended after them), and `get_facets_strict()` emits the
     referenced ones in order. Combined with step 1, **selector vertex index `i` is our vertex `i`**.
     Split vertices live past the end of our array and are simply skipped — they sit on the paint
     boundary anyway (splitting only happens at partial coverage), so they would be pinned regardless.
4. A vertex used by at least one **unpainted** triangle is a boundary vertex — pinned, never
   displaced (its final position is ambiguous, it belongs to both regions). Only vertices used
   exclusively by painted triangles get displaced. This is what keeps bakes seamless with zero
   remeshing/hole-filling at the seam.
5. Per interior vertex: sample the height texture (`sample_layer_height()`, see Projection methods)
   and fold `height * depth_mm * (invert ? -1 : 1)` into that vertex's running total via the layer's
   `TextureBlendMode` (see Blend modes). A `visited` set makes each layer fold in exactly **once**
   per vertex, no matter how many of the patch's triangles share it — otherwise a Multiply/Subtract
   layer would apply two or three times over depending on local triangle fan-out.
6. Finally, move each touched vertex along its (step 2) normal by its accumulated total.

**This replaced a sequential design** that re-meshed after each layer and carried the next layer's
paint mask onto the result with `TriangleSelector::remap_painting()`. That was the root cause of the
reported "the second texture is never applied" bug: remapping a mask onto a mesh whose vertices had
just been displaced out from under it routinely produced an empty bitstream, and the layer was then
silently `continue`d past. It was also what forced the per-layer vertex duplication and the final
`its_compactify_vertices()` weld. The current formulation has neither problem, is substantially
faster (no remap, no welding, one pass), makes blend modes possible at all (they need a shared
per-vertex accumulator, which sequential re-meshing cannot provide), and — because the output keeps
the input's exact vertex indexing — lets GUI code overlay a preview on the base mesh with no index
translation.

### Blend modes

`TextureBlendMode` {Add, Subtract, Multiply, Divide}, per layer, applied per vertex against the
total accumulated by the layers **below** it (lower slots). The quantity blended is a signed
displacement in **mm**, not a pixel value.

Add/Subtract are self-explanatory. Multiply/Divide are *scaling* operations and so need a unit
convention: they treat the layer's own value as a **factor relative to 1 mm**. That makes `depth_mm`
a gain, and — the property that makes a Multiply layer usable as a mask — a layer with depth 1 mm
sampling a white (1.0) texel multiplies by exactly 1, i.e. leaves the layers below unchanged.
Divide floors its divisor's magnitude at 0.05 — a black texel samples to *exactly* zero, so the
divisor really does hit zero in ordinary use, and an unbounded `1/0` would fling vertices thousands
of mm away and poison the mesh's bounding box (and every plate/print-volume check downstream). The
floor doubles as a cap on how far Divide can amplify the relief beneath it: at most 20×.

The **lowest painted layer ignores its blend mode**: it has nothing beneath it, and Multiply/Divide
against an implicit zero base would annihilate (or blow up) it. Enforced in `build_texture_
displacement()` (the first layer to reach a given vertex always folds in additively) and surfaced in
the UI, which labels that layer "Base layer" instead of offering a control that silently does nothing.

### Projection methods

Four choices per layer (`TextureProjectionMethod`), all funneling through `apply_uv_transform()`
(scale by `1/tiling_scale`, rotate by `rotation_deg`, add `offset`). They are dispatched by
`sample_layer_height()`, which returns a **height**, not a UV — because Triplanar takes three
texture samples per vertex and so has no single UV that represents it.

- **Triplanar** (default) — samples the texture on all three world planes (`(y,z)`, `(x,z)`, `(x,y)`)
  and blends the three by the vertex's own normal raised to `TRIPLANAR_BLEND_SHARPNESS` (4).
  This is the fix for a real, user-reported bug. The previous version *hard-picked* the single axis
  most aligned with the normal, which is discontinuous wherever that dominant axis flips: on a +X
  face the planar coordinate is `(y, z)`, on a −Y face it is `(x, z)`, so at the shared edge `u`
  jumps from `y_edge` to `x_edge`. On a box centred near the origin those two happen to **agree** at
  the (+,+) and (−,−) corners and **differ by the full corner width** at the (+,−) and (−,+) corners
  — which is exactly the "two bad corners, two good ones" symmetry that was observed. A weighted
  blend is continuous across the transition by construction, since the weight of the axis being left
  behind falls smoothly to zero. (Note this removes the hard *seam*; some cross-fade blurring in the
  band right at a 90° edge is inherent to triplanar mapping. A genuinely seam-free wrap around a box
  needs a real unwrap — that is what the LSCM mode is for.)
- **Cylindrical** — wraps around an axis through the patch centroid, axis auto-picked as the world
  axis *least* aligned with the average normal (perpendicular to the outward radial normal, as a
  cylinder's own axis would be). `u = angle * local_radius` (arc length in mm), `v = distance along
  axis`. Approximation, not an exact fit for arbitrary geometry.
- **Spherical** — longitude/latitude around the centroid, scaled by local radius. Same caveat.
- **LSCM** — real UV unwrap via `MeshBoolean::cgal::parameterize_lscm()` (CGAL's
  `Surface_mesh_parameterization` package, LSCM algorithm). Computed **once per patch** (not
  per-vertex like the others — it's a single global least-squares solve), then each vertex looks up
  its precomputed UV. Requires the patch to be a single topological disk (one connected component,
  one boundary loop) — `compute_lscm_uvs()` returns empty and the layer silently falls back to
  Triplanar if not (e.g. multiple disconnected painted islands, or a fully closed patch).
  CGAL's parameterizer needs a mesh with no isolated/unreferenced vertices, but `get_facets_strict()`
  returns the *whole* mesh's vertex array — so there's a compaction step
  (`compact_patch_with_map()`) that builds a clean sub-mesh + an index map back to the original
  (uncompacted) vertex numbering, purely local to this file.
- **ViewProjected** ("From view") — a flat projection along a fixed direction captured from the 3D
  camera, like a slide projector. `capture_view_projection()` takes the camera's right/up axes,
  transforms them into the volume's *local* frame (so the projection rides along if the part is later
  moved), and stores them as `TextureDisplacementLayer::view_project_right/up` (unit vectors, so the
  projected coordinate stays in mm and `tiling_scale` keeps meaning mm). `sample_layer_height()`
  projects `Vec2f(dot(pos, right), dot(pos, up))`. Single-valued per point, so — like LSCM but unlike
  blended Triplanar — the fast preview and UV-check overlay precompute it per vertex
  (`compute_layer_vertex_uvs()`) and drive the shader's `use_vertex_uv` path. Faces angled away from
  the projector smear; that is inherent to view projection, not a bug.

  Two companions to this mode:
  - **Projection frame overlay** (`TextureProjectorFrame`, see below) — a semi-transparent window
    dragged over the 3D view whose border becomes the projection's edge. Applying it stores an exact
    **projective** map in `view_project_matrix`, which supersedes the affine `right`/`up` axes above
    for that layer (`view_project_projective`).
  - **"Project only on visible"** (`select_visible_faces()`) — repaints the layer with exactly the
    facets the camera can see, so the projected area matches the viewpoint the projector was captured
    from. Two tests: a facing test (normal vs. view direction, per triangle — under perspective the
    view direction varies across the model, so it is taken from the eye to each centroid), then
    `MeshRaycaster::get_unobscured_idxs()` on the survivors to drop facets hidden behind other
    geometry, so a concave part's far inner wall is correctly excluded. One ray query per front-facing
    facet, hence click-driven (on the checkbox and on each "Capture current view"), never per frame.
    It **replaces** the layer's paint rather than adding to it — "project onto what I can see" would
    otherwise accumulate every angle the user had ever looked from.

### Manual seams and island cutting

`TextureDisplacementLayer::lscm_seam_edges` — undirected mesh-vertex-index edge pairs the unwrap is
forced to cut along, on top of the dihedral-angle seams. `segment_into_charts()` takes a set of these
(translated from mesh → compacted-patch numbering inside `compute_patch_unwrap()`) and refuses to
union two triangles across a marked edge whatever their angle. Both the unwrap cache key and the
gizmo's `UVEditorState` include the seam list, so marking a seam (which leaves the paint mask
untouched) still forces a re-solve. Like the paint masks, seams are mesh-index-space and so dropped on
any topology change.

Two ways to write to it:
- **Mark seam (manual, #9)** — a "Mark seams" click mode (`m_seam_edit_mode`) that suppresses
  painting. A click raycasts the volume (`m_c->raycaster()->raycasters()[idx]->unproject_on_mesh()`,
  `idx` = the volume's slot among model-part volumes), finds the facet's edge nearest the hit point,
  and toggles it. Marked edges render as a red overlay (`render_seam_overlay()`), pulled toward the
  camera so they read on top. This is the Blender mark-seam workflow.
- **Cut island (auto, #17)** — `cut_island()` takes the selected chart's triangles (back-mapped from
  the unwrap via `source_vertex`), finds their 3D bounding box, and marks every edge that straddles
  the mid-plane perpendicular to the longest axis. The re-unwrap then splits the chart across its
  narrow waist — the "islands might be very long" case. Exposed as the UV pane's **Cut** button.

### UV-check overlays (checker / distortion)

`resources/shaders/{110,140}/texture_displacement_uvcheck.{vs,fs}`, one shader with a `mode` uniform,
drawn over the painted patch (`rebuild_uvcheck_mesh()`/`render_uvcheck_mesh()`, P3N3T2: `normal.x` =
distortion, `tex_coord` = uv), pulled forward with a polygon offset. **Checker** (#13) samples a
procedural checkerboard at the layer's uv (per-vertex for LSCM/ViewProjected, in-shader triplanar
otherwise) — squares that stay square mean low distortion. **Distortion** (#14) colours each triangle
blue→green→red by `log2(uv_area / surface_area)` centred on the patch's *median* stretch (so a
globally-scaled unwrap reads as uniformly ideal and only relative stretch shows), averaged to
vertices. A separate **Show mesh wireframe** toggle (#8) draws the whole volume's triangle edges,
rebuilt only when the vertex count changes (not per stroke).

### Tiling

`DecodedHeightTexture::sample(uv, tile_enabled, tile_method)`. Two tile methods when enabled
(Repeat, MirroredRepeat). **When `tile_enabled` is false, sampling outside `[0,1)` returns `0`
directly** — clamping the *coordinate* into range (what an earlier version did) instead smears the
border row/column of pixels outward to infinity in every direction, which is a real bug that was
reported and fixed (visually: streaky lines radiating out from the painted patch).

### Subdivision (`subdivide_mesh_uniform()`)

Deliberately **whole-mesh and uniform**, not limited to the painted patch. A patch-only /
adaptive subdivision would create a classic T-junction/cracking problem where the denser
(subdivided) and sparser (untouched) regions meet — the fine side has edge midpoints the coarse
side doesn't know about, producing a real (non-manifold-looking) crack in the baked geometry. This
was consciously scoped down from the original plan's "adaptive per-patch subdivider" idea to avoid
that correctness risk (a subtly-cracked mesh is a much worse outcome than "not implemented yet").
Algorithm: recursive 1-to-4 triangle split via edge midpoints, with a shared per-pass midpoint cache
(keyed by sorted vertex-index pair) so triangles sharing an edge get the *same* new vertex — capped
at `max_iterations` (default 6) passes to bound worst-case triangle-count explosion.

Wired as a "Subdivide steps" slider (**0–5**, where 0 means no subdivision and previews nothing) plus
Preview/Apply/Done in the gizmo panel. **Apply snaps the slider back to 0** — see bug #22: leaving it
at the count just committed made the panel immediately re-preview N more passes on top of a mesh 4^N
times denser, which is what the "subdivide lags" report was. A real, committed geometry change (like
Bake), using the same `save_painting()`/`set_mesh()`/`restore_painting()` dance `GLGizmoSimplify`
uses: supported/seam/mmu/fuzzy-skin masks get remapped onto the new triangles, texture-displacement
paint does not (no remap support yet) and is dropped rather than left pointing at now-meaningless
triangle indices.

### Preview pipeline (perf)

`rebuild_preview()` used to call `build_texture_displacement()` **synchronously on the UI thread**
on every stroke-end and every slider release. With multiple painted layers this got slow (each
layer's PNG sampling + vertex welding + `remap_painting()` stacks up). Fixed by moving the actual
computation into `TextureDisplacementPreviewJob` (mirrors `TextureDisplacementBakeJob`'s
process()/finalize() split), queued on the app's shared UI job worker via plain `queue_job()` (not
`replace_job()` — that worker is shared app-wide, including with Bake; `replace_job()` would cancel
an in-flight bake if one happened to be running). A `m_preview_generation` counter discards stale
results if a burst of edits queues several jobs in a row and an older one finishes after a newer one.

Two other real perf fixes worth remembering:
- Sliders in ImGui report "changed" continuously on every drag frame, not just once on release —
  gating the (then-synchronous) rebuild behind "mouse button not currently down" was necessary to
  stop dozens of rebuilds per drag.
- `decode_height_texture()` used to re-decode the same PNG from scratch on every call. Now cached
  in `TextureDisplacement.cpp`, keyed by a `weak_ptr` to the layer's `image_data` (not just the raw
  pointer — a `weak_ptr` correctly detects a freed-then-reused address, where a raw-pointer key
  would alias a stale cache entry onto an unrelated later texture).

### Fast bump preview (GPU-only, no CPU meshing)

`resources/shaders/{110,140}/texture_displacement_bump.{vs,fs}`, registered as
`"texture_displacement_bump"`. Perturbs the *shading* normal from the height texture's local
gradient instead of moving geometry — active-layer-only, toggled via a "Fast preview (normal map)"
checkbox. Vertex format is `GLModel::Geometry::EVertexLayout::P3N3T2`: `normal.x` carries the
per-vertex paint weight (0/1) and `tex_coord` carries a precomputed texture UV, so it can use
`GLModel` normally instead of needing a hand-rolled VBO/VAO manager. Weight buffer is
rebuilt at the same cadence as the true-displacement preview (stroke-end/slider-release), using the
**live** `TriangleSelector` state (not the flushed model facets), so it doesn't lag by a full model
round-trip.

The perturbed normal is the analytic one for a height field `H = ±depth_mm · h(uv)` displaced along
`N` over any orthonormal surface tangent pair `T`/`B`:

    N' = normalize(N − (dH/da)·T − (dH/db)·B),   a = dot(p,T), b = dot(p,B)

The two slopes have to be genuine **mm-per-mm** derivatives for the preview's apparent depth to
match the bake's — see bug #13.

**Two projection paths (`use_vertex_uv` uniform):**
- **Triplanar (`use_vertex_uv = 0`)** — `uv` and the `T`/`B` axes are both derived in-shader from
  the dominant normal component, mirroring `project_planar()`/`apply_uv_transform()`, and the slope is
  formed analytically. `T`/`B` are the projection's axis-aligned pair, exact only when the face is
  axis-aligned; the shader drops the along-normal component to keep the gradient in the surface. Here
  one `uv` unit is exactly `tiling_scale` mm, so the `1/tiling_scale` gradient factor is right.
- **Precomputed UV (`use_vertex_uv = 1`, used for LSCM)** — `uv` comes per-vertex from the CPU
  (`compute_lscm_uvs(patch, layer)`, so island placement + tiling/rotation/offset are already folded
  in), and the perturbed normal is built with **Mikkelsen's method** ("Bump Mapping Unparametrized
  Surfaces on the GPU"): the surface gradient taken directly from the screen-space derivatives of the
  *sampled height* and position. **This makes no uv→mm scale assumption**, which is essential —
  the first cut used the same global `1/tiling_scale` factor as triplanar and the depth came out
  visibly wrong, because an LSCM map is **conformal, not isometric**: it is globally area-scaled but
  the *local* mm-per-uv varies across the chart. `dFdx(h)` captures the true on-screen rate of change
  however the chart is stretched. **This path is also what makes the fast preview follow the UV
  editor: move an island and its uv — hence its bump — moves with it** (the bump mesh rebuilds on
  drag-end, since `on_island_edited(finished)` → `rebuild_preview()` → `rebuild_bump_preview_mesh()`).
  The branch is uniform (`use_vertex_uv` is a uniform) and the paint weight gates by multiply, so the
  texture derivatives stay well defined. A triangle straddling a seam has a discontinuous uv → the
  `det≈0` guard skips it (a localised preview-only artifact, never in the bake).

Remaining deliberate approximation: the GPU sampler's wrap mode stands in for
`tile_enabled`/`tile_method`, so with tiling *off* the GPU repeats where the CPU returns 0 outside
`[0,1)`.

> **Known divergence from the CPU path (not yet reconciled).** The shader's `project_uv()` mirrors
> the *hard-axis* `project_planar()`, but the CPU's Triplanar mode is now a **blended** three-plane
> sample (see Projection methods — that change is what fixed the 90°-corner seam). The two therefore
> still agree on any face that is roughly axis-aligned (one blend weight ≈ 1 there, so the blend
> degenerates to exactly the hard-axis pick), and disagree in the cross-fade band around a sharp
> edge — precisely where the fast preview will still show the old hard seam that the true preview and
> the bake no longer have. Reconciling it means sampling all three planes in the shader and blending
> the three gradients by `pow(abs(N), TRIPLANAR_BLEND_SHARPNESS)`, the same weights
> `sample_layer_height()` uses. Also note the shader is still **active-layer-only** and knows nothing
> about `TextureBlendMode`, so a multi-layer stack cannot match the true preview by construction.

### On-canvas "Adjust Texture" gizmo

A per-active-layer toggle ("Adjust placement (drag on model)") that disables painting and shows a
flat pan panel (free 2D drag on both axes) plus two arrows along the patch's own U/V axes
(constrained single-axis drag). Anchored to the painted patch's centroid/average-normal
(`compute_layer_paint_anchor()`). Hit-testing is screen-space distance/point-to-segment (not real
3D ray intersection against the handle geometry) — simple and good enough at this handle size.

Known unverified detail: the **offset-drag direction/sign** is reasoning-based (increasing `offset`
shifts which texel is sampled at a fixed world position, which visually slides the pattern the
*opposite* way — so the code subtracts), not visually confirmed, since this environment can't
render pixels. May need a one-line sign flip once actually tested. The rotation-arrow-implied
direction should be reliable (it follows directly from a self-consistent 2D basis, no such
ambiguity).

### Projection frame overlay (ViewProjected)

`src/slic3r/GUI/TextureProjectorFrame.hpp/.cpp` — a semi-transparent, resizable `wxFrame` the user
drags **over the 3D view**, like a slide projector's gate. Whatever the model shows through it is what
the texture is projected onto, and the window's border becomes the hard edge of the displacement.
Press **Apply projection frame** and the gizmo reads the window's rectangle and commits it.

The window is deliberately **dumb**: it owns no placement state and reports nothing continuously. Its
position and size *are* the placement, read on demand at Apply — which is also when the expensive
visible-facet raycast runs. So dragging it is free and nothing recomputes until asked.

Plain 2D (`wxPaintDC`), not a `wxGLCanvas`: a second GL canvas would have to share the app's one real
`wxGLContext`, the cause of bugs #10 and #14 below. It only ever draws a bitmap and a border.

**The projective mapping (`apply_projection_frame()`)** is the substantive part. The frame defines a
**screen-space** rectangle, but the bake samples from a **local-space** position, so the two have to be
reconciled. `view_project_right/up` can only express an *affine* projection — exact under an
orthographic camera, but wrong under perspective, where the near end of a part projects larger than the
far end and no pair of axes reproduces that. So the layer instead stores a full projective map
(`view_project_matrix`, row-major 3×4, `uv = (row0·p̃/row2·p̃, row1·p̃/row2·p̃)`), built like this:

- `K = projection · view · (instance · volume)`, i.e. local → clip, the same product the renderer uses.
  Note `Camera::get_projection_matrix()` is typed `Transform3d` (nominally affine) but its perspective
  form explicitly writes a `(0, 0, −1, 0)` bottom row into the underlying 4×4, so `clip.w = −z_eye` is
  genuinely carried. The build therefore multiplies **`.matrix()` products** (plain `Matrix4d`), never
  `Transform3d` products, which would not compose that row correctly.
- Window coordinates follow `igl::project`'s convention (as `CameraUtils::project` does), with y
  measured downward. Writing `uv = (win − rect_origin) / rect_size` makes u and v affine in
  `ndc = clip.xyz / clip.w`; multiplying through by `clip.w` leaves a plain linear combination of `K`'s
  rows, which is exactly the 3×4 matrix — the perspective divide survives intact.
- `w > 0` is checked rather than divided blindly. A point behind the projector has `w < 0` and divides
  to a plausible-looking but **mirrored** uv — the classic way a projected decal reappears on the back
  of a model. `project_uv_projective()` returns false there and the caller treats it as no height.

The map already includes placement, so `apply_uv_transform()` is **not** applied on top of it — the
window's own position and size are the placement, and the tiling/rotation/offset sliders would shove
the result off the frame the user just aligned. A "Clear" button drops back to the affine path where
those controls mean something again.

Apply also sets `tile_enabled = false`, so `DecodedHeightTexture::sample()` returns 0 outside `[0,1)`
and the border is a hard edge rather than the first seam of an endless repeat, and repaints the layer
via `select_visible_faces(&matrix)` — the frame's uv square clips the selection, which both matches the
paint to the border and keeps the ray queries proportional to the framed area instead of the model.

Owned by the gizmo and **destroyed** (not just hidden) in `on_shutdown()`. Closing it only hides it, so
reopening keeps it where it was left.

### UV Editor pane

`UVEditorCanvas` (`src/slic3r/GUI/UVEditorCanvas.hpp/.cpp`) — a standalone `wxGLCanvas` rendering the
flattened LSCM islands (per-island wireframe + outline + fill) over the height texture (background
quad tiled across the whole unwrap), with mouse pan/zoom. It is wrapped in a **`UVEditorPanel`**
(same file) that adds a button row (Frame / Snap / Average scale) and a Blender-style status line
along the bottom naming the current gesture and the shortcuts in play. The *panel* is what is
registered as a `wxAuiPaneInfo` pane on `Plater`'s `m_aui_mgr`; `Plater::show_uv_editor(bool)`
shows/hides it (deferred via `CallAfter`, since the gizmo calls it mid-3D-frame), and
`get_uv_editor_canvas()` returns the inner canvas the gizmo talks to.

Deliberately **shares the app's one real `wxGLContext`** (`wxGetApp().init_glcontext(*this)`, the
same call `View3D`/`Preview`/`AssembleView` make) rather than creating an independent context like
`SkipPartCanvas` does elsewhere in this codebase — this is what lets it reuse the already-registered
`"flat"`/`"flat_texture"` shaders and `GLModel` as-is, instead of needing its own shader
compilation/VBO management.

**Geometry is uploaded once, in the unwrap's own (raw, mm) coordinates**, one `GLModel` set per
island; each island is then drawn through its own 2x3 affine (`island_transform_matrix()` composed
with the layer's tiling/rotation/offset) passed as the `flat` shader's `view_model_matrix`. This is
the fix for the ~200 ms-per-frame island-drag stall (#3): the old design pre-transformed every UV on
the CPU and re-uploaded the entire wireframe on every mouse-move event, which on a million-triangle
patch is exactly as slow as it sounds. Now a drag updates one matrix per island and touches no vertex
buffer — `on_island_edited(!finished)` calls only `set_island_transforms()`, and the full
`set_islands()` rebuild happens solely when the unwrap itself changes (`unwrap_changed` in
`update_uv_editor()`).

**Gestures** (canvas-owned, reported to the gizmo as incremental deltas via `IslandEditFn`): left-drag
= move, right-drag or **R** = rotate (hold **Shift** to snap to 15° steps — quantised on the
*cumulative* rotation, not each delta, so it doesn't judder, and accumulated incrementally so it
survives crossing ±180°), **S** = scale (R/S modal, click/Enter to confirm, Esc to cancel), wheel =
zoom about the cursor, middle-drag = pan, **Home**/**F** = frame all. Scale writes
`TextureIsland::scale`; "Average scale" (`average_island_scales()`) sets every island to the mean, so
one island scaled by hand can be matched back to its neighbours' texel density. **Snap** (canvas-owned
`m_snap_enabled`, toggled from the toolbar) sticks a dragged island's nearest boundary vertex onto a
neighbouring island's at drag-*end* only — a magnet that re-applies mid-drag is very hard to pull out
of. Toolbar commands the canvas can't service itself (Average scale) are forwarded to the gizmo via
`CommandFn`; view-only ones (Frame, Snap) it handles directly.

## Bugs found and fixed this session (worth remembering)

These were all real, confirmed root causes (found by reading the actual code path, not guessed):

1. **Cross-face projection distortion** — see "Triplanar" above. Fixed by projecting each vertex
   with its own normal instead of one shared patch-average normal.
2. **Disabled-tile smearing to infinity** — clamping the UV *coordinate* into `[0,1]` instead of
   returning 0 outside it, when tiling is off. Fixed in `DecodedHeightTexture::sample()`.
3. **Invisible checkbox/radio "checked" state in light mode** — `ImGuiWrapper::push_toolbar_style()`
   sets `ImGuiCol_CheckMark` to white while the checkbox/radio frame background is fully transparent
   (alpha 0) over a light window background — a white checkmark on an effectively-white background
   is invisible by construction. This is a **pre-existing, general app-wide bug**, not specific to
   this feature (every panel using `push_toolbar_style()` in light mode has it) — fixed by changing
   just the light-mode branch's `CheckMark` color to the app's teal accent.
4. **Distorted (non-aspect-correct) texture thumbnails** — was forcing a square `ImGui::Image` size
   regardless of the source image's actual aspect ratio.
5. **`std::array<FacetsAnnotation, 8>` compile error** — see Data model above (MSVC C2280,
   `std::array`'s implicit special members don't inherit element-type friendship).
6. **Eigen ternary expression-template type mismatch** (MSVC C2446) — `cond ? (n / len) :
   Vec3f::UnitZ()` fails because the two branches are different unevaluated Eigen expression
   *types* with no common type; fixed by wrapping the non-`UnitZ()` branch in an explicit
   `Vec3f(...)` to force a concrete common type.
7. **Post-bake stale preview** — `GLGizmoPainterBase::data_changed()`'s change-detection only
   checks object id / volume count, neither of which changes when Bake replaces a volume's mesh
   (same object, same volume count, just a new mesh/id on the volume itself) — so the gizmo kept
   rendering/painting against the pre-bake `TriangleSelectorPatch` until manually deselected and
   reselected. Fixed by explicitly calling `update_from_model_object()` in the bake-completion
   callback.
8. **Bake job / crash-report `resources` junction going stale** — unrelated to this feature's code,
   but hit during testing: `build/src/Release/resources` was a leftover **empty plain directory**
   instead of the junction CMake's post-build step creates (`if not exist` skipped it because the
   empty folder already "existed"), so the built exe couldn't find `resources/data/hints.ini`,
   leaving `HintDatabase`'s hint list empty → `rand() % 0` divide-by-zero crash before the UI ever
   opened. Fixed by deleting the empty folder and manually recreating the `mklink /J` junction.
9. **Fast bump preview showing a solid black object** — `GLModel::render()` **unconditionally**
   re-sets the shader's `"uniform_color"` uniform from its own internal `Geometry::color` field
   (defaulting to `ColorRGBA::BLACK()`) right before every draw call — so a manual
   `shader->set_uniform("uniform_color", ...)` call made just before `.render()` gets silently
   clobbered. Any `GLModel` that needs a specific flat color **must** call `.set_color(...)` on the
   model itself, not set the shader uniform directly. Found by reading `GLModel::render()`'s actual
   source rather than guessing at shader/lighting math.
10. **UV editor canvas rendering nothing / showing stale content on resize** — the canvas requested
    a generic `wxGLAttributes().Defaults()` pixel format while sharing the app's one real
    `wxGLContext` (which was originally created against `View3D`'s canvas, itself requesting a
    specific RGBA/24-bit-depth/8-bit-stencil format). `wxGLCanvas::SetCurrent()` on WGL/GLX
    generally requires the target window's pixel format to be compatible with the one the context
    was created against; a mismatch can make `SetCurrent()` silently fail, leaving the canvas
    showing whatever was last in its backbuffer (looks exactly like "blank" or "stale image on
    resize"). Fixed by requesting the same explicit attribute list
    `OpenGLManager::create_wxglcanvas()` uses for the main view canvases — but see bug #14: the
    first attempt at this copied only *part* of that list and the symptom therefore survived.
11. **`<glad/gl.h>` / `<wx/glcanvas.h>` include-order conflict** — `wx/glcanvas.h` pulls in the
    platform's real `GL/gl.h`; if that happens before `<glad/gl.h>` is processed in the same
    translation unit, glad's own header errors out (`OpenGL (gl.h) header already included`).
    Fixed by including `<glad/gl.h>` first in `UVEditorCanvas.hpp`, before `<wx/glcanvas.h>` — any
    file that includes this header (including `Plater.cpp`, transitively) needs glad to win that
    race.
12. **Fast preview hidden behind the paint-highlight overlay** — `render_painter_gizmo()` always
    drew the selection-highlight overlay on top with a depth-bias trick (`glPolygonOffset`) that
    only makes sense for the *true*-displacement preview: real geometry moves in the painted area,
    so the depth-biased overlay only wins the depth test in the *unpainted* (coincident) region.
    The bump preview never moves geometry — its depth is identical to the overlay's *everywhere* —
    so the overlay was winning the depth test across the whole surface and hiding the bump shading
    entirely. Fixed by skipping the overlay draw entirely when the bump-preview path is active.
13. **Fast preview's apparent depth not matching the true preview's** — the bump shader built its
    perturbed normal as `normalize(N + depth_mm * vec3(hL-hR, hD-hU, 0))`. Two things wrong with
    that. (a) `hL-hR` is a height difference across *one texel step*, i.e. `dh/du` already scaled by
    `2·texel`, and `du` is in uv units, not mm — the real surface slope needs the full chain rule
    back through `uv = R(rotation) · planar_mm / tiling_scale`, i.e. a further `1/tiling_scale` and
    a rotation of the gradient by `−rotation`. The missing `1/(2·texel·tiling_scale)` factor is
    ~26× at a 1024px texture and a 20mm tile size, all in the flattening direction — which is
    exactly what "fast preview has a different height from the real preview" looks like. (b) the
    gradient was added to model-space `xy`, but the two axes the planar projection actually runs
    along are `yz`/`xz`/`xy` depending on the dominant normal component, so on any face not
    dominated by `z` the perturbation was applied to the wrong axes. Fixed by computing the real
    mm-per-mm slope and rotating it into the projection's own `T`/`B` axes (see "Fast bump preview"
    above for the derivation).
14. **UV editor pane still blank after bug #10** — three separate causes, all of them live at once:
    - The bug-#10 fix copied `OpenGLManager::create_wxglcanvas()`'s attribute list but **dropped its
      multisampling attributes** (`WX_GL_SAMPLE_BUFFERS`/`WX_GL_SAMPLES`, 4 samples by default), on
      the reasoning that a flat 2D wireframe view doesn't need AA. But a differing sample count *is*
      a differing pixel format, so this left exactly the `wglMakeCurrent()` mismatch bug #10 set out
      to fix. It now mirrors the full list, AA included, reading `OpenGLManager::can_multisample()`
      (already resolved by then — `View3D` is constructed first).
    - `set_mesh()`/`set_background_texture()` each called `render()` **inline**, and both are reached
      from `update_uv_editor()` → `rebuild_preview()` → the gizmo's ImGui panel — i.e. from the
      middle of the *3D* canvas's GL frame, and (since `show_uv_editor(true)` is the last line of
      `update_uv_editor()`) while this pane was still **hidden**. `wxGLCanvas::SetCurrent()` returns
      false outright on a canvas that isn't shown on screen, and the old code ignored the return
      value — so every GL call in `render()`, `glViewport`/`glClear` included, silently landed on the
      3D canvas instead. These now only mark dirty + `Refresh()`; `render()` bails unless
      `IsShownOnScreen()` *and* `SetCurrent()` succeeds; and `Plater::show_uv_editor()` defers its
      AUI relayout via `CallAfter` so the pane's first size/paint can't be delivered mid-frame either.
    - Even once drawing, nothing would have been *visible*: the background quad spanned `[-1,1]²`
      while LSCM UVs land around `[0,1]²`, and the view was centered on the origin at a half-extent
      of 0.6. The quad is now the unit square in the same UV space the wireframe uses (which is also
      where `sample()` maps the texture, regardless of its pixel aspect), the projection's Y is
      negated so `v` runs down-screen (putting the texture's first pixel row at the top rather than
      upside down), and the view auto-fits to the unwrap ∪ unit square the first time a patch shows up.
15. **A second texture layer was silently never applied** — the reported "multiple textures don't
    work reliably". Root cause was the old sequential bake: layer N's paint mask was stored against
    the volume's original mesh, so before layer N+1 could be deserialized the mask had to be carried
    onto the mesh layer N had *just displaced*, via `TriangleSelector::remap_painting()`. Remapping a
    mask onto geometry that has moved out from under it routinely returned an empty bitstream, and
    the code then did `if (data.bitstream.empty()) continue;` — i.e. dropped the layer **without any
    diagnostic**. Fixed structurally rather than patched: every layer is now evaluated against the
    base mesh and merged per vertex (see Bake algorithm), so no remap happens at all. Covered by a
    regression test.
16. **Hard-axis triplanar seam at exactly two of a box's four vertical corners** — see "Triplanar"
    under Projection methods. Worth recording the *diagnostic* here, because the asymmetry is what
    pinned it down: the user reported the seam at the (X+,Y−) and (X−,Y+) corners with the other two
    clean. That is precisely what a dominant-axis switch predicts (`u = y` on an X face, `u = x` on a
    Y face; those agree where `x == y` and differ by the corner width where `x == −y`) and it ruled
    out every "the texture is wrong" hypothesis, since the texture itself is fine — the *mapping* is
    discontinuous. Fixed by blending the three axis projections instead of picking one.
17. **Use-after-free when removing a texture layer** (latent, pre-existing — found while touching the
    panel, not caused by it). The layer list's "Remove" button called `remove_texture_layer()` *in
    the middle of rendering that layer's row*. That erases the layer from
    `mv->texture_displacement_layers`, shifting every later element down — after which the loop
    happily carried on dereferencing `layer` for the rest of the row's widgets (depth/tiling sliders,
    `PopID`) and kept iterating `ordered`, a vector of pointers into the storage that had just moved.
    Never crashed loudly because `vector::erase` doesn't reallocate, so the reads landed on a *valid*
    but *wrong* (shifted) layer. Fixed by recording the slot and doing the removal after the loop.
18. **Every LSCM island collapsed to a point** (the UV editor was empty; the Tile-size slider did
    nothing; an LSCM bake came out as a flat "single-face extrude"). One line in
    `compute_patch_unwrap()`: `chart.indices = std::move(chart_mesh.indices);` ran *before*
    `area_3d(chart_mesh)` was taken. `area_3d()` iterates those indices, so on the moved-from
    (emptied) mesh it returned `0`, giving `scale = sqrt(0 / uv_area) = 0` — **every chart's UVs
    multiplied by zero**. That one zero explained all three symptoms at once (no island extent to
    draw; a zero-size unwrap is still zero after any tiling divide; the bake sampled ~one constant
    texel per chart). Fixed by measuring the 3D area before the move. Found only by instrumenting the
    actual island bbox into the panel — three rounds of reasoning from screenshots had each guessed
    wrong, because a collapsed-to-a-point unwrap and an off-screen-framed one look identical.

19. **Isotropic remeshing produced scrambled geometry with dropped triangles** — the "remeshing kinda
    does not work properly" report, and a genuine one-line root cause. `isotropic_remeshing()` edits
    the `Surface_mesh` **in place**, and its edge collapses only *mark* vertices and faces as removed;
    the underlying arrays keep the holes until `collect_garbage()` is called. That matters because the
    shared `cgal_to_indexed_triangle_set()` numbers its output vertices by **iteration order** (which
    skips removed slots) while reading each face's corner as the **raw integer value of the vertex
    descriptor** (which does not). The two agree only up to the first collapse; past that every
    triangle indexes the wrong vertices, and any descriptor beyond the live vertex count hits the
    converter's `iv >= vsize` guard and is silently dropped *together with its triangle*. Fixed by
    compacting (`cgal_mesh.collect_garbage()`) before converting. Note the pre-existing callers were
    unaffected: the boolean ops build their result into a fresh mesh, so only the in-place remesher
    ever handed the converter a mesh with garbage in it.
20. **Remeshing rounded off every sharp edge** — the second half of the same report. CGAL's
    `isotropic_remeshing` relaxes vertices tangentially along the surface, which erodes hard features
    unless they are constrained: a cube came back with wobbly, eroded edges. Fixed by detecting sharp
    edges (`PMP::detect_sharp_edges()` at a user-settable dihedral angle, default 40°) plus every open
    border, constraining them, and passing `protect_constraints(true)`. That option additionally
    requires each constrained edge to already be shorter than 4/3 · target, hence the
    `PMP::split_long_edges()` pre-pass (given the same map, so the halves inherit the constraint) —
    this mirrors CGAL's own isotropic_remeshing example. Also added a guard for the case where
    `Surface_mesh::add_face()` refused faces on a non-manifold input: remeshing a mesh that silently
    lost faces yields a **punctured** model, so it now bails out and reports instead.
21. **Remesh falsely reporting "did not change the model"** — the GUI decided success by comparing
    *vertex counts*. A remesh that redistributes triangles at roughly the current density legitimately
    lands on the same count, so a perfectly good result was discarded with an error. Now compared
    structurally against the input (which is what `remesh_isotropic()` hands back on failure).
22. **Subdivide re-previewing at the same count after Apply** — Apply committed N passes and then
    immediately rebuilt the *preview* at N passes again, now on top of a mesh up to 4^N times denser.
    That is the single most expensive thing the panel can do, and it ran on every Apply. The slider
    now starts at 0 (a real "no subdivision" value that previews nothing), and Apply snaps back to it.

## Known limitations / deferred work

- **No `.3mf` serialization** for texture-displacement paint data or texture assets. A background
  agent attempted this in an earlier session, hit its own usage limit mid-edit, and left
  `bbs_3mf.cpp` with an undefined forward-declared function; that partial edit was reverted rather
  than shipped broken. Practical impact: **baked** geometry round-trips fine (it's just an ordinary
  part of the mesh via the existing mesh serialization path) — what does *not* survive a project
  save/reload is any *unbaked* paint stroke and texture layer definition.
- **No remap-across-topology-change** for texture-displacement paint (`ModelObject::split()`, mesh
  boolean ops, Simplify, and now `subdivide_mesh_uniform()` all drop it via `reset_extra_facets()`).
  The other four paint channels (supported/seam/mmu/fuzzy) do get remapped in these cases.
- **Cylindrical/Spherical axis/center are auto-picked heuristically**, not user-controllable — no
  UI to override the auto-detected wrap axis if it picks the "wrong" one for an odd shape.
- **Fast preview covers the active layer only**, while the true preview stacks every painted layer —
  so with more than one layer painted the two will legitimately not agree, independently of bug #13.
- **Bump preview and true preview both only refresh at stroke-end**, not continuously during an
  active drag (a deliberate scope cut for simplicity/consistency — the original plan's "instant
  update mid-stroke" idea for the bump shader specifically was not carried through).
- **On-canvas Adjust-Texture gizmo's offset-drag direction is unverified** (see above).
- **The bump shader still uses hard-axis, single-layer projection** while the CPU path is now
  blended-triplanar and blend-mode aware — see the callout under "Fast bump preview".
- **Displacement resolution is capped by the mesh's own vertex density.** Baking only ever *moves*
  existing vertices (it never inserts any), so a coarse patch cannot show fine texture detail no
  matter how high-resolution the height map is — that is what the "Subdivide model" button is for.
  Since the rewrite the bake is topology-preserving, so this is now a hard, explicit property rather
  than something partly papered over by the old per-layer re-meshing.
- **Placeholder toolbar icon** — reuses `toolbar_fuzzy_skin_paint.svg`, noted as a TODO in code.
- **Textures are matched to the picker by absolute path** (`TextureDisplacementLayer::path`), so the
  picker's "which entry is selected" highlight goes blank if a project is moved between machines.
  Harmless — the layer keeps its own embedded `image_data` and still bakes correctly.

### Requested UV-editing features — status

A user working through the feature end-to-end asked for a batch of UV-editing features. All of the
functional ones are now implemented (see the sections above): checker (#13), distortion heatmap
(#14), mesh wireframe overlay (#8), cut island (#17), project-from-view (#6), and Blender-style mark
seam (#9), plus per-island fills, the fast preview honouring the LSCM unwrap and island moves (#1),
the UV pane toolbar + status line (#18), Shift-snap rotation, midlevel/bidirectional displacement
(#19), and island scale/average/padding/snap (#15/#16/#2).

Remaining cosmetic / known gaps:
- The UV pane toolbar has **text buttons, not icons** — no existing SVG reads cleanly as "average
  island scale" / "snap islands", so real icons are deferred rather than mis-assigned.
- **Mark-seam edge picking** snaps to the nearest edge of the *hit facet* only; it does not
  highlight the candidate edge on hover before you click (a hover-preview would be a nice refinement).
- **Cut island** always halves along the longest 3D axis; there is no UI to pick the cut line.

## File map

**libslic3r (core, no GUI dependency):**
- `src/libslic3r/TextureDisplacement.hpp/.cpp` — data model, bake algorithm, projection methods,
  tiling, subdivision. See doc comments throughout, they're kept accurate and up to date.
- `src/libslic3r/MeshBoolean.hpp/.cpp` — added `parameterize_lscm()` and `remesh_isotropic()`
  (sharp-feature-preserving isotropic remeshing, see bugs #19–#21) in the `cgal` sub-namespace,
  reusing the existing `CGALMesh`/`_EpicMesh`/conversion-helper infrastructure already there for
  mesh boolean ops. New CGAL includes: `Polygon_mesh_processing/border.h`,
  `Polygon_mesh_processing/connected_components.h`, `Surface_mesh_parameterization/{Error_code,
  LSCM_parameterizer_3, parameterize}.h`. No new dependency — CGAL 5.6.3 is already vendored and
  the `Surface_mesh_parameterization` package headers were already present, just unused before now.
- `src/libslic3r/Model.hpp/.cpp` — the 8 named `FacetsAnnotation` fields + accessor,
  `texture_displacement_layers`, and all the mirrored touch points (see Data model above).

**GUI:**
- `src/slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp/.cpp` — the gizmo. Panel controls: dock/
  undock toggle, brush/face/connected-area selection mode + "select whole model" button, per-layer
  texture picker + depth/tiling/rotation/invert/tile-mode/projection-mode/blend-mode controls,
  "Adjust placement" toggle (on-canvas gizmo), "Fast preview (normal map)" toggle, "Subdivide model"
  button, Add layer/Erase all/Bake.
- `src/slic3r/GUI/TextureLibrary.hpp/.cpp` — scans the shipped + user texture folders, imports an
  arbitrary image into the user folder (converting it to the 8-bit grayscale PNG libslic3r decodes),
  and loads a library file's bytes for a layer. The image→grayscale-PNG conversion lives here, on the
  GUI side, because libslic3r has no image toolkit; both the import path and the "pick a shipped
  texture" path go through the same one function.
- `resources/textures/displacement/*.png` — the 10 shipped height maps (Bricks, Grid, Hexagons,
  Knurl, Noise, Quilt, Studs, Waves, Weave, Wood Grain). All 512×512 8-bit grayscale and **seamless**
  (each is periodic over the full image in both axes, so tiling shows no seam). Generated
  procedurally; the whole `resources/` tree is installed recursively by CMake, so a new folder under
  it ships with no build-system change.
- `src/slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp/.cpp` — background bake commit.
- `src/slic3r/GUI/Jobs/TextureDisplacementPreviewJob.hpp/.cpp` — background preview compute
  (mirrors the bake job's shape but commits nothing to the Model).
- `src/slic3r/GUI/TextureProjectorFrame.hpp/.cpp` — the semi-transparent projection-frame overlay for
  ViewProjected layers (plain 2D `wxPaintDC`, no GL context — see its section above).
- `src/slic3r/GUI/UVEditorCanvas.hpp/.cpp` — the 2D UV unwrap viewer widget.
- `src/slic3r/GUI/Plater.hpp/.cpp` — `uv_editor_canvas` member, AUI pane registration,
  `get_uv_editor_canvas()`/`show_uv_editor()`.
- `src/slic3r/GUI/GLShadersManager.cpp` — registers `"texture_displacement_bump"`.
- `resources/shaders/{110,140}/texture_displacement_bump.{vs,fs}` — the bump-preview shader.
- `src/slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp` — `PainterGizmoType::TEXTURE_DISPLACEMENT`.
- `src/slic3r/GUI/Gizmos/GLGizmosManager.hpp/.cpp` — `EType::TextureDisplacement` registration.
- `src/slic3r/GUI/ImGuiWrapper.cpp` — the light-mode checkmark-color fix (bug #3 above; a
  pre-existing, general bug, not scoped to this feature).

**CMake:** all new source files added to `src/libslic3r/CMakeLists.txt`,
`src/slic3r/CMakeLists.txt` (in roughly-alphabetical position matching each list's existing
convention), and `tests/libslic3r/CMakeLists.txt` for the unit test file.

**Tests:** `tests/libslic3r/test_texture_displacement.cpp` — **run and passing** (7 cases, 116
assertions). Covers `decode_height_texture` round-trip, empty-layer no-op, full-cube uniform
displacement, boundary-vertex pinning on a hand-built fan mesh, and — added with the bake rewrite —
a regression test that a **second layer over the same area actually contributes** (the bug that
rewrite fixed), a table-driven check of all four blend modes, and that the lowest layer ignores its
blend mode. `BUILD_TESTS` is `OFF` in the checked-in build cache; flip it on to run them:

    cmake -S . -B build -DBUILD_TESTS=ON
    cmake --build build --config Release --target libslic3r_tests -- -m
    ./build/tests/libslic3r/Release/libslic3r_tests.exe "[TextureDisplacement]" --order rand

## Build notes

- Everything here lives in `libslic3r`/`libslic3r_gui`/`libslic3r_cgal` — no new external
  dependency, no `deps/` rebuild needed. CGAL's parameterization package was already vendored.
- To build just this feature's code path fastest: `cmake --build build --config Release --target
  libslic3r_gui -- -m` (pulls in `libslic3r` and `libslic3r_cgal` as needed). The full app target
  is `OrcaSlicer_app_gui` (produces `build/src/Release/orca-slicer.exe`) — only needed to actually
  run and visually test, not to verify compilation.
- `BUILD_TESTS` is `OFF` in the existing build cache; flip it on to actually run
  `test_texture_displacement.cpp`.
