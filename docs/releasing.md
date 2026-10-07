# Releasing

One tag push builds, tests, and publishes the GitHub source release. Arch packages are maintained in the [Omarchy Package Repository (OPR)](https://github.com/omacom/omarchy-pkgs/tree/master/pkgbuilds/owe).

## Release steps

1. Update the version in `meson.build` and `qml-plugin/CMakeLists.txt`.
2. Commit and push the verified changes to `main`.
3. Tag the commit and push the tag:

   ```bash
   git tag -a vX.Y.Z -m vX.Y.Z
   git push origin vX.Y.Z
   ```

4. The release workflow builds and tests on Arch, checks that the tag matches `meson.build`, and runs `meson dist` to attach the source archive and `SHA256SUMS` to the GitHub release.
5. OPR watches GitHub releases for version and checksum updates. Review and merge the OWE package update there; changes to dependencies, build steps, or installed files belong in its PKGBUILD. OPR builds, signs, and publishes the packages.

To run the flow without a tag, dispatch it with the version:

```bash
gh workflow run release.yml -f version=X.Y.Z
```

The workflow creates the tag at the current `main` commit. Both paths require that the version matches `meson.build`.

## Dependencies

OPR's PKGBUILD declares three dependency groups:

- `depends` contains `mpv`, `ffmpeg`, `wayland`, `libglvnd`, `libepoxy`, `systemd-libs`, `socat`, and `qt6-declarative`. `mpv` supplies Mesa and the VAAPI libraries as dependencies.
- `makedepends` contains `meson`, `ninja`, `gcc`, `pkgconf`, `wayland-protocols`, and `cmake`.
- `checkdepends` is `python`, used by the daemon test.
- `optdepends` names the VAAPI drivers for hardware decode: `intel-media-driver` on Intel and `libva-mesa-driver` on AMD.

To audit the direct set again, read the `DT_NEEDED` entries and map them:

```bash
for b in owe owed owe-render; do readelf -d "$b" | awk '/NEEDED/ {print $NF}'; done | sort -u
```

## Rollback

To correct a bad release, publish a new version or update OPR's package with a new `pkgrel`.
