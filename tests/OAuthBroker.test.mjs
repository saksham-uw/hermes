import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import handler from '../website/api/oauth.mjs';

async function request(provider, action, query={}, body) {
  const res = {
    setHeader() {}, status(code) { this.code=code; return this; }, end() { return this; },
    send(text) { this.text=text; return this; }, json(value) { this.value=value; return this; },
    redirect(code,url) { this.code=code; this.url=new URL(url); return this; }
  };
  return handler({method:body?'POST':'GET',query:{provider,action,...query}, headers:{'content-type':'application/json'},body},res);
}

for (const provider of ['github','slack']) {
  test(provider+' approval is bound to the initiating Mac and returns a user token', async t=>{
    t.mock.method(globalThis,'fetch',async(url,opts)=>{
      assert.equal(opts.body.get('code'),'accepted');
      assert.equal(opts.redirect,'error');
      return {ok:true,json:async()=>provider==='github'?{access_token:'ghu_test'}:
        {ok:true,access_token:'bot-token',team:{id:'T1'},authed_user:{token_type:'user',access_token:'xoxp-test'}}};
    });
    const prefix='HERMES_'+provider.toUpperCase();
    process.env[prefix+'_CLIENT_ID']='test'; process.env[prefix+'_CLIENT_SECRET']='fixture';
    process.env.HERMES_GITHUB_APP_SLUG='hermes-test';
    const verifier=randomBytes(32).toString('base64url'),nonce=randomBytes(32).toString('base64url');
    const challenge=createHash('sha256').update(verifier).digest('base64url');
    const start=await request(provider,'start',{challenge,nonce});
    assert.equal(start.code,302);
    assert.equal(start.url.hostname,provider==='slack'?'slack.com':'github.com');
    const state=start.url.searchParams.get('state');
    const tampered=await request(provider,'callback',{state:'invalid',code:'accepted'});assert.equal(tampered.code,400);
    const callback=await request(provider,'callback',{state,code:'accepted'});
    assert.equal(callback.url.protocol,'com.sakshamuw.hermes.auth:');
    assert.equal(callback.url.searchParams.get('state'),nonce);
    const ticket=callback.url.searchParams.get('ticket');
    assert.equal((await request(provider,'exchange',{}, {ticket,verifier:nonce})).code,400);
    const ok=await request(provider,'exchange',{}, {ticket,verifier});
    assert.equal(ok.value.token,provider==='slack'?'xoxp-test':'ghu_test');
    const denied=await request(provider,'callback',{state,error:'access_denied'});
    assert.equal(denied.url.searchParams.get('error'),'denied');
  });
}
