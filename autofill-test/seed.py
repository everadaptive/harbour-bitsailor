# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT

# Seed the local Vaultwarden test vault through the API. Test use only.
import base64, hashlib, hmac, json, os, ssl, sys, urllib.parse, urllib.request
from cryptography.hazmat.primitives import hashes, padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand

BASE, EMAIL, PASSWORD = sys.argv[1], sys.argv[2], sys.argv[3]
ITER = 600000
ctx = ssl._create_unverified_context()
b64 = lambda b: base64.b64encode(b).decode()

def call(path, body=None, form=None, token=None):
    data = urllib.parse.urlencode(form).encode() if form else (json.dumps(body).encode() if body is not None else None)
    h = {"Content-Type": "application/x-www-form-urlencoded" if form else "application/json"}
    if token:
        h["Authorization"] = "Bearer " + token
    req = urllib.request.Request(BASE + path, data, h)
    try:
        with urllib.request.urlopen(req, context=ctx) as r:
            return json.loads(r.read().decode() or "null")
    except urllib.error.HTTPError as e:
        sys.exit("HTTP %d on %s: %s" % (e.code, path, e.read().decode()[:300]))

def enc(key64, text):
    ek, mk = key64[:32], key64[32:]
    iv = os.urandom(16)
    p = padding.PKCS7(128).padder()
    c = Cipher(algorithms.AES(ek), modes.CBC(iv)).encryptor()
    ct = c.update(p.update(text.encode()) + p.finalize()) + c.finalize()
    return "2.%s|%s|%s" % (b64(iv), b64(ct), b64(hmac.new(mk, iv + ct, hashlib.sha256).digest()))

def dec(key64, s):
    iv, ct, mac = [base64.b64decode(x) for x in s[2:].split("|")]
    assert hmac.compare_digest(hmac.new(key64[32:], iv + ct, hashlib.sha256).digest(), mac)
    d = Cipher(algorithms.AES(key64[:32]), modes.CBC(iv)).decryptor()
    u = padding.PKCS7(128).unpadder()
    return u.update(d.update(ct) + d.finalize()) + u.finalize()

master = hashlib.pbkdf2_hmac("sha256", PASSWORD.encode(), EMAIL.lower().encode(), ITER, 32)
mph = b64(hashlib.pbkdf2_hmac("sha256", master, PASSWORD.encode(), 1, 32))
stretched = HKDFExpand(hashes.SHA256(), 32, b"enc", None).derive(master) + \
            HKDFExpand(hashes.SHA256(), 32, b"mac", None).derive(master)

tok = call("/identity/connect/token", form={
    "grant_type": "password", "username": EMAIL, "password": mph, "scope": "api offline_access",
    "client_id": "cli", "deviceType": "8", "deviceName": "seed-script",
    "deviceIdentifier": "6d9b3b1a-0000-4000-8000-000000000001"})
key_str = tok.get("Key") or tok.get("key") or tok["UserDecryptionOptions"]["MasterPasswordUnlock"]["MasterKeyEncryptedUserKey"]
user_key = dec(stretched, key_str)
access = tok["access_token"]

existing = call("/api/ciphers", token=access)
names = {dec(user_key, c["name"]).decode() for c in existing.get("data", [])}

def add(name, user, pw, uri):
    if name in names:
        print("exists:", name); return
    call("/api/ciphers", token=access, body={
        "type": 1, "name": enc(user_key, name), "notes": None, "favorite": False, "folderId": None,
        "login": {"username": enc(user_key, user), "password": enc(user_key, pw), "totp": None,
                  "uris": [{"uri": enc(user_key, uri), "match": None}]}})
    print("created:", name)

add("Demo app",            "alice", "demo-app-pass-1",  "app://autofill-demo")
add("Example.com - Alice", "alice", "alice-example-pw", "https://example.com")
add("Example.com - Bob",   "bob",   "bob-example-pw",   "https://example.com")
print("items in vault:", len(call("/api/ciphers", token=access)["data"]))
