// provisioning.h — first-boot Wi-Fi provisioning via Improv-serial (USB).
// Runs only when NVS holds no valid credentials. Blocks (showing a setup
// screen) until the web installer provisions Wi-Fi, then reboots into the
// normal app. No-op when creds already exist.
#pragma once
void provisioning_gate(void);
