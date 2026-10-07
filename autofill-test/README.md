# Autofill provider test setup

Test BitSailor as an autofill provider on the Sailfish emulator. The setup
uses a local Vaultwarden server with seed data. Use test data only.

## Components

- `start-vaultwarden.sh` runs Vaultwarden with a self-signed certificate on
  port 8222. The emulator reaches the host as `10.0.2.2`.
- `seed-server.sh` registers the account `autofill@example.test` and adds
  six logins (`register.py`, `seed.py`).
- `setup-emulator.sh <rpm>` installs the RPM, logs in, starts the provider,
  and points the autofill daemon at it.
- `run-e2e.sh` runs the QML client through the daemon and the provider.

## Seed logins

| Name | Username | URI |
|------|----------|-----|
| Demo app | alice (email field: alice@example.com) | `app://autofill-demo` |
| Example.com - Alice | alice (email field: alice@example.com) | `https://example.com` |
| Example.com - Bob | bob | `https://example.com` |
| GitHub | octocat-dev | `https://github.com` |
| Fakesite.com - Charlie | charlie@fakesite.com (email-shaped, no email field) | `https://fakesite.com` |
| Other app | carol | `app://org.example.otherapp` |

Alice has a custom field named `email`. The provider returns it as `email`. The last three match no demo field. They show only in the "Search all logins…"
list.

## Steps

1. Run `nix-shell -p vaultwarden openssl --run ./start-vaultwarden.sh`.
2. Run `./seed-server.sh`.
3. Build the RPM with `sfdk` (see `../../AGENTS.md`).
4. Start the daemon stack once with `autofill-demo/scripts/seed.sh`.
5. Run `./setup-emulator.sh <rpm>`.
6. Run `./run-e2e.sh`. Expect `E2E resolved: alice demo-app-pass-1 alice@example.com`.

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
- The provider also supports saving and generating (protocol v2). The save
  prompt needs the UI app. For a test without the UI, start the dev picker
  with `BITSAILOR_MODE=autofill-dev-picker BITSAILOR_DEV_SAVE_ANSWER=save`
  (or `decline`, `never`). `autofill-qml/tests/run-device-e2e.sh
  bitsailor` does this and runs a full save, update, and fill sequence.
- The provider unlocks the vault with the stored user key. The user must
  log in once, in the UI or with `--autofill-dev-login`.

## Host tests (`host/`)

The tests in `host/` need no emulator, no Go core library, and no Sailfish
secrets stack. They build the real autofill provider code
(`src/autofill/autofillprovider.cpp` and its helpers) against a stub of
`BitSailorCore` (`host/stub/bitsailorcore.h`) with Qt 6. The stub keeps the vault in memory.

- `autofillsave_test`: unit test of `src/autofill/autofillsave.cpp` (origin
  rules, item JSON for create and update, password policy).
- `provider_v2.py`: integration test of `SaveCredentials` and
  `GeneratePassword`. It starts the provider and a fake UI app. The fake UI
  app runs the real `AutofillPickerService` and answers a save prompt like a
  user. A Python client calls the provider.

Run both with `host/run-host-tests.sh`. The tests do not cover the QML
pages, the Go core, or the Qt 5.6 build of the target. Check those on the
emulator (`autofill-test/e2e*.sh`).
