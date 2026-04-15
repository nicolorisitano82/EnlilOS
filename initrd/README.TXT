EnlilOS embedded initrd rootfs.
Questo mount `/` e' un archivio CPIO read-only caricato in RAM al boot.
Contiene i file di bootstrap, i binari ELF utente e l'entrypoint INIT.ELF.
La rootfs bootstrap include anche:
- /bin/arksh      -> login shell bridge v1
- /bin/nsh        -> recovery shell
- /NETD.ELF       -> server user-space rete bootstrap
- /SOCKDEMO.ELF   -> smoke BSD socket API v1 su loopback
- /usr/bin/loadkeys   -> cambio layout tastiera (`us`/`it`)
- /usr/bin/kbdlayout  -> query layout tastiera attivo
- /etc/arkshrc    -> rc globale bootstrap
- /etc/vconsole.conf -> configurazione tastiera bootstrap
- /home/user/.config/arksh/arkshrc -> rc utente fallback
- /usr/share/kbd/keymaps/us.map -> keymap US bootstrap
- /usr/share/kbd/keymaps/it.map -> keymap IT bootstrap
