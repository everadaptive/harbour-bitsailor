// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#ifndef AUTOFILLMODE_H
#define AUTOFILLMODE_H

// Command line modes of the harbour-bitsailor binary. Both modes run
// without a UI.

// --autofill-provider: export tech.everadaptive.Autofill1.Provider on the
// session bus.
int runAutofillProvider(int argc, char *argv[]);

// --autofill-dev-login: log in with a password and store the unlock key,
// so that the provider can unlock the vault without a UI. For test
// setups only. The credentials come from the environment:
// BITSAILOR_DEV_SERVER, BITSAILOR_DEV_EMAIL, BITSAILOR_DEV_PASSWORD.
// BITSAILOR_DEV_INSECURE=1 accepts a self-signed server certificate.
// BITSAILOR_DEV_RESET=1 deletes the secrets collection and exits.
int runAutofillDevLogin(int argc, char *argv[]);

// --autofill-dev-picker: stand in for the UI app in tests. The process owns
// the UI bus name, receives the picker request through the real
// AutofillPickerService, and selects the item in BITSAILOR_DEV_PICK_ID.
int runAutofillDevPicker(int argc, char *argv[]);

#endif // AUTOFILLMODE_H
