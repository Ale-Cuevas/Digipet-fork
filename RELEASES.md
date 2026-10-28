# Signed firmware releases

Digipet releases use an application-level ECDSA P-256 trust chain. GitHub
Actions builds the firmware, creates a manifest containing the firmware SHA-256
digest and size, signs the exact manifest bytes, verifies the signature, and
publishes all assets on a tagged GitHub release.

This is the trust mechanism intended for the future self-updater. HTTPS and the
manifest checksum detect ordinary corruption; the ECDSA signature proves that
the manifest was authorised by the Digipet release key.

## Release assets

Every `v*` release contains:

- `digipet-firmware.bin`: application image for OTA
- `digipet-factory.bin`: combined image for wired recovery/first install
- `digipet-manifest.json`: canonical update metadata and image hashes
- `digipet-manifest.sig`: ECDSA P-256/SHA-256 signature of the manifest

The public verification key is committed at
`release/firmware-signing-public.pem` and duplicated as `kPublicKey` in
`src/ota_updater.cpp`; the build workflow fails if the two differ. The private
key lives outside the repository at `~/.config/digipet/firmware-signing-key.pem`
and must never enter the repository, workflow logs, firmware, release assets,
or an SD card.

The device downloads updates from the GitHub repository named by the
`DIGIPET_OTA_REPO` build flag in `platformio.ini` (default
`Ale-Cuevas/Digipet-fork`). That repository's release key must match the
public key compiled into the firmware.

## Configure the GitHub secret

The release workflow expects `FIRMWARE_SIGNING_KEY_B64`, containing the base64
encoding of the complete PEM private key as one line:

```bash
base64 -w 0 ~/.config/digipet/firmware-signing-key.pem | \
  gh secret set FIRMWARE_SIGNING_KEY_B64 --repo Ale-Cuevas/Digipet-fork
```

On macOS, use `base64 < key.pem | tr -d '\n'` instead of `base64 -w 0`.

Restrict access to the private key and keep an encrypted offline backup. Losing
it prevents publishing updates trusted by already-installed firmware. If it is
exposed, devices need a firmware release containing a replacement trusted key;
key rotation support should therefore be added before broad deployment.

## Publish a release

After pushing `main`, create and push a signed or annotated version tag:

```bash
git tag -a v0.1.0 -m "Digipet v0.1.0"
git push origin main v0.1.0
```

The tag triggers `.github/workflows/release.yml`. The workflow refuses to
publish if the signing secret is missing or if verification with the committed
public key fails. Maintainers with a configured Git signing identity may use
`git tag -s` instead; Git tag signing and firmware-manifest signing are separate.

## OTA client rules

The device-side updater follows these rules:

1. Run only after an explicit update check or configured maintenance event.
2. Download `digipet-manifest.json` and `digipet-manifest.sig` over HTTPS.
3. Verify the signature against the public key compiled into the firmware.
4. Validate `schema`, `product`, `target`, and a strictly newer semantic version.
5. Stream the application image into the inactive OTA partition.
6. Verify its exact byte count and SHA-256 digest before selecting it to boot.

The current client implements steps 1-6. Automatic post-boot health validation
and bootloader rollback are planned production hardening; until then, retain USB
recovery access when testing a newly published firmware image.

Never install an image based only on a URL, filename, GitHub account name, or
unsigned checksum. The updater must fail closed while normal offline Digipet
operation continues.

## Secure Boot and flash encryption

Application-level signing protects the update channel without changing eFuses.
ESP32-S3 Secure Boot V2 and flash encryption offer stronger local protection,
but provisioning them is a separate production process and can irreversibly
lock hardware. Do not enable those eFuses on development devices until wired
recovery, OTA rollback, key custody, and release signing have all been tested.
