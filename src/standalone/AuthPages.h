#pragma once

#include <pgmspace.h>

// The /login page: the login form, or the Set a password form while no
// password is set. It loads /auth.js, which does the hashing in the browser so
// the password never leaves it. The `next` query value is read in script and
// checked with the same rule as policy::safeNextPath; it is never written into
// the markup. Keep it free of runtime placeholders and of the sequence that
// closes the raw string below.
static const char kLoginHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><link rel="icon" type="image/png" href="/logo.png"><title>iHeater Remote: log in</title>
<style>
:root{color-scheme:dark;--bg:#0a0e14;--panel:#111823;--inset:#0d131c;--line:#253244;--text:#e8eef6;--muted:#8c9bb0;--accent:#5ba9ff;--warn:#ffb347;--bad:#ff7a85;font-family:ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;background:var(--bg);color:var(--text);padding:16px}
main{width:100%;max-width:380px;background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:20px}
h1{margin:0 0 12px;font-size:1.25rem}label{display:block;margin:12px 0 4px;color:var(--muted);font-size:.9rem}
input{width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--inset);color:var(--text);font:inherit}
button{margin-top:16px;width:100%;padding:11px;border-radius:8px;border:0;background:var(--accent);color:#04121f;font:inherit;font-weight:600;cursor:pointer}
button:disabled{opacity:.6;cursor:default}
.note{margin:0 0 10px;padding:8px 10px;border-radius:8px;border:1px solid var(--warn);color:var(--warn);font-size:.85rem}
#msg{min-height:1.2em;margin-top:12px;color:var(--bad);font-size:.9rem}.hidden{display:none}
</style></head><body>
<main><h1 id="title">Log in</h1><div id="notes"></div>
<form id="form" autocomplete="on"><input type="text" name="username" value="remote" autocomplete="username" class="hidden" tabindex="-1" aria-hidden="true">
<label for="pw">Password</label><input id="pw" type="password" autocomplete="current-password" required>
<div id="confirmRow" class="hidden"><label for="pw2">Confirm password</label><input id="pw2" type="password" autocomplete="new-password"></div>
<button id="go" type="submit" disabled>Log in</button></form><div id="msg" role="alert"></div></main>
<script src="/auth.js"></script>
<script>
(function(){
var $=function(id){return document.getElementById(id)},me=null,setting=false;
function safeNext(n){
  if(typeof n!=="string"||n.charAt(0)!=="/")return false;
  if(n.charAt(1)==="/"||n.charAt(1)==="\\")return false;
  if(new TextEncoder().encode(n).length>64)return false;
  for(var i=0;i<n.length;i++){var c=n.charCodeAt(i);if(c<32||c===127)return false}
  return true;
}
function go(){
  var n=new URLSearchParams(location.search).get("next");
  location.replace(safeNext(n)?n:"/");
}
function note(text){var p=document.createElement("p");p.className="note";p.textContent=text;$("notes").appendChild(p)}
function show(m){
  me=m;setting=!m.passwordSet;
  if(m.loggedIn){go();return}
  var t=setting?"Set a password":"Log in";
  $("title").textContent=t;$("go").textContent=t;
  $("pw").autocomplete=setting?"new-password":"current-password";
  $("confirmRow").classList.toggle("hidden",!setting);
  $("pw2").required=setting;
  if(location.protocol!=="https:")note("This connection isn't encrypted. Your password isn't sent, but someone on this network could capture enough to log in. Use the HTTPS address through nginx, or the Remote's own access point.");
  if(m.apOpen)note("This access point is open: anyone nearby can read this traffic.");
  if(setting)note("Set a password now: until you do, anyone who can reach this Remote can set one.");
  $("go").disabled=false;
}
$("form").onsubmit=function(ev){
  ev.preventDefault();
  var pw=$("pw").value,msg=$("msg");
  msg.textContent="";
  if(setting){
    if(pw.length<8){msg.textContent="Use at least 8 characters.";return}
    if(pw!==$("pw2").value){msg.textContent="The passwords don't match.";return}
  }
  var btn=$("go"),label=btn.textContent;
  btn.disabled=true;btn.textContent="Working...";
  // Let the button repaint before the hashing keeps the page busy.
  setTimeout(function(){
    (setting?IHA.setPassword(pw):IHA.login(pw)).then(go,function(e){
      msg.textContent=e.message;btn.disabled=false;btn.textContent=label;
    });
  },30);
};
fetch("/api/auth/me",{credentials:"same-origin"}).then(function(r){return r.json()}).then(show,function(){
  $("msg").textContent="Couldn't reach the Remote. Reload to try again.";
});
})();
</script></body></html>
)HTML";
