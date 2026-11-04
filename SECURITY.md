# Security and intended deployment

**DC-OGX-W is intended for a trusted private network.** Its local browser configuration service and remote-input transport are not designed as hardened, Internet-facing services. Do not expose the adapter or its SoftAP to the public Internet using port forwarding or a reverse proxy without separately implementing authentication, transport security and access restrictions.

- Change default network credentials when required by your environment.
- Keep firmware packages from trusted sources; verify you are using the correct board target.
- Do not post Wi-Fi credentials or personal network addresses in public issue screenshots/logs.
- Report a reproducible security problem privately to the repository maintainer using GitHub's private vulnerability reporting feature, if enabled. Otherwise contact the maintainer through an appropriate private channel rather than posting exploit instructions publicly.

The ESP8285 module's AT firmware is a separate component. Updating the RP2040 does not patch the ESP8285 itself.
