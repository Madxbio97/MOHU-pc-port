# Release checklist

1. Confirm `git status` contains only intended changes.
2. Confirm no BIN/CUE, save, log, dump, screenshot, build output or credential is
   tracked.
3. Build with `windows-psycross-release`.
4. Run `ctest --preset windows-psycross-release`.
5. Run manual gameplay, visual, audio and controller checks.
6. Update `CHANGELOG.md` and tag the tested commit.
7. Package the executable, required runtime DLLs, locales, licenses and user
   documentation.
8. Verify the package on a clean Windows user profile.

The release archive must never contain proprietary game data or user saves.
