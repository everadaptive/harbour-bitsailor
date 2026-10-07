# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT

# Register a test account on a local Vaultwarden. Test use only.
import base64, hashlib, hmac, json, os, sys, urllib.request
from cryptography.hazmat.primitives import hashes, padding, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand

BASE, EMAIL, PASSWORD = sys.argv[1], sys.argv[2], sys.argv[3]
ITER = 600000
b64 = lambda b: base64.b64encode(b).decode()

def post(path, body):
    req = urllib.request.Request(BASE + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()

def enc(key64, data):
    ek, mk = key64[:32], key64[32:]
    iv = os.urandom(16)
    p = padding.PKCS7(128).padder()
    c = Cipher(algorithms.AES(ek), modes.CBC(iv)).encryptor()
    ct = c.update(p.update(data) + p.finalize()) + c.finalize()
    mac = hmac.new(mk, iv + ct, hashlib.sha256).digest()
    return "2.%s|%s|%s" % (b64(iv), b64(ct), b64(mac))

master = hashlib.pbkdf2_hmac("sha256", PASSWORD.encode(), EMAIL.lower().encode(), ITER, 32)
mph = b64(hashlib.pbkdf2_hmac("sha256", master, PASSWORD.encode(), 1, 32))
stretched = HKDFExpand(hashes.SHA256(), 32, b"enc", None).derive(master) + \
            HKDFExpand(hashes.SHA256(), 32, b"mac", None).derive(master)
user_key = os.urandom(64)
priv = rsa.generate_private_key(65537, 2048)
priv_der = priv.private_bytes(serialization.Encoding.DER, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
pub_der = priv.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)

st, body = post("/identity/accounts/register/send-verification-email",
                {"email": EMAIL, "name": "Autofill Test", "receiveMarketingEmails": False})
print("send-verification:", st, body[:80])
token = body.strip().strip('"') if st == 200 else None
st, body = post("/identity/accounts/register/finish", {
    "email": EMAIL, "emailVerificationToken": token,
    "masterPasswordHash": mph, "masterPasswordHint": None,
    "userSymmetricKey": enc(stretched, user_key),
    "userAsymmetricKeys": {"publicKey": b64(pub_der), "encryptedPrivateKey": enc(user_key, priv_der)},
    "kdf": 0, "kdfIterations": ITER,
})
print("finish:", st, body[:200])
