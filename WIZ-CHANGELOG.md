<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- Copyright Contributors to the OpenColorIO Project. -->

# Wizard fork changes

This file records the changes carried by the `story-wizard/OpenColorIO` fork on
top of the upstream branch it is based on. It exists because the fork is
consumed by a downstream project (`story-wizard/wizard-core`) that pins a
specific SHA, and some of these changes alter pixel output — a reader diffing
two builds needs to be able to tell which change did it.

Upstream's own release notes live on the GitHub Releases page and in
`CHANGELOG.md`; nothing here duplicates them.

Every entry is meant to be temporary. The fork carries a change only until the
equivalent fix lands in an upstream release, at which point the entry is removed
along with the commit.

## `wiz/RB-2.5`

Based on upstream `RB-2.5` at `c52966a6` (= v2.5.2).

### Changes pixel output

- **Use the op's log exposure step in the log inverse CPU renderer.**
  **CPU only — the GPU path was already correct.** `ECLogarithmicRevRenderer`
  never copied `logExposureStep` out of the op data, so it scaled exposure by
  the 0.088 default no matter what the op was given, while the shader built by
  `AddECLogarithmicRevShader()` read it directly. The two renderers of the same
  op therefore disagreed. CPU `STYLE_LOGARITHMIC_REV` results change for any op
  whose step is not 0.088 — an ACEScct (0.057) or LogC (~0.074) config — by
  `exposure * (step - 0.088)`, and move onto the value the GPU was already
  producing. Ops left at the default are unaffected, as are the other five
  styles, and no generated shader text changes.
  This is an upstream defect, present in v2.5.2 and on upstream `main`.
  Upstream status: not yet submitted.

### No pixel change

- **Allow the ExposureContrast pivot to be a dynamic property.** Exposure,
  contrast and gamma could each be driven from a viewport control without
  rebuilding the processor; the pivot could not, and was folded into the
  generated shader as a literal, so changing it forced a shader recompile. A new
  `DYNAMIC_PROPERTY_PIVOT` and `makePivotDynamic()` let it reach the shader as a
  uniform instead. The pivot stays non-dynamic by default, so a static pivot
  produces byte-identical shader text and byte-identical CPU results; only a
  transform that opts in generates different source. Note that a dynamic pivot
  is deliberately not serialized — the CTF `DynamicParameter` vocabulary has no
  PIVOT token — and that this inserts virtuals into the exported
  `ExposureContrastTransform` interface, so the library is not binary-compatible
  with a stock 2.5 build (harmless while wizard-core builds from source against
  a pinned SHA).
  Upstream status: not yet submitted. (fork PR #3)

- **Hold constant references to array uniforms in the Metal class wrapper.**
  The generated MSL struct took owning copies of every array uniform and
  rebuilt them once per shader invocation, which for a compute kernel
  dispatched one thread per pixel meant ~496 private-memory writes per pixel on
  any curve op. It now points at constant memory instead. Output texels are
  bit-identical; only the generated shader text changes.
  Upstream status: not yet submitted. (wizard-core#895, fork PR #1)
