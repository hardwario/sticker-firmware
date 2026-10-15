# TOWER frame KAT generator

Produces `../tower_frame_kat.json` from the upstream TOWER crates, so the STICKER node and the
Northbridge test their codec against the reference implementation.

```bash
git clone https://github.com/hardwario/tower /tmp/tower
git -C /tmp/tower checkout 24259e35b4607b1978e678153092bb1641d8d2a1
mkdir -p vendor && cp -r /tmp/tower/firmware/crates/tower-radio-core /tmp/tower/firmware/crates/tower-net-core vendor/
cargo run --release > ../tower_frame_kat.json
```

The crates are vendored rather than a git dependency because the tower repo has no root
`Cargo.toml`. The output was cross-checked with pycryptodome (AES-CCM, 8 B tag).
