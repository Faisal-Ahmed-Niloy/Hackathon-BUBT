// Root CA certificate(s) used to CHECK the platform's HTTPS certificate.
// Public certificates - not secrets.
// Regenerate for your platform address with:
//     python tools/make_ca_header.py https://<your-platform-host>
// Until generated, the firmware refuses to talk to the server (no insecure TLS).
#pragma once
static const char ROOT_CA[] = "";
