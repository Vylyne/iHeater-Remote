#pragma once

#include <pgmspace.h>

// The login page's script, served at GET /auth.js. It is the browser copy of
// lib/remote-policy/src/AuthCrypto and must give byte-identical results; the
// vectors in scripts/test_auth_js.py check that. It avoids crypto.subtle,
// which doesn't exist on plain HTTP. Keep it ASCII and free of the sequence
// that closes the raw string below.
static const char kAuthJs[] PROGMEM = R"JS(
(function () {
  "use strict";
  var K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ]);
  var W = new Uint32Array(64);

  function Sha() {
    this.h = new Uint32Array([
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    ]);
    this.buf = new Uint8Array(64);
    this.n = 0;
    this.total = 0;
  }
  Sha.prototype.clone = function () {
    var c = new Sha();
    c.h.set(this.h);
    c.buf.set(this.buf);
    c.n = this.n;
    c.total = this.total;
    return c;
  };
  Sha.prototype.block = function (b, o) {
    var h = this.h, i, t1, t2;
    for (i = 0; i < 16; i++) {
      W[i] = ((b[o + 4 * i] << 24) | (b[o + 4 * i + 1] << 16) |
              (b[o + 4 * i + 2] << 8) | b[o + 4 * i + 3]) >>> 0;
    }
    for (i = 16; i < 64; i++) {
      var x = W[i - 15], y = W[i - 2];
      var s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      var s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      W[i] = (W[i - 16] + s0 + W[i - 7] + s1) >>> 0;
    }
    var a = h[0], bb = h[1], c = h[2], d = h[3];
    var e = h[4], f = h[5], g = h[6], hh = h[7];
    for (i = 0; i < 64; i++) {
      var S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^
               ((e >>> 25) | (e << 7));
      var ch = (e & f) ^ (~e & g);
      t1 = (hh + S1 + ch + K[i] + W[i]) >>> 0;
      var S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^
               ((a >>> 22) | (a << 10));
      var maj = (a & bb) ^ (a & c) ^ (bb & c);
      t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = bb; bb = a; a = (t1 + t2) >>> 0;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  };
  Sha.prototype.update = function (data) {
    var i = 0, len = data.length;
    this.total += len;
    if (this.n > 0) {
      while (i < len && this.n < 64) this.buf[this.n++] = data[i++];
      if (this.n === 64) { this.block(this.buf, 0); this.n = 0; }
    }
    while (len - i >= 64) { this.block(data, i); i += 64; }
    while (i < len) this.buf[this.n++] = data[i++];
    return this;
  };
  Sha.prototype.finish = function () {
    var bits = this.total * 8;
    var pad = new Uint8Array(this.n < 56 ? 64 - this.n : 128 - this.n);
    pad[0] = 0x80;
    var hi = Math.floor(bits / 4294967296), lo = bits >>> 0;
    var p = pad.length;
    pad[p - 8] = hi >>> 24; pad[p - 7] = hi >>> 16;
    pad[p - 6] = hi >>> 8; pad[p - 5] = hi;
    pad[p - 4] = lo >>> 24; pad[p - 3] = lo >>> 16;
    pad[p - 2] = lo >>> 8; pad[p - 1] = lo;
    this.update(pad);
    var out = new Uint8Array(32);
    for (var i = 0; i < 8; i++) {
      out[4 * i] = this.h[i] >>> 24; out[4 * i + 1] = this.h[i] >>> 16;
      out[4 * i + 2] = this.h[i] >>> 8; out[4 * i + 3] = this.h[i];
    }
    return out;
  };

  function sha256(data) { return new Sha().update(data).finish(); }

  // Inner and outer states with the key pads already absorbed.
  function hmacStates(key) {
    var k = new Uint8Array(64);
    k.set(key.length > 64 ? sha256(key) : key);
    var ip = new Uint8Array(64), op = new Uint8Array(64);
    for (var i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    return { i: new Sha().update(ip), o: new Sha().update(op) };
  }
  function hmacWith(st, data) {
    var inner = st.i.clone().update(data).finish();
    return st.o.clone().update(inner).finish();
  }
  function hmac(key, data) { return hmacWith(hmacStates(key), data); }

  function pbkdf2(pw, salt, iterations, len) {
    var st = hmacStates(pw);
    var out = new Uint8Array(len);
    var blocks = Math.ceil(len / 32);
    for (var b = 1; b <= blocks; b++) {
      var s = new Uint8Array(salt.length + 4);
      s.set(salt);
      s[salt.length] = b >>> 24; s[salt.length + 1] = b >>> 16;
      s[salt.length + 2] = b >>> 8; s[salt.length + 3] = b;
      var u = hmacWith(st, s), t = new Uint8Array(u);
      for (var i = 1; i < iterations; i++) {
        u = hmacWith(st, u);
        for (var j = 0; j < 32; j++) t[j] ^= u[j];
      }
      out.set(t.subarray(0, Math.min(32, len - (b - 1) * 32)), (b - 1) * 32);
    }
    return out;
  }

  function hex(u8) {
    var s = "";
    for (var i = 0; i < u8.length; i++) {
      s += (u8[i] < 16 ? "0" : "") + u8[i].toString(16);
    }
    return s;
  }
  function unhex(s) {
    var out = new Uint8Array(s.length >> 1);
    for (var i = 0; i < out.length; i++) {
      out[i] = parseInt(s.substr(2 * i, 2), 16);
    }
    return out;
  }
  function utf8(s) { return new TextEncoder().encode(s); }

  function fail(res) {
    return res.json().catch(function () { return {}; }).then(function (j) {
      throw new Error(j.error || ("Request failed (" + res.status + ")."));
    });
  }
  function getChallenge() {
    return fetch("/api/auth/challenge", { credentials: "same-origin" })
      .then(function (res) { return res.ok ? res.json() : fail(res); });
  }
  function post(url, fields) {
    return fetch(url, {
      method: "POST",
      credentials: "same-origin",
      body: new URLSearchParams(fields)
    }).then(function (res) { return res.ok ? undefined : fail(res); });
  }

  function login(password) {
    return getChallenge().then(function (c) {
      if (!c.passwordSet) throw new Error("No password is set yet.");
      var v = pbkdf2(utf8(password), unhex(c.salt), c.iterations, 32);
      var proof = hmac(v, unhex(c.nonce));
      return post("/api/auth/login", { nonce: c.nonce, proof: hex(proof) });
    });
  }

  function setPassword(password) {
    return getChallenge().then(function (c) {
      var v = pbkdf2(utf8(password), unhex(c.newSalt), c.newIterations, 32);
      return post("/api/auth/password", {
        salt: c.newSalt,
        iterations: String(c.newIterations),
        verifier: hex(v)
      });
    });
  }

  globalThis.IHA = {
    sha256: sha256, hmac: hmac, pbkdf2: pbkdf2, hex: hex, unhex: unhex,
    utf8: utf8, login: login, setPassword: setPassword
  };
})();
)JS";
