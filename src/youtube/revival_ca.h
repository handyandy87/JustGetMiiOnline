// The root the revival host's certificate is signed by.
//
// The app ships its own certificate store at /vol/content/ssl/certs and ignores the system
// one, and there is no ISRG root among its 68, so a Let's Encrypt certificate cannot serve it.
// This root goes in beside them, under the filename openssl derives from its subject.
//
// Only the public certificate is here. The key that signs with it never leaves the machine
// that made it.
#pragma once

#define REVIVAL_CA_FILENAME "bc844f63.0"

static const char REVIVAL_CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "i7DJ+su7dz1BWD0cUA==\n"
    "-----END CERTIFICATE-----\n"
    ;
