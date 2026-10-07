#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT
#
# Integration test of the v2 methods of the BitSailor autofill provider.
# The provider runs on a stub core (stub/bitsailorcore.h). The real
# AutofillPickerService runs in a fake UI app (ui_host). This client calls
# the provider directly, without the daemon. Run it through run-host-tests.sh.

import glob
import os
import subprocess
import sys
import tempfile

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = "cz.chrastecky.bitsailor.Autofill"
PATH = "/tech/everadaptive/Autofill1/Provider"
IFACE = "tech.everadaptive.Autofill1.Provider"
ERR = "tech.everadaptive.Autofill1.Error."
KEY_ID = "abc123"
SITE = "https://example.com"

failures = []


def check(condition, what):
    print(("ok   " if condition else "FAIL ") + what, flush=True)
    if not condition:
        failures.append(what)


def sv(value):
    if isinstance(value, bool):
        return GLib.Variant("b", value)
    if isinstance(value, int):
        return GLib.Variant("u", value)
    return GLib.Variant("s", value)


def vmap(values):
    return {key: sv(value) for key, value in values.items()}


def pump(predicate, timeout_ms=4000):
    context = GLib.MainContext.default()
    end = GLib.get_monotonic_time() + timeout_ms * 1000
    while not predicate() and GLib.get_monotonic_time() < end:
        context.iteration(False)
        GLib.usleep(5000)
    return predicate()


def error_name(func):
    try:
        func()
    except GLib.Error as error:
        return Gio.DBusError.get_remote_error(error)
    return None


class Host:
    """A provider process, a fake UI process, and a client connection."""

    def __init__(self, bin_dir, env=None, ui=True, answer="save"):
        self.work = tempfile.mkdtemp()
        self.answer_file = os.path.join(self.work, "answer")
        self.ui_log = os.path.join(self.work, "ui.log")
        self.set_answer(answer)
        base = dict(os.environ, XDG_CONFIG_HOME=os.path.join(self.work, "config"),
                    UI_ANSWER_FILE=self.answer_file, UI_LOG=self.ui_log)
        base.update(env or {})
        self.bin_dir = bin_dir
        self.env = base
        self.ui = subprocess.Popen([os.path.join(bin_dir, "ui_host")], env=base) if ui else None
        self.provider = subprocess.Popen([os.path.join(bin_dir, "provider_host")], env=base, stderr=subprocess.DEVNULL)
        self._seq = 0
        self.conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        if not pump(lambda: self.has_owner(BUS_NAME) and (not ui or self.has_owner("cz.chrastecky.bitsailor")), 6000):
            sys.exit("the provider or the fake UI did not start")
        self.completed = []
        self.changed = []
        self.conn.signal_subscribe(None, "tech.everadaptive.Autofill1.Prompt", "Completed", None, None,
                                   Gio.DBusSignalFlags.NONE, self._on_completed)
        self.conn.signal_subscribe(None, IFACE, "CredentialsChanged", PATH, None,
                                   Gio.DBusSignalFlags.NONE, lambda *a: self.changed.append(1))

    def set_answer(self, answer):
        with open(self.answer_file, "w") as handle:
            handle.write(answer + "\n")

    def ui_lines(self):
        if not os.path.exists(self.ui_log):
            return []
        with open(self.ui_log) as handle:
            return [line.rstrip("\n").split("|") for line in handle if line.strip()]

    def has_owner(self, name):
        reply = self.conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                    "NameHasOwner", GLib.Variant("(s)", (name,)), GLib.VariantType("(b)"),
                                    Gio.DBusCallFlags.NONE, 2000, None)
        return reply.unpack()[0]

    def _on_completed(self, conn, sender, path, iface, name, params):
        dismissed, result = params.unpack()
        self.completed.append((path, dismissed, result))

    def stop(self):
        for proc in (self.provider, self.ui):
            if proc:
                proc.terminate()
                proc.wait()

    def call(self, method, args, reply_type, timeout=8000):
        return self.conn.call_sync(BUS_NAME, PATH, IFACE, method, args, GLib.VariantType(reply_type),
                                   Gio.DBusCallFlags.NONE, timeout, None).unpack()

    def request(self, origin, app="app-x", session="s1", signature=KEY_ID, **extra):
        values = {"origin": origin, "autofill-session-id": session, "app-id": app, "app-signature": signature}
        values.update(extra)
        return vmap(values)

    def candidates(self, origin=SITE, **kw):
        (reply,) = self.call("GetCandidates", GLib.Variant("(a{sv})", (self.request(origin, **kw),)), "((a(sa{sv})a{sv}))")
        return reply[0]

    def wait_ready(self):
        def ready():
            try:
                self.candidates()
                return True
            except GLib.Error:
                return False
        return pump(ready, 6000)

    def usernames(self, origin=SITE, **kw):
        return sorted(meta["username"] for _, meta in self.candidates(origin, **kw))

    def resolve(self, token):
        (credentials,) = self.call("Resolve", GLib.Variant("(s)", (token,)), "(a{sv})")
        return credentials

    def save_call(self, origin, username, password, app=None, session="s1", **extra):
        # A new app id for each call, so that the rate limit (5 a minute for
        # each app) does not stop the test. The rate test sets the app id.
        if app is None:
            self._seq += 1
            app = "app-auto-%d" % self._seq
        creds = {"password": password} if password else {}
        if username:
            creds["username"] = username
        creds.update(extra.pop("creds", {}))
        (path,) = self.call("SaveCredentials", GLib.Variant("(a{sv}a{sv})", (
            self.request(origin, app=app, session=session, **extra), vmap(creds))), "(o)")
        return path

    def save(self, origin, username, password, wait=True, **extra):
        before = len(self.completed)
        path = self.save_call(origin, username, password, **extra)
        if wait:
            pump(lambda: any(c[0] == path for c in self.completed[before:]), 6000)
        done = [c for c in self.completed[before:] if c[0] == path]
        return path, (done[-1][1:] if done else None)

    def generate(self, policy, app="app-g", **kw):
        (result,) = self.call("GeneratePassword", GLib.Variant("(a{sv}a{sv})", (
            self.request(SITE, app=app, **kw), vmap(policy))), "(a{sv})")
        return result["password"]

    def never_list(self):
        for path in glob.glob(os.path.join(self.work, "config", "**", "settings_v2.ini"), recursive=True):
            with open(path) as handle:
                return handle.read()
        return ""


