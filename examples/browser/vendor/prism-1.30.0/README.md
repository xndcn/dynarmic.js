# Prism browser dependency

Prism **1.30.0**, MIT license. These are unchanged upstream source files from
<https://registry.npmjs.org/prismjs/-/prismjs-1.30.0.tgz>.
Only the core, C-like base, C and JavaScript grammars are retained.

`site.js` loads the core and the required grammar when a source viewer opens.
The files are served locally from this versioned directory. No CDN, npm install,
language auto-detection or build-time highlighting is used.

To update, download the exact npm tarball, verify its registry integrity, then
copy the same files from `package/`. Retain the license and refresh this inventory.

Tarball integrity:

```text
sha512-DEvV2ZF2r2/63V+tK8hQvrR2ZGn10srHbXviTlcv7Kpzw8jWiNTqbVgjO3IY8RxrrOUF8VPMQQFysYYYv0YZxw==
```

Original paths and SHA-256 digests:

```json
{
  "components/prism-core.js": "6fe39cc3a95b3da596218fbca3acf03f1208bc936f9fb84d08a2b4a61e2b9af7",
  "components/prism-clike.js": "6a7e077bbcb4a2259bc3152e55681f804ec8a3c2b969f234bb220df0f2d79654",
  "components/prism-c.js": "326fe0f50c2f00c5c9cf741b693654e215017b5d16a86c1c7938455cc64b340b",
  "components/prism-javascript.js": "7be3e7caf699bfddcd6d94d01e9df233257477f7f521e6c6765f03bd936a2d3e",
  "LICENSE": "2b947f0901a7ffcf08a89957da9783c0e9c6e72cb6ce8e959f501ab5409e4d2b"
}
```
