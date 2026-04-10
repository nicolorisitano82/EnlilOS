# Arksh Compatibility Layer for EnlilOS

Questa directory contiene il materiale `v1` per il porting di `arksh` su EnlilOS.

Il repo `arksh` non e' vendorizzato dentro EnlilOS, quindi qui teniamo:

- il file toolchain CMake [tools/enlilos-aarch64.cmake](/Users/nicolo/nros/tools/enlilos-aarch64.cmake)
- il target host-side `make arksh-configure` / `make arksh-build`
- uno shim di riferimento [enlilos.c](/Users/nicolo/nros/compat/arksh/enlilos.c) da contribuire
  o adattare upstream

Uso previsto:

```bash
make arksh-configure ARKSH_DIR=/percorso/arksh
make arksh-build ARKSH_DIR=/percorso/arksh
```

Oppure, per validare solo la toolchain EnlilOS lato CMake senza sorgenti `arksh`:

```bash
make arksh-smoke
```

Lo smoke ELF risultante viene anche inserito nell'`initrd` come `ARKSHSMK.ELF`
e validato dal selftest runtime.
