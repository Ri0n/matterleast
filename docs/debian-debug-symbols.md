# Debian debug-symbol packages

Release builds for Ubuntu 22.04, 24.04, 26.04 and Debian 13 use
`RelWithDebInfo`. Each native DEB has a matching
`matterleast-<version>-<distro>-amd64-dbgsym.deb` asset.

`scripts/package-deb-debug.sh` takes the CPack DEB, extracts separate
symbols for `/usr/bin/matterleast` and bundled native plugin libraries,
adds GNU `.gnu_debuglink` references to the runtime ELF files, strips debug
sections from the normal executable, regenerates package checksums, and
rebuilds the normal DEB alongside the `matterleast-dbgsym` DEB.

Install the **runtime and dbgsym packages from the same release and target
distribution** together. The dbgsym package depends on the exact runtime
version. GDB automatically discovers symbols in
`/usr/lib/debug/usr/bin/matterleast.debug` and corresponding plugin paths.

Example:

```bash
sudo apt install ./matterleast-1.10-ubuntu-24.04-amd64.deb \
                 ./matterleast-1.10-ubuntu-24.04-amd64-dbgsym.deb
coredumpctl debug matterleast
```

Package verification runs in the Debian 13 CI job. Debug symbol extraction
is restricted to project-owned ELF binaries; Qt/system libraries still use
the distribution's own debuginfo packages.
