# Web installer

Lives under the site's `/install/` path and is published by the repo's single
GitHub Pages deployment (`.github/workflows/pages.yml`). Flashes the board and
provisions Wi-Fi via ESP Web Tools + Improv-serial. No toolchain needed by the
end user (Chrome/Edge desktop only — Web Serial).

## Local test

Web Serial needs a secure context; `http://localhost` counts. From `site/`:

    python -m http.server 8000

Open http://localhost:8000/install/ in Chrome/Edge. Drop a locally-built
`ytm-firmware.bin` (see below) next to `manifest.json` to test flashing.

## Building the firmware binary

CI does this in the Pages workflow. Manually:

    cd firmware && idf.py build
    idf.py merge-bin -o ../site/install/ytm-firmware.bin

`ytm-firmware.bin` is a single image flashed at offset 0 (bootloader +
partition table + app). It is git-ignored; CI produces it on deploy.

## Vendored ESP Web Tools bundle

`install-button.js` and its sibling chunk files (`install-dialog-*.js`,
`index-*.js`, `styles-*.js`, the per-chip `esp32*.js` files, and the
`stub_flasher_*.js` files) are the full `dist/web/` output of
`esp-web-tools@10` on unpkg, vendored so the page has no runtime CDN
dependency. The bundle code-splits itself via relative dynamic `import()`
calls, so all of these files must stay together, unmodified, and named
exactly as downloaded — deleting or renaming any of them breaks the
install flow. Re-vendor by fetching every file listed at
`https://unpkg.com/esp-web-tools@10/dist/web/?meta` into this directory.
