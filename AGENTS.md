# Agent instructions

## This fork stands on its own

`bee-san/vita-jp-overlay` is bee-san's personal software, forked from
[Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay). It is not
maintained as a contribution to that project.

- Upstream is pull-only. Its changes may be merged into this fork, never the
  other way round.
- Never open pull requests, issues or discussions on `Dartv/vita-jp-overlay`,
  never comment there, and never push branches or tags to it.
- All work lands in `bee-san/vita-jp-overlay`, through pull requests against
  its `main`.
- In a fork, `gh` defaults to the upstream parent. Always pass
  `-R bee-san/vita-jp-overlay`, for example
  `gh pr create -R bee-san/vita-jp-overlay --base main`.

To pull in upstream changes, merge `upstream/main` into a branch of this fork,
keep this fork's behaviour wherever the two differ, and open a pull request
against this fork's `main`:

```sh
git remote add upstream https://github.com/Dartv/vita-jp-overlay.git  # once
git remote set-url --push upstream DISABLED                            # fetch only
git fetch upstream
git switch -c sync/upstream origin/main
git merge upstream/main
```

Build and test instructions are in [README.md](README.md#building).
