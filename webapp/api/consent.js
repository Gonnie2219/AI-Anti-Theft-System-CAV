const crypto = require('crypto');

module.exports = async function handler(req, res) {
  if (req.method !== 'POST') return res.status(405).json({ error: 'POST only' });

  const { phone, consents, smsConsent, userAgent, timestamp } = req.body || {};

  // Validate phone (E.164)
  if (!phone || !/^\+[1-9]\d{9,14}$/.test(phone)) {
    return res.status(400).json({ error: 'Invalid phone number. Must be E.164 format (e.g. +1XXXXXXXXXX).' });
  }

  // Validate required service consents (terms + privacy)
  if (!consents || consents.terms !== true || consents.privacy !== true) {
    return res.status(400).json({ error: 'Terms and Privacy Policy must be accepted.' });
  }

  const consentId = crypto.randomUUID();
  const recordedAt = new Date().toISOString();
  const ip = req.headers['x-forwarded-for'] || 'unknown';

  console.log('[CONSENT]', JSON.stringify({
    consentId, phone, smsConsent: !!smsConsent, timestamp, recordedAt, userAgent, ip
  }));

  return res.status(200).json({ success: true, consentId, recordedAt });
};
