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

_(none yet)_

### No pixel change

- **Hold constant references to array uniforms in the Metal class wrapper.**
  The generated MSL struct took owning copies of every array uniform and
  rebuilt them once per shader invocation, which for a compute kernel
  dispatched one thread per pixel meant ~496 private-memory writes per pixel on
  any curve op. It now points at constant memory instead. Output texels are
  bit-identical; only the generated shader text changes.
  Upstream status: not yet submitted. (wizard-core#895, fork PR #1)
