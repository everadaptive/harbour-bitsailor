# Autofill provider test setup

Test BitSailor as an autofill provider on the Sailfish emulator. The setup
uses a local Vaultwarden server with seed data. Use test data only.

## Components

- `start-vaultwarden.sh` runs Vaultwarden with a self-signed certificate on
  port 8222. The emulator reaches the host as `10.0.2.2`.
- `seed-server.sh` registers the account `autofill@example.test` and adds
  three logins (`register.py`, `seed.py`).
- `setup-emulator.sh <rpm>` installs the RPM, logs in, starts the provider,
  and points the autofill daemon at it.
- `run-e2e.sh` runs the QML client through the daemon and the provider.

## Seed logins

| Name | Username | URI |
|------|----------|-----|
| Demo app | alice | `app://autofill-demo` |
| Example.com - Alice | alice | `https://example.com` |
| Example.com - Bob | bob | `https://example.com` |

## Steps

1. Run `nix-shell -p vaultwarden openssl --run ./start-vaultwarden.sh`.
2. Run `./seed-server.sh`.
3. Build the RPM with `sfdk` (see `../../AGENTS.md`).
4. Start the daemon stack once with `autofill-demo/scripts/seed.sh`.
5. Run `./setup-emulator.sh <rpm>`.
6. Run `./run-e2e.sh`. Expect `E2E resolved: alice demo-app-pass-1`.

## Notes

- The emulator needs `sailfish-polkit-agent`,
  `sailfishsecretsdaemon-secretsplugins-default` and `libsailfishcrypto`.
  Install them with `pkcon install`.
- The secrets daemon identifies an application by its command line. Select
  the headless modes with `BITSAILOR_MODE`, not with an argument. An
  argument gives the process another identity, and it cannot read the
  secrets of the UI app.
- `BITSAILOR_AUTOFILL_ALLOWED_KEYIDS` limits the callers by rpm Key ID
  (comma separated). Without it, the provider accepts every signed caller.
- Manual fill is not implemented. The provider dismisses the prompt.
- The provider unlocks the vault with the stored user key. The user must
  log in once, in the UI or with `--autofill-dev-login`.
