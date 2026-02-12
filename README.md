# nft-tui — un “htop” per nftables

Frontend **TUI/CLI** (ncurses) per gestire e monitorare `nftables` in stile “htop”.

## Obiettivo
- 💻 Interamente CLI
- 🛡 Sicuro (read-only, input validati)
- ⚡ Leggero (no web, no demoni)
- 🚀 Professionale (monitor realtime, autodetect set, plugin)

## Funzioni
- Monitor (refresh) con info base
- Rilevamento automatico set IPv4/IPv6 da `nft list ruleset`
- Gestione set: view / add / del / flush
- Check/apply config: `/etc/nftables.conf`
- Log kernel (journalctl -k)
- Plugin: esegue script in `plugins/`

## Build
```bash
sudo apt install -y build-essential libncurses-dev nftables
make
