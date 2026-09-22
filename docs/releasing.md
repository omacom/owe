# Releasing

One tag push builds, tests, publishes the GitHub release, and updates the AUR package.

## One-time setup

1. The AUR repository exists at `ssh://aur@aur.archlinux.org/owe.git`. The first push created it.
2. Add the AUR private key as the repository secret `AUR_SSH_PRIVATE_KEY`.
   The matching public key must be registered on the AUR account.
   Without the secret the release still publishes to GitHub and skips the AUR job.
3. Confirm the AUR package name is `owe`. No other package uses the name.

## Release steps

1. Update the version in `meson.build` and `qml-plugin/CMakeLists.txt`.
2. Commit and push the verified changes to `main`.
3. Tag the commit and push the tag:

   ```bash
   git tag -a vX.Y.Z -m vX.Y.Z
   git push origin vX.Y.Z
   ```

4. The release workflow runs:
   - builds and tests on Arch,
   - checks that the tag matches `meson.build`,
   - runs `meson dist` and attaches the archive and `SHA256SUMS` to the GitHub release,
    - rewrites the AUR `PKGBUILD` for the version and the tag archive checksum, then pushes it.

The tag archive checksum becomes available after the tag exists.
After publication, update `packaging/PKGBUILD` with that version and checksum in a separate commit.

To run the flow without a tag, dispatch it with the version:

```bash
gh workflow run release.yml -f version=X.Y.Z
```

The workflow creates the tag at the current `main` commit. Both paths require that the version matches `meson.build`.

## Dependencies

The AUR package declares three dependency groups:

- `depends` contains `mpv`, `ffmpeg`, `wayland`, `libglvnd`, `libepoxy`, `systemd-libs`, `socat`, and `qt6-declarative`.
  `mpv` supplies Mesa and the VAAPI libraries as dependencies.
- `makedepends` contains `meson`, `ninja`, `gcc`, `pkgconf`, `wayland-protocols`, and `cmake`.
- `checkdepends` is `python`, used by the daemon test.
- `optdepends` names the VAAPI drivers for hardware decode: `intel-media-driver` on Intel and `libva-mesa-driver` on AMD.

To audit the direct set again, read the `DT_NEEDED` entries and map them:

```bash
for b in owe owed owe-render; do readelf -d "$b" | awk '/NEEDED/ {print $NF}'; done | sort -u
```

## Rollback

The workflow never rewrites AUR history. It adds one commit per release. To correct a bad release, publish a new `pkgrel` or version.
