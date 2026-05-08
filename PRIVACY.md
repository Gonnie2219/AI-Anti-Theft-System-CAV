# Privacy Policy — AI-Enabled Vehicle Anti-Theft System

**Effective date:** May 7, 2026

## 1. Scope

This privacy policy covers the personal vehicle anti-theft system operated by Gonnie Ben-Tal as part of Wayne State University Undergraduate Research Opportunity Program project HP0787. The system is a single-owner, single-recipient research prototype.

## 2. Information collected

The system collects and processes:
- The phone number of the vehicle owner (the sole recipient of all outbound SMS)
- GPS coordinates of the vehicle, captured at the moment of an alert event
- Photographs from an onboard camera, captured at the moment of an alert event
- Sensor event metadata (vibration detection, door open/close, timestamp)

## 3. How the information is used

All collected information is used solely to deliver alert messages and command responses to the device owner via SMS. Information is not used for any other purpose.

## 4. Data sharing

We do not sell, rent, lease, or share any personal information with third parties. We do not use information for marketing or promotional purposes.

## 5. Data retention

Sensor events and photographs are retained on the ntfy.sh notification service for up to 12 hours, then automatically purged. SMS message logs are retained by the SMS service provider (Twilio) per their standard retention schedule. No long-term database of user activity is maintained.

## 6. SMS Consent and Opt-In

**Call to action:** During device provisioning, the vehicle owner enters their own mobile phone number into the system firmware as the alert recipient. This action constitutes opt-in consent to receive SMS messages from the system. By provisioning their number, the owner consents to receive event-driven SMS alerts and command-response messages from the registered Twilio number associated with this campaign.

**Message types sent by this campaign:**
1. Intrusion alerts triggered by vibration or door sensors, containing the alert reason, a timestamp, GPS coordinates, a Google Maps link, and a URL to a photo captured at the moment of the event.
2. Reply messages confirming receipt of, or returning information for, commands the owner sends to the system: ARM, DISARM, STATUS, PHOTO, GPS, IMMOBILIZE, RESTORE, HELP.

**Message frequency:** Event-driven. Typically 0–2 messages per week during normal use, plus replies to commands the user explicitly initiates.

**Cost:** Message and data rates may apply per the recipient's mobile carrier plan.

**Opt-out:** Reply STOP to the registered Twilio number at any time to unsubscribe. All outbound SMS from the system to that number is then disabled. Reply START to resubscribe.

**Help:** Reply HELP to the registered Twilio number to receive support contact information.

**No third parties:** Messages are sent only to the device owner's own phone number, which the owner configured into the device themselves. There is no list, no enrollment of others, no resale of recipient information, and no sharing of message content with third parties.

## 7. Opt-out

The vehicle owner may opt out of all SMS messages at any time by texting STOP to the registered Twilio number, which disables outbound SMS from this system to that number.

## 8. Contact

For questions about this privacy policy: gonnie2219@gmail.com