def outcome(done):
    return done[1].get("outcome") if done else None


def run(bin_dir):
    # ----- Group A: unlocked vault, the user accepts
    h = Host(bin_dir)
    try:
        check(h.wait_ready(), "provider is ready")
        version = h.conn.call_sync(BUS_NAME, PATH, "org.freedesktop.DBus.Properties", "Get",
                                   GLib.Variant("(ss)", (IFACE, "Version")), GLib.VariantType("(v)"),
                                   Gio.DBusCallFlags.NONE, 2000, None).unpack()[0]
        check(version == 2, "Version is 2")
        check(h.usernames() == ["alice@example.com"], "the vault starts with one login")

        # Create. The email goes into the custom field. Resolve returns it.
        path, done = h.save(SITE + "/signup", "bob", "pw-bob", creds={"email": "bob@example.net"})
        check(done is not None and done[0] is False and outcome(done) == "saved", "new login: saved, dismissed=false")
        check(path.startswith("/tech/everadaptive/Autofill1/prompt/"), "the prompt path is a provider prompt path")
        ui = h.ui_lines()
        check(len(ui) == 1 and ui[0][:2] == ["create", "https://example.com"] and ui[0][2].startswith("app-auto-")
              and ui[0][3:5] == ["bob", "example.com"],
              "the UI got: action, origin, app, username, item name")
        check("pw-bob" not in "|".join("|".join(line) for line in ui), "the UI request holds no password")
        check(pump(lambda: len(h.changed) >= 1), "CredentialsChanged after a create")
        check(h.usernames() == ["alice@example.com", "bob"], "the new login is a candidate")
        bob = [t for t, m in h.candidates() if m["username"] == "bob"][0]
        creds = h.resolve(bob)
        check(creds["password"] == "pw-bob" and creds.get("email") == "bob@example.net",
              "Resolve returns the saved password and the email custom field")

        # Update by origin and username.
        before_ui = len(h.ui_lines())
        _, done = h.save(SITE, "alice@example.com", "alice-new")
        check(outcome(done) == "updated", "same username, new password: updated")
        check(h.ui_lines()[before_ui][0] == "update" and h.ui_lines()[before_ui][4] == "Example", "the UI got action update and the item name")
        alice = [t for t, m in h.candidates() if m["username"] == "alice@example.com"][0]
        check(h.resolve(alice)["password"] == "alice-new", "the update changes the stored password")
        check(len(h.candidates()) == 2, "an update adds no item")

        # Unchanged: no UI, immediate completion.
        before_ui = len(h.ui_lines())
        _, done = h.save(SITE, "alice@example.com", "alice-new")
        check(outcome(done) == "unchanged" and done[0] is False, "same credentials: unchanged")
        check(len(h.ui_lines()) == before_ui, "unchanged does not ask the UI")

        # filled-token: update of the filled item, with a new username.
        alice = [t for t, m in h.candidates(session="s2") if m["username"] == "alice@example.com"][0]
        h.call("Resolve", GLib.Variant("(s)", (alice,)), "(a{sv})")
        _, done = h.save(SITE, "alice2@example.com", "alice-third", session="s2", **{"filled-token": alice})
        check(outcome(done) == "updated", "filled-token: update of the filled item")
        check(h.usernames(session="s3") == ["alice2@example.com", "bob"], "filled-token update renames the item, no new item")
        _, done = h.save(SITE, "alice3@example.com", "x", session="s9", **{"filled-token": alice})
        check(outcome(done) == "saved", "filled-token of another session is ignored (create)")

        # Two saves at once. The writes run one after the other.
        before = len(h.completed)
        p1 = h.save_call(SITE, "carol", "pw-c", app="app-two")
        p2 = h.save_call(SITE, "dave", "pw-d", app="app-two", session="s4")
        check(pump(lambda: {c[0] for c in h.completed[before:]} >= {p1, p2}, 8000), "two concurrent saves both complete")
        names = h.usernames(session="s5")
        check("carol" in names and "dave" in names, "both concurrent saves are in the vault")

        # Rejections.
        n = len(h.completed)
        check(error_name(lambda: h.save_call(SITE, "eve", "x", signature="(none)")) == ERR + "OriginMismatch", "unsigned caller: OriginMismatch")
        check(error_name(lambda: h.save_call("ftp://example.com", "eve", "x")) == ERR + "OriginMismatch", "origin that cannot be saved: OriginMismatch")
        check(error_name(lambda: h.save_call("https://user:pw@example.com", "eve", "x")) == ERR + "OriginMismatch", "origin with user info: OriginMismatch")
        check(error_name(lambda: h.save_call(SITE, "eve", "")) == ERR + "InvalidArgs", "empty password: InvalidArgs")
        check(error_name(lambda: h.save_call("", "eve", "x")) == ERR + "InvalidArgs", "empty origin: InvalidArgs")
        check("eve" not in h.usernames(session="s6"), "a rejected save changes nothing")
        check(len(h.completed) == n, "a rejected save sends no Completed signal")

        # An app origin.
        _, done = h.save("app://org.example.app", "frank", "pw-f", app="org.example.app")
        check(outcome(done) == "saved" and h.usernames("app://org.example.app", session="s7") == ["frank"], "app origin: saved and offered on the app origin")
        check("frank" not in h.usernames(SITE, session="s8"), "an app login is not offered on a web origin")

        # Username empty: the email is the username.
        _, done = h.save("https://mail.example.org", "", "pw-m", creds={"email": "m@example.org"})
        check(outcome(done) == "saved" and h.usernames("https://mail.example.org", session="s10") == ["m@example.org"], "no username: the email is the username")
    finally:
        h.stop()

    # ----- Group B: the user declines, then chooses never
    h = Host(bin_dir, answer="decline")
    try:
        h.wait_ready()
        _, done = h.save(SITE, "gina", "pw-g", app="app-b")
        check(done and done[0] is True and outcome(done) == "declined", "decline: dismissed=true, outcome declined")
        check(h.usernames() == ["alice@example.com"], "decline saves nothing")
        h.set_answer("never")
        _, done = h.save(SITE, "gina", "pw-g", app="app-b")
        check(done and done[0] is True and outcome(done) == "never", "never: dismissed=true, outcome never")
        check("https://example.com" in h.never_list(), "the never list is stored in the settings file")
        before_ui = len(h.ui_lines())
        _, done = h.save(SITE + "/other", "gina", "pw-g", app="app-b")
        check(outcome(done) == "never", "origin on the never list: never")
        check(len(h.ui_lines()) == before_ui, "the never list stops the UI prompt")
        h.set_answer("save")
        _, done = h.save("https://other.example", "gina", "pw-g", app="app-b")
        check(outcome(done) == "saved", "another origin is not on the never list")
    finally:
        h.stop()

    # ----- Group C: the vault write fails
    h = Host(bin_dir, env={"STUB_WRITE_FAIL": "1"})
    try:
        h.wait_ready()
        _, done = h.save(SITE, "hank", "pw-h", app="app-c")
        check(done and done[0] is False and outcome(done) == "failed", "create write fails: outcome failed, dismissed=false")
        _, done = h.save(SITE, "alice@example.com", "alice-x", app="app-c")
        check(outcome(done) == "failed", "update write fails: outcome failed")
        check(h.usernames() == ["alice@example.com"], "a failed write changes nothing")
        check(len(h.changed) == 0, "no CredentialsChanged after a failed write")
    finally:
        h.stop()

    # ----- Group D: the vault is locked when the save comes
    h = Host(bin_dir, env={"STUB_LOCKED": "1", "STUB_UNLOCK_DELAY_MS": "800"})
    try:
        locked = error_name(lambda: h.candidates())
        check(locked == ERR + "VaultLocked", "the vault starts locked")
        _, done = h.save(SITE, "ivy", "pw-i", app="app-d")
        check(outcome(done) == "saved", "save while locked: the provider unlocks and saves")
        check("ivy" in h.usernames(session="s2"), "the login saved after the unlock is a candidate")
    finally:
        h.stop()
    h = Host(bin_dir, env={"STUB_LOCKED": "1", "STUB_UNLOCK_FAIL": "1", "STUB_UNLOCK_DELAY_MS": "300"})
    try:
        _, done = h.save(SITE, "jack", "pw-j", app="app-d")
        check(done and done[0] is True and outcome(done) == "declined", "save when the unlock fails: declined, dismissed=true")
        check(len(h.ui_lines()) == 0, "no UI prompt when the unlock fails")
    finally:
        h.stop()

    # ----- Group E: no UI app
    h = Host(bin_dir, ui=False)
    try:
        h.wait_ready()
        _, done = h.save(SITE, "kim", "pw-k", app="app-e")
        check(done and done[0] is True and outcome(done) == "declined", "no UI app: declined, dismissed=true")
        check(h.usernames() == ["alice@example.com"], "no UI app: nothing saved")
    finally:
        h.stop()

    # ----- Group F: generate, and the rate limit
    h = Host(bin_dir, answer="decline")
    try:
        h.wait_ready()
        check(len(h.generate({})) == 20, "default length is 20")
        password = h.generate({"length": 24, "symbols": False})
        check(len(password) == 24 and password.isalnum(), "length 24 without symbols is alphanumeric")
        check(len(h.generate({"length": 3})) == 8 and len(h.generate({"length": 1000})) == 128, "length is clamped to 8..128")
        password = h.generate({"length": 60, "exclude-chars": "abcdefghijklmnopqrstuvwxyz"})
        check(len(password) == 60 and not any(c.islower() for c in password), "exclude-chars is respected")
        check(error_name(lambda: h.generate({"lowercase": False, "uppercase": False, "digits": False, "symbols": False}))
              == ERR + "PolicyViolation", "no class left: PolicyViolation")
        check(error_name(lambda: h.generate({"exclude-chars": "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*"}))
              == ERR + "PolicyViolation", "everything excluded: PolicyViolation")
        check(error_name(lambda: h.generate({}, signature="(none)")) == ERR + "OriginMismatch", "unsigned caller: OriginMismatch")
        check(error_name(lambda: h.call("GeneratePassword", GLib.Variant("(a{sv}a{sv})", (vmap({"autofill-session-id": "s1", "app-signature": KEY_ID}), {})), "(a{sv})"))
              == ERR + "InvalidArgs", "no origin: InvalidArgs")

        # Several calls at once, each with its own policy. A password must
        # never answer a call with another policy.
        results = {}
        def make(i, policy):
            def done(source, res, data=None):
                try:
                    results[i] = source.call_finish(res).unpack()[0]["password"]
                except GLib.Error as error:
                    results[i] = "error:" + str(error)
            h.conn.call(BUS_NAME, PATH, IFACE, "GeneratePassword",
                        GLib.Variant("(a{sv}a{sv})", (h.request(SITE, app="app-g"), vmap(policy))), GLib.VariantType("(a{sv})"),
                        Gio.DBusCallFlags.NONE, 8000, None, done)
        policies = [{"length": 10, "lowercase": False, "uppercase": False, "symbols": False},
                    {"length": 30, "digits": False, "symbols": False},
                    {"length": 12, "lowercase": False, "uppercase": False, "digits": False},
                    {"length": 40, "exclude-chars": "0123456789"}]
        for i, policy in enumerate(policies):
            make(i, policy)
        check(pump(lambda: len(results) == 4, 10000), "four concurrent generate calls finish")
        check(len(results.get(0, "")) == 10 and results[0].isdigit(), "concurrent call 0: digits only")
        check(len(results.get(1, "")) == 30 and results[1].isalpha(), "concurrent call 1: letters only")
        check(len(results.get(2, "")) == 12 and all(c in "!@#$%^&*" for c in results[2]), "concurrent call 2: symbols only")
        check(len(results.get(3, "")) == 40 and not any(c.isdigit() for c in results[3]), "concurrent call 3: no digits")

        # Rate limit: 5 save prompts for each app in a minute.
        for i in range(5):
            h.save(SITE, "user%d" % i, "pw", app="app-rate", session="r")
        check(error_name(lambda: h.save_call(SITE, "user5", "pw", app="app-rate", session="r")) == ERR + "Busy", "the sixth save of an app in a minute: Busy")
        _, done = h.save(SITE, "other-app-user", "pw", app="app-other")
        check(done is not None, "the rate limit counts for each app")
    finally:
        h.stop()

    if failures:
        print("\n%d check(s) failed" % len(failures))
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: provider_v2.py <build directory with provider_host and ui_host>")
    sys.exit(run(sys.argv[1]))
