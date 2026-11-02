import os,signal,socket,subprocess,pathlib
root=pathlib.Path.cwd()
def port():
 with socket.socket() as s:
  s.bind(('127.0.0.1',0)); return s.getsockname()[1]
for build in ['build-em','build-em-profile']:
 http,debug=port(),port(); procs=[]
 with open('/tmp/compilation-boundary-chrome-'+build+'.log','w') as log:
  try:
   procs.append(subprocess.Popen(['python3','-m','http.server',str(http),'--bind','127.0.0.1','-d',str(root/build/'examples/browser/dist')],stdout=log,stderr=log,start_new_session=True))
   procs.append(subprocess.Popen(['/usr/bin/google-chrome','--headless=new','--no-sandbox','--disable-gpu',f'--remote-debugging-port={debug}',f'--user-data-dir=/tmp/compilation-boundary-chrome-profile-{build}','about:blank'],stdout=log,stderr=log,start_new_session=True))
   subprocess.run(['node','examples/browser/verify_chrome.js',f'http://127.0.0.1:{http}/index.html?auto=1',str(debug)],check=True,timeout=150)
  finally:
   for p in procs:
    try: os.killpg(p.pid,signal.SIGTERM)
    except ProcessLookupError: pass
   for p in procs: p.wait(timeout=10)
 subprocess.run(['node','examples/browser/verify_browser.mjs','/usr/bin/firefox',str(root/build/'examples/browser/dist')],check=True,timeout=150)
