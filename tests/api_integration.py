#!/usr/bin/env python3
"""Real isolated HTTP/storage/auth integration tests. No cloud credentials needed."""
import argparse,json,os,socket,sqlite3,subprocess,tempfile,time,urllib.request,urllib.error,urllib.parse
from pathlib import Path

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--server',type=Path,required=True);parser.add_argument('--client-test',type=Path);args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='glass-v3-') as tmp:
        tmp=Path(tmp);database=tmp/'music.sqlite3';server=None
        with socket.socket() as sock: sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        origin=f'http://127.0.0.1:{port}'
        def call(method,path,data=None,token=None,expected=200,raw=False,headers=None):
            hdr=dict(headers or {})
            if token:hdr['Authorization']='Bearer '+token
            if data is not None:hdr['Content-Type']='application/octet-stream' if isinstance(data,bytes) else 'application/json'
            body=data if isinstance(data,bytes) else json.dumps(data).encode() if data is not None else None
            try:r=urllib.request.urlopen(urllib.request.Request(origin+path,data=body,headers=hdr,method=method),timeout=100)
            except urllib.error.HTTPError as e:r=e
            content=r.read();assert r.status==expected,(method,path,r.status,content[:300])
            return (content,r.headers) if raw else json.loads(content) if content else None
        def start():
            nonlocal server
            server=subprocess.Popen([str(args.server.resolve()),'--db',str(database),'--assets',str(root/'assets'),'--port',str(port)],stdout=log,stderr=log)
            for _ in range(100):
                if server.poll() is not None:raise AssertionError('Server failed to start')
                try:
                    if call('GET','/api/v1/health')['status']=='ok':return
                except OSError:time.sleep(.05)
            raise AssertionError('Server timeout')
        def stop():
            if server and server.poll() is None:server.terminate();server.wait(timeout=10)
        with (tmp/'server.log').open('w+') as log:
            try:
                start()
                assert len(call('GET','/api/v1/onboarding')['slides'])==3
                for path in ['/home','/tracks','/tracks/1/stream','/library']:call('GET','/api/v1'+path,expected=401)
                account={'name':'Listener','email':'listener@example.test','password':'Test-Listener-Password-123'}
                listener=call('POST','/api/v1/auth/register',account,expected=201)['token']
                assert call('GET','/api/v1/tracks',token=listener)['tracks']==[]
                assert call('GET','/api/v1/library',token=listener)=={'likes':[],'playlists':[]}
                admin_account={'name':'Administrator','email':'admin@example.test','password':'Test-Admin-Password-123','role':'admin'}
                assert call('POST','/api/v1/auth/register',admin_account,expected=201)['user']['role']=='listener'
                subprocess.run([str(args.server.resolve()),'--db',str(database),'--promote-admin',admin_account['email']],check=True,capture_output=True)
                admin=call('POST','/api/v1/auth/login',admin_account)['token']
                call('GET','/api/v1/admin/storages',token=listener,expected=403)
                call('POST','/api/v1/admin/storages',{'name':'bad','provider':'local','config':{}},listener,expected=403)
                local=call('POST','/api/v1/admin/storages',{'name':'Second library','provider':'local','config':{'password':'SecretMustBeEncrypted'}},admin,expected=201)['storages'][-1]
                assert local['name']=='Second library' and 'config' not in local
                second=local['id'];call('POST',f'/api/v1/admin/storages/{second}',token=admin)
                call('PATCH',f'/api/v1/admin/storages/{second}',{'name':'Studio library'},admin)
                bad={'name':'Bad cluster','provider':'mongodb','config':{'host':'invalid/host','username':'testuser','password':'MustNeverEcho123','database':'glass'}}
                response=call('POST','/api/v1/admin/storages',bad,admin,expected=400)
                assert 'MustNeverEcho123' not in json.dumps(response)
                wav=(root/'assets/aurora.wav').read_bytes()
                uploaded=[]
                for storage in [1,second]:
                    path='/api/v1/admin/tracks?'+urllib.parse.urlencode({'title':'Song '+str(storage),'artist':'Test artist','album':'Test album','genre':'Ambient','storageId':storage})
                    call('POST',path,wav,listener,expected=403)
                    track=call('POST',path,wav,admin,expected=201);assert track['duration']==16;uploaded.append(track['id'])
                tracks=call('GET','/api/v1/tracks',token=listener)['tracks'];assert len(tracks)==2
                assert {t['storageName'] for t in tracks}=={'Local library','Studio library'}
                stream=tracks[0]['streamPath'];content,headers=call('GET',stream,raw=True);assert content==wav
                assert headers['Cache-Control']=='private, no-store'
                assert call('GET',stream,headers={'Range':'bytes=10-59'},expected=206,raw=True)[0]==wav[10:60]
                assert call('HEAD',stream,raw=True)[0]==b''
                call('GET',stream.replace('t=','t=x'),expected=401)
                call('DELETE',f'/api/v1/admin/storages/{second}',token=admin,expected=409)
                with sqlite3.connect(database) as db:
                    encrypted=db.execute('SELECT config FROM storages WHERE id=?',(second,)).fetchone()[0]
                    assert 'SecretMustBeEncrypted' not in encrypted and 'directory' not in encrypted
                    db.execute('DELETE FROM tracks WHERE storage_id=?',(second,))
                result=call('POST','/api/v1/catalog/refresh',token=listener);assert result['added']==1 and result['errors']==[]
                assert len(call('GET','/api/v1/tracks',token=listener)['tracks'])==2
                playlist=call('POST','/api/v1/playlists',{'name':'My own mix'},listener,expected=201)['playlists'][0]['id']
                call('PUT',f'/api/v1/playlists/{playlist}/tracks/{uploaded[0]}',token=listener)
                assert call('PATCH',f'/api/v1/playlists/{playlist}',{'pinned':True},listener)['playlists'][0]['pinned']==1
                call('DELETE',f'/api/v1/playlists/{playlist}',token=admin,expected=404)
                call('PUT',f'/api/v1/likes/{uploaded[0]}',token=listener)
                prefs={'name':'Updated listener','bio':'Music is life','backgroundPlay':False,'reducedMotion':True}
                assert call('PATCH','/api/v1/me',prefs,listener)['user']['backgroundPlay'] is False
                call('PUT','/api/v1/admin/theme',{'backgroundColor':'#503890'},admin)
                slides=[{'title':'Welcome to our music','body':'Saved by an administrator.','color':'#4838af'}]
                call('PUT','/api/v1/admin/onboarding',{'slides':slides},listener,expected=403)
                call('PUT','/api/v1/admin/onboarding',{'slides':slides},admin)
                assert call('GET','/api/v1/onboarding')['slides']==slides
                stop();start()
                assert call('GET','/api/v1/home',token=listener)['backgroundColor']=='#503890'
                assert call('GET','/api/v1/library',token=listener)['playlists'][0]['pinned']==1
                assert call('GET','/api/v1/me',token=listener)['user']['bio']=='Music is life'
                call('POST','/api/v1/auth/logout',token=listener)
                call('GET',stream,expected=401)
                call('GET','/api/v1/tracks',token=listener,expected=401)
                assert (tmp/'storage.key').stat().st_mode & 0o077==0
                if args.client_test:
                    env={**os.environ,'GLASS_API_URL':origin,'QT_QPA_PLATFORM':os.environ.get('GLASS_TEST_QPA_PLATFORM','offscreen'),'GLASS_TEST_ADMIN_EMAIL':admin_account['email'],'GLASS_TEST_ADMIN_PASSWORD':admin_account['password']}
                    subprocess.run([str(args.client_test.resolve())],env=env,check=True,timeout=180)
                print('PASS: mandatory auth, empty initial library, encrypted reusable storage, two storage uploads, duration, sync/import, protected range streaming, pins, profiles, welcome editor and restart persistence')
            finally:
                stop()
                log.seek(0)
                logs=log.read()
                assert 'MustNeverEcho123' not in logs and 'SecretMustBeEncrypted' not in logs
if __name__=='__main__':main()
