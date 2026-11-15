# Contributing

Four rules are absolute. Hooks enforce them; enable them once per clone:

```sh
git config core.hooksPath tools/hooks
```

## 1. No copyrighted material, ever

Not in files, not in commit messages, not in issues or pull requests.

| Never in the repository | Fine |
|---|---|
| Anything from `orig/`: executables, DLLs, archives, movies, music, maps, manuals | SHA-256 hashes, file names and sizes |
| Generated (lifted) C, or any binary built from it | The tools that generate it |
| Disassembly listings, decompiled function bodies | Addresses, counts, names we invented, our own description of behaviour |
| Screenshots, recordings, extracted art, sound or text | Synthetic test fixtures we wrote and built ourselves |
| Code copied from Wine, ReactOS, SDKs with restrictive licences, or projects without a licence | Code written from public documentation and observation |

If you are not sure whether something qualifies, it does not go in.

## 2. Commit messages name no AI model, vendor or session

Write commit messages as plain technical descriptions of the change. No
attribution trailers for tools.

## 3. No personal data

Not in files and not in commit messages: no real names, no private e-mail
addresses, no user names, no machine names, and no paths from anyone's
machine (`/home/<user>/…`, `/Users/<user>/…`, `C:\Users\<user>\…`).

- Write paths relative to the repository root (`orig/t1/app`), or with a
  placeholder (`<game-dir>`).
- Tools must not write absolute paths, user names or host names into files
  that get committed, and generated reports must use relative paths.
- Paste logs and tool output only after removing such details.
- Commit under a public handle with a no-reply address.

## 4. Every source file carries an SPDX identifier

Within the first five lines, in that language's comment syntax:

```
// SPDX-License-Identifier: MIT        (Rust, C, JavaScript)
# SPDX-License-Identifier: MIT         (shell, Python, CMake, TOML)
```

This covers everything we write that is code or build configuration,
including scripts, hooks, build files and the C headers of the runtime.
Documentation in Markdown does not need it. Code taken from another project
keeps that project's identifier and notice, and is only taken if its licence
is compatible with MIT.

Lifted code is the exception in the other direction: it is derived from the
game, is not ours to license, and so gets no MIT identifier. It is never
committed anyway.

## Everything else

See [docs/WORKFLOWS.md](docs/WORKFLOWS.md) for how work is done and
[docs/PLAN.md](docs/PLAN.md) for what is being done.
