# Contributing to Entropic

Thanks for your interest in contributing. Entropic is a personal
project of Tristan VanFossen distributed under LGPL-3.0-or-later
with the BISSELL Homecare Inc. Linking Exception (see `NOTICE`).
This document covers the rules for proposing changes.

---

## Developer Certificate of Origin (DCO)

Every commit must be signed off under the [Developer Certificate of
Origin v1.1](https://developercertificate.org/). The DCO is a
lightweight per-commit attestation — copy below — that you wrote
the code or otherwise have the right to submit it under the
project's license.

By signing off on a commit, you certify that:

> 1. The contribution was created in whole or in part by you and
>    you have the right to submit it under the open-source license
>    indicated in the file; or
> 2. The contribution is based upon previous work that, to the best
>    of your knowledge, is covered under an appropriate open-source
>    license and you have the right under that license to submit
>    that work with modifications, whether created in whole or in
>    part by you, under the same open-source license (unless you
>    are permitted to submit under a different license), as
>    indicated in the file; or
> 3. The contribution was provided directly to you by some other
>    person who certified (1), (2) or (3) and you have not
>    modified it.
> 4. You understand and agree that this project and the
>    contribution are public and that a record of the contribution
>    (including all personal information you submit with it,
>    including your sign-off) is maintained indefinitely and may
>    be redistributed consistent with this project or the
>    open-source license(s) involved.

### How to sign off

Append a `Signed-off-by:` line to every commit message. Use real
name (no pseudonyms) and a real reachable email address:

```
Signed-off-by: Jane Smith <jane@example.com>
```

The easy way is `git commit -s`, which adds the line automatically
based on `git config user.name` and `git config user.email`.

PRs without DCO sign-off on every commit will not be merged.

---

## Copyright

You retain copyright in your contributions. By submitting a change
under DCO sign-off, you license your contribution to the project
under LGPL-3.0-or-later (with the linking exception described in
`NOTICE`). The `AUTHORS` file lists the project's copyright holders;
significant contributors may be added on request.

This project does not require copyright assignment. The DCO is the
only attestation you need to make.

---

## Identity discipline

Do not commit using employer email addresses unless the employer
has explicitly acknowledged in writing that the contribution is
yours to submit under this project's license. When in doubt, use a
personal email and personal Git identity.

The maintainer enforces this on his own commits via repo-local
`git config` (`vanfosst@gmail.com`); contributors are responsible
for their own configuration.

---

## Quality gates

Before submitting a PR, run the full pre-commit suite locally:

```
.venv/bin/pre-commit run --all-files
```

This runs all 16 hooks, including the C/C++ complexity gates, the
doxygen-guard, the unit tests, and the per-library coverage check.
PRs that don't pass pre-commit will not be merged.

See `docs/contributing.md` for development setup, build
instructions, and architectural conventions.

---

## What gets accepted

- **Bug fixes** with a regression test — usually merged.
- **Small features** that align with the engine's scope (per
  `docs/roadmap.md`) — discussed first via an issue, then merged.
- **Large features** or architectural changes — open an issue to
  discuss before writing code. The architecture doc
  (`docs/architecture-cpp.md`) is the source of truth on direction.

---

## What does not get accepted

- Commits without DCO sign-off
- Code that fails pre-commit gates (do not disable hooks to "fix"
  this — refactor the code instead)
- Vendor / third-party code dropped into `src/` or `include/`
  (vendored deps go under `extern/`)
- Backwards-incompatible changes to the C ABI in `entropic.h`
  without a corresponding architecture-doc update and a major
  version bump
