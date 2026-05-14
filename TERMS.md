# Terms and Conditions — AI-Enabled Anti-Theft System SMS Program

**Program name:** AI-Enabled Unauthorized Access Detection for Connected and Autonomous Vehicles (CAVs)
**Operator:** Gonnie Ben-Tal (Sole Proprietor)
**Project context:** Wayne State University UROP HP0787, Department of Electrical and Computer Engineering, advised by Dr. Lubna Alazzawi
**Support contact:** gonnie2219@gmail.com
**Effective date:** May 14, 2026

---

## 1. Description of the SMS program

The Anti-Theft System is a research-grade vehicle security prototype that monitors a single vehicle using vibration, magnetic-reed, and camera sensors. When an intrusion event is detected (vibration or door opening while armed), the system sends an SMS alert to a pre-configured phone number belonging to the system's owner-operator. The owner can also send SMS commands (ARM, DISARM, STATUS, PHOTO, GPS, HELP, STOP) to control or query the device.

## 2. Sole owner-operator model

This program operates strictly between two endpoints:

- one vehicle-mounted device, and
- one owner-operator phone number, hard-coded in the device firmware.

There is no mass enrollment, no third-party recipients, no list acquisition, and no marketing distribution. The only person who receives outbound SMS from this campaign is the same person who owns the SIM, owns the device, and authored the firmware.

## 3. Opt-in

The recipient phone number is configured by the owner-operator directly in the device firmware (`AntiTheftSystemMainESP.ino` / `AntiTheftSystemLilygo.ino`) before deployment. By flashing the firmware with their own number, the owner-operator self-administers opt-in. The system also persists opt-out state in non-volatile storage (NVS) so that an opt-out survives power cycles.

## 4. Message frequency

Messages are event-driven, not scheduled. Under normal conditions the system sends approximately **0–2 messages per week**. During an active intrusion event, the system may send up to **5 messages within a short window** (alert, GPS link, photo URL, immobilization-state change confirmations).

## 5. Message and data rates

**Message and data rates may apply.** Standard carrier rates for the recipient's mobile plan apply to all messages received.

## 6. **HELP**

Reply **HELP** at any time to receive a list of available commands and the operator's support contact. The system will respond with a single SMS listing the available commands (ARM, DISARM, STATUS, PHOTO, GPS) and the support email gonnie2219@gmail.com.

## 7. **STOP**

Reply **STOP** at any time to opt out of all SMS from this program. On receipt of STOP, the device firmware records an opt-out flag in NVS and suppresses all subsequent outbound SMS to the recipient number. After STOP, the device will no longer originate SMS to that number until the firmware is re-flashed with a new number.

## 8. Supported carriers

This program supports all major US carriers. Carriers are not liable for delayed or undelivered messages.

## 9. Privacy

User data handling, retention, and sharing practices are described in the program's Privacy Policy:
https://github.com/Gonnie2219/AI-Anti-Theft-System-CAV/blob/master/PRIVACY.md

## 10. Changes to these terms

These terms may be updated at any time. The current authoritative version is published at:
https://github.com/Gonnie2219/AI-Anti-Theft-System-CAV/blob/master/TERMS.md
