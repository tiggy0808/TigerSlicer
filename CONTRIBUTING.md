# Contributing to TigerSlicer

Thanks for your interest! TigerSlicer is a one-person project, so replies and reviews can be slow.

## Bugs and ideas

- Check the [latest release](https://github.com/tiggy0808/TigerSlicer/releases/latest) and the
  [changelog](CHANGELOG.md) (including its known issues) first.
- [Open an issue](https://github.com/tiggy0808/TigerSlicer/issues/new/choose) with the TigerSlicer version,
  your printer, what you expected and what happened. A project file (`.3mf`) helps a lot.

## Pull requests

- For anything bigger than a small fix, open an issue first so we can agree on the approach.
- Build and test with [docs/BUILDING.md](docs/BUILDING.md). Tests live in `tests/`; their conventions are in
  [tests/AGENTS.md](tests/AGENTS.md).
- Format C++ with the repository's `.clang-format`, and write commit messages in the
  [Conventional Commits](https://www.conventionalcommits.org/) style (`feat:`, `fix:`, `docs:` …).
- Add a line to the `Unreleased` section of [CHANGELOG.md](CHANGELOG.md) for anything users will notice.
- Keep the internal OrcaSlicer identifiers (`ORCA_*`, `OrcaFilamentLibrary`, the 3MF `OrcaSlicer`
  metadata key) as they are: profiles, project files and plugins depend on them.
- A fix that isn't specific to TigerSlicer most likely belongs in
  [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) too.

## License

TigerSlicer is licensed under the [GNU Affero General Public License v3](LICENSE.txt). By contributing you
agree that your contribution is licensed under the same terms.

## Code of conduct

Everyone taking part is expected to follow the [code of conduct](CODE_OF_CONDUCT.md).
