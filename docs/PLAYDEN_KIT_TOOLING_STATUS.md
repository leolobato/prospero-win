# Playden kit tooling status

This is local development tooling. It is not a release kit, final dependency pin,
or hardware acceptance. It is not connected to Playden's production installation
path. A release must close these gates before the lock owner pins its manifest.

The tools prepare a fresh prefix with the repaired 32-bit audio/input registrations
and validate structural module, registry, architecture, path, permission and
per-title helper contracts. Preparation and packaging share exact Volatile
Environment USERNAME/USERPROFILE validation. Packaging scans both text and binary
payloads for host paths (including UTF-16); it rejects missing runtime PRX execute
bits and duplicate firmware entries instead of repairing those inputs.

Required release follow-up:

- Independently bind complete retained runtime inventory to the reviewed artifact.
- Bind initialization's prefix manifest and source/loader provenance to packaging;
  require the reviewed source-binding digest and actual source/patch identities.
- Verify DXVK DLLs against the pinned archive or independently reviewed digests
  derived from it; input spec hashes alone do not establish DXVK identity.
- Include dependency license artifacts and their source-bound hashes.
- Rebuild binaries without absolute builder paths; existing development binaries
  fail the now-enforced binary scan.
- Run an emitted-manifest integration check through Playden's real SHA-256 admission.
- Test a fresh private bottle through PS5 Home/Playden, game exit and durable session
  replacement on the target console; independently exercise native launch glue.

The retained Wine artifact e16ec8f and actual corresponding Wine source/patches
are different identities and must remain separately recorded. A caller-created
inventory or nonempty self-asserted source-binding string is not independent proof.

Current focused checks: 29 kit tests, 12 preparation tests, title identity, native
contract, release package layout, portable title parser/session and data mount
checks pass. The full x86 Linux suite remains unverified under the available ARM
host's Docker x86 emulation (original MAP32 host-execution assertion). None of the
focused checks substitutes for full release or hardware acceptance.
