#!/usr/bin/env python3
"""Sign release images for the watch's update check (ECDSA P-256 over SHA-256, DER).

  sign_firmware.py keygen [--out update_signing_key.pem]   new key pair; prints the public key for include/UpdateKey.h
  sign_firmware.py sign FIRMWARE.bin [--key KEY.pem]       writes FIRMWARE.bin.sig
  sign_firmware.py verify FIRMWARE.bin PUBLIC.pem          checks FIRMWARE.bin.sig

Without --key the private key is read from the environment variable
UPDATE_SIGNING_KEY (PEM), which is how the CI release step gets it.
Keep the private key out of the repository.

Requirement: pip install cryptography
"""
import argparse
import os
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


def public_pem(key):
    return key.public_key().public_bytes(serialization.Encoding.PEM,
                                         serialization.PublicFormat.SubjectPublicKeyInfo).decode()


def cmd_keygen(args):
    if os.path.exists(args.out):
        sys.exit(f"{args.out} exists, not overwriting")
    key = ec.generate_private_key(ec.SECP256R1())
    with open(args.out, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    print(f"private key: {args.out}  (GitHub secret UPDATE_SIGNING_KEY, never commit it)")
    print("public key for include/UpdateKey.h:\n")
    print("static const char UPDATE_PUBKEY_PEM[] =")
    for line in public_pem(key).splitlines():
        print(f'    "{line}\\n"')
    print("    ;")


def cmd_sign(args):
    pem = open(args.key, "rb").read() if args.key else os.environ.get("UPDATE_SIGNING_KEY", "").encode()
    if not pem.strip():
        sys.exit("no key: pass --key or set UPDATE_SIGNING_KEY")
    key = serialization.load_pem_private_key(pem, password=None)
    image = open(args.image, "rb").read()
    sig = key.sign(image, ec.ECDSA(hashes.SHA256()))
    key.public_key().verify(sig, image, ec.ECDSA(hashes.SHA256()))     # self-check before publishing
    with open(args.image + ".sig", "wb") as f:
        f.write(sig)
    print(f"{args.image}.sig: {len(sig)} bytes")


def cmd_verify(args):
    pub = serialization.load_pem_public_key(open(args.pubkey, "rb").read())
    try:
        pub.verify(open(args.image + ".sig", "rb").read(), open(args.image, "rb").read(), ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        sys.exit("signature INVALID")
    print("signature ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("keygen")
    p.add_argument("--out", default="update_signing_key.pem")
    p.set_defaults(fn=cmd_keygen)
    p = sub.add_parser("sign")
    p.add_argument("image")
    p.add_argument("--key")
    p.set_defaults(fn=cmd_sign)
    p = sub.add_parser("verify")
    p.add_argument("image")
    p.add_argument("pubkey")
    p.set_defaults(fn=cmd_verify)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
