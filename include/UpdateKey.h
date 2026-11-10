#pragma once
// Public key (ECDSA P-256, PEM) that release images must be signed with.
// Empty = signatures are not enforced: downloads then rely on HTTPS to GitHub
// plus the confirmation on the watch. Create a key pair and sign releases with
// tools/sign_firmware.py, then paste the public key here.
static const char UPDATE_PUBKEY_PEM[] = "";
