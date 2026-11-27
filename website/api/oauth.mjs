// Provider secrets stay on Vercel. The Mac holds a fresh verifier for each login.
import { createCipheriv, createDecipheriv, createHash, randomBytes } from 'node:crypto';

const endpoint = 'https://hermes-mac-saksham.vercel.app/api/oauth';
const callback = 'com.sakshamuw.hermes.auth://oauth/complete';
const providers = {
  slack: { authorize: 'https://slack.com/oauth/v2/authorize', token: 'https://slack.com/api/oauth.v2.access' },
  github: { authorize: 'https://github.com/login/oauth/authorize', token: 'https://github.com/login/oauth/access_token' }
};
const encode = b => b.toString('base64url');
const challenge = s => encode(createHash('sha256').update(s).digest());
const valid = s => typeof s === 'string' && /^[A-Za-z0-9_-]{43}$/.test(s);

function seal(payload, key) {
  const iv = randomBytes(12);
  const aes = createCipheriv('aes-256-gcm', key, iv);
  const bytes = Buffer.concat([aes.update(JSON.stringify(payload)), aes.final()]);
  return [iv, bytes, aes.getAuthTag()].map(encode).join('.');
}
function unseal(text, key, kind) {
  if (typeof text !== 'string' || text.length > 8000) throw Error();
  const [iv, bytes, tag] = text.split('.').map(s => Buffer.from(s, 'base64url'));
  const aes = createDecipheriv('aes-256-gcm', key, iv);
  aes.setAuthTag(tag);
  const p = JSON.parse(Buffer.concat([aes.update(bytes), aes.final()]));
  if (p.kind !== kind || p.expires < Date.now()) throw Error();
  return p;
}

export default async function handler(req, res) {
  res.setHeader('Cache-Control','no-store');
  res.setHeader('Referrer-Policy','no-referrer');
  const q = req.query;
  const name = q.provider;
  if (typeof name !== 'string' || !Object.hasOwn(providers, name)) return res.status(400).end();
  const provider = providers[name];
  const prefix = 'HERMES_' + name.toUpperCase();
  const id = process.env[prefix + '_CLIENT_ID'], secret = process.env[prefix + '_CLIENT_SECRET'];
  const slug = process.env.HERMES_GITHUB_APP_SLUG;
  if (!id || !secret || (name === 'github' && !/^[a-z0-9-]+$/.test(slug || ''))) {
    return res.status(503).send('HERMES sign-in for this provider is awaiting app registration.');
  }
  const key = createHash('sha256').update('hermes-oauth-v1:' + name + ':' + secret).digest();
  const redirect = new URL('/auth/' + name + '/callback', endpoint).href;
  try {
    if (req.method === 'GET' && q.action === 'ready') return res.status(200).json({ ready:true });
    if (req.method === 'GET' && q.action === 'start') {
      if (!valid(q.nonce) || !valid(q.challenge)) throw Error();
      const state = seal({ kind:'start', nonce:q.nonce, challenge:q.challenge, expires:Date.now()+600000 },key);
      const url = new URL(provider.authorize);
      url.search = new URLSearchParams({ client_id:id, redirect_uri:redirect, state });
      if (name === 'slack') url.searchParams.set('user_scope','chat:write,channels:read,groups:read');
      else {
        url.searchParams.set('code_challenge',q.challenge);
        url.searchParams.set('code_challenge_method','S256');
      }
      return res.redirect(302,url.href);
    }
    if (req.method === 'GET' && q.action === 'callback') {
      const start = unseal(q.state,key,'start');
      const url = new URL(callback);
      url.searchParams.set('state',start.nonce);
      if (q.error) url.searchParams.set('error','denied');
      else {
        if (typeof q.code !== 'string' || !q.code || q.code.length > 2000) throw Error();
        url.searchParams.set('ticket',seal({ kind:'exchange', code:q.code, challenge:start.challenge, expires:Date.now()+120000 },key));
      }
      return res.redirect(302,url.href);
    }
    if (req.method !== 'POST' || !['exchange','refresh'].includes(q.action)) return res.status(405).end();
    if (!req.headers['content-type']?.startsWith('application/json')) return res.status(415).end();
    const args = new URLSearchParams({ client_id:id, client_secret:secret });
    if (q.action === 'exchange') {
      const ticket = unseal(req.body.ticket,key,'exchange');
      if (!valid(req.body.verifier) || challenge(req.body.verifier) !== ticket.challenge) throw Error();
      args.set('code',ticket.code);
      args.set('redirect_uri',redirect);
      if (name === 'github') args.set('code_verifier', req.body.verifier);
    } else {
      if (typeof req.body.refresh !== 'string' || !req.body.refresh || req.body.refresh.length > 4000) throw Error();
      args.set('grant_type','refresh_token'); args.set('refresh_token',req.body.refresh);
    }
    const response = await fetch(provider.token, { method:'POST', headers:{ Accept:'application/json' }, body:args, redirect:'error', signal:AbortSignal.timeout(15000) });
    const data = await response.json();
    if (!response.ok || data.error || data.ok === false) return res.status(401).json({error:'Authorization expired or was denied. Connect again.'});
    const user = name === 'slack' && q.action === 'exchange' ? data.authed_user : data;
    if (!user?.access_token || (name==='slack' && user.token_type!=='user')) return res.status(401).end();
    return res.status(200).json({ token:user.access_token, refresh:user.refresh_token, expires:user.expires_in,
      workspace:data.team?.id, install:name==='github' ? 'https://github.com/apps/'+slug+'/installations/new' : undefined });
  } catch {
    return res.status(400).send('Authorization could not be completed. Return to HERMES and try again.');
  }
}
