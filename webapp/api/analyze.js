const Anthropic = require('@anthropic-ai/sdk');

const client = new Anthropic();

module.exports = async function handler(req, res) {
  if (req.method !== 'POST') return res.status(405).json({ error: 'POST only' });

  const { imageUrl } = req.body || {};
  if (!imageUrl) return res.status(400).json({ error: 'imageUrl required' });

  // SSRF protection: only allow ntfy.sh image URLs
  try {
    const parsed = new URL(imageUrl);
    if (!parsed.hostname.endsWith('ntfy.sh')) {
      return res.status(400).json({ error: 'Only ntfy.sh image URLs are allowed' });
    }
    if (!['http:', 'https:'].includes(parsed.protocol)) {
      return res.status(400).json({ error: 'Invalid URL protocol' });
    }
  } catch {
    return res.status(400).json({ error: 'Invalid URL' });
  }

  try {
    // Fetch image and convert to base64
    const imgResp = await fetch(imageUrl);
    if (!imgResp.ok) throw new Error('Failed to fetch image');
    const buf = Buffer.from(await imgResp.arrayBuffer());
    const base64 = buf.toString('base64');
    const contentType = imgResp.headers.get('content-type') || 'image/jpeg';

    const message = await client.messages.create({
      model: 'claude-sonnet-4-20250514',
      max_tokens: 256,
      messages: [{
        role: 'user',
        content: [
          { type: 'image', source: { type: 'base64', media_type: contentType, data: base64 } },
          { type: 'text', text: 'You are a security camera AI. Analyze this photo and classify the threat level as HIGH, MEDIUM, or LOW. Reply with exactly one line in this format: THREAT_LEVEL: <HIGH|MEDIUM|LOW> - <short reason>. HIGH = person clearly visible or door/window tampered. MEDIUM = ambiguous shape, partial occlusion, animal. LOW = empty scene, wind, lighting change.' }
        ]
      }]
    });

    const text = message.content[0].text.trim();
    const match = text.match(/THREAT_LEVEL:\s*(HIGH|MEDIUM|LOW)\s*-\s*(.*)/i);
    const threatLevel = match ? match[1].toUpperCase() : 'MEDIUM';
    const verdict = match ? match[2].trim() : text;

    return res.status(200).json({ verdict, threatLevel });
  } catch (err) {
    console.error('Analyze error:', err);
    return res.status(500).json({ error: 'Analysis failed' });
  }
};
