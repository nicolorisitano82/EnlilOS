EnlilOS embedded initrd rootfs.
Questo mount `/` e' un archivio CPIO read-only caricato in RAM al boot.
Contiene i file di bootstrap, i binari ELF utente e l'entrypoint INIT.ELF.
La rootfs bootstrap include anche:
- /bin/arksh      -> login shell bridge v1
- /bin/nsh        -> recovery shell
- /etc/arkshrc    -> rc globale bootstrap
- /home/user/.config/arksh/arkshrc -> rc utente fallback
