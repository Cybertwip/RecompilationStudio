# PSXRecomp macOS signer

`tools/signer` applies the same isolated-keychain signing flow used by
PSXRecomp Studio to an existing `.app` bundle.

```sh
cd tools
./signer /path/to/package.app /path/to/certificate.pfx '<password>'
```

The app is modified **in place**. The utility:

- normalizes the PFX with OpenSSL 3 and a generated ASCII password;
- imports the identity into a temporary keychain that is deleted on exit;
- signs every nested Mach-O file, including Frameworks and all `MacOS`
  directory binaries, deepest first;
- signs nested app/XPC/plugin bundles inside-out after their Mach-O files;
- signs the main executable and outer app with Hardened Runtime and the same
  `disable-library-validation` entitlement as Studio;
- requests a secure timestamp and finishes with strict deep verification.

Requirements:

- macOS and Xcode command-line tools;
- OpenSSL 3 with `pkcs12 -legacy` support (`brew install openssl@3`);
- a PFX/P12 containing a usable code-signing identity.

The requested command-line form exposes the password to the local process list
and may store it in shell history. Quote passwords containing shell characters
and clear or disable history when appropriate.

For a disposable offline smoke test only,
`PSXRECOMP_SIGNER_NO_TIMESTAMP=1` skips the timestamp request and
`PSXRECOMP_SIGNER_ADHOC_TEST=1` uses ad-hoc signing if the generated test PFX is
not a trusted production identity. Production signing should use neither
override.

Verification evidence is recorded in `docs/macos_signer_proof.json`.
