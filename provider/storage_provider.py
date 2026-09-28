#!/usr/bin/env python3
"""Storage adapters. Credentials arrive on stdin; stdout contains sanitized JSON only.
All object keys are server generated. Scans include only Glass Music managed audio.
"""
import json, os, sys, shutil, uuid, math, wave
from pathlib import Path
from urllib.parse import quote, urlsplit
MAX_BYTES = 32 * 1024 * 1024
class StorageError(Exception): pass

def duration(path):
    try:
        if str(path).lower().endswith('.wav'):
            with wave.open(str(path)) as f: seconds = f.getnframes() / f.getframerate()
        else:
            from mutagen import File
            f = File(path)
            seconds = f.info.length if f and f.info else 0
        if not math.isfinite(seconds) or seconds <= 0: raise ValueError()
        return max(1, round(seconds))
    except Exception: raise StorageError('The audio file cannot be decoded or has no valid duration.')

def google_credentials(config, scope):
    import google.auth
    data = json.loads(config['credentials'])
    if data.get('type') not in ('service_account','authorized_user'):
        raise StorageError('Use a service-account or authorized-user credentials JSON document.')
    return google.auth.load_credentials_from_dict(data, scopes=[scope])[0]

def adapter(provider, config):
    if provider == 'local': return Local(config)
    if provider == 'mongodb': return Mongo(config)
    if provider == 'firebase': return Firebase(config)
    if provider == 'gdrive': return Drive(config)
    if provider == 's3': return S3(config)
    raise StorageError('Unsupported storage provider.')

class Local:
    def __init__(self,c): self.root=Path(c['directory']).resolve(); self.root.mkdir(parents=True,exist_ok=True)
    def test(self):
        if not os.access(self.root,os.R_OK|os.W_OK): raise StorageError('Storage directory is not writable.')
    def upload(self,path,key,meta):
        try:
            shutil.copyfile(path,self.root/key)
            (self.root/(key+'.json')).write_text(json.dumps(meta))
            return key
        except Exception:
            self.delete(key)
            raise
    def download(self,key,path): shutil.copyfile(self.root/key,path)
    def delete(self,key):
        (self.root/key).unlink(missing_ok=True); (self.root/(key+'.json')).unlink(missing_ok=True)
    def scan(self):
        return [{'key':f.name[:-5],**json.loads(f.read_text())} for f in sorted(self.root.glob('*.json'))[:1000] if (self.root/f.name[:-5]).is_file()]

class Mongo:
    def __init__(self,c):
        from pymongo import MongoClient
        import gridfs
        host=c['host'].strip()
        if not host or any(x in host for x in ['/', '@', '?', '#']): raise StorageError('Enter the SRV hostname only, such as cluster0.example.mongodb.net.')
        uri='mongodb+srv://'+quote(c['username'],safe='')+':'+quote(c['password'],safe='')+'@'+host+'/?retryWrites=true&w=majority&appName='+quote(c.get('appName','GlassMusic'),safe='')
        self.client=MongoClient(uri,serverSelectionTimeoutMS=10000,connectTimeoutMS=10000,socketTimeoutMS=30000)
        self.db=self.client[c.get('database','glass_music')]; self.bucket=gridfs.GridFSBucket(self.db,bucket_name='glass_audio')
    def test(self): self.client.admin.command('ping'); self.db.list_collection_names()
    def upload(self,path,key,meta):
        with open(path,'rb') as f: self.bucket.upload_from_stream_with_id(key,key,f,metadata={'glassMusic':True,**meta})
        return key
    def download(self,key,path):
        with open(path,'wb') as f: self.bucket.download_to_stream(key,f)
    def delete(self,key): self.bucket.delete(key)
    def scan(self):
        return [{'key':str(f['_id']),**f['metadata']} for f in self.db.glass_audio.files.find({'metadata.glassMusic':True}).limit(1000)]

class Firebase:
    def __init__(self,c):
        from google.cloud import storage
        self.bucket=storage.Client(project=c['projectId'],credentials=google_credentials(c,'https://www.googleapis.com/auth/devstorage.read_write')).bucket(c['bucket'])
    def test(self): self.bucket.reload(timeout=15)
    def upload(self,path,key,meta):
        obj=self.bucket.blob('glass-music/'+key); obj.metadata={'glassMusic':json.dumps(meta)}
        obj.upload_from_filename(path,timeout=45,content_type=meta['mime']); return key
    def download(self,key,path): self.bucket.blob('glass-music/'+key).download_to_filename(path,timeout=45)
    def delete(self,key): self.bucket.blob('glass-music/'+key).delete(timeout=15)
    def scan(self):
        return [{'key':b.name.split('/')[-1],**json.loads(b.metadata['glassMusic'])} for b in self.bucket.list_blobs(prefix='glass-music/',max_results=1000,timeout=20) if b.metadata and b.metadata.get('glassMusic')]

class S3:
    def __init__(self,c):
        import boto3
        from botocore.config import Config
        endpoint=c.get('endpoint','').strip()
        if endpoint and (urlsplit(endpoint).scheme!='https' or urlsplit(endpoint).username): raise StorageError('S3 endpoints must be HTTPS origins without embedded credentials.')
        self.client=boto3.client('s3',endpoint_url=endpoint or None,region_name=c.get('region','auto'),aws_access_key_id=c['accessKey'],aws_secret_access_key=c['secretKey'],config=Config(connect_timeout=10,read_timeout=30,retries={'max_attempts':1}))
        self.bucket=c['bucket']
    def test(self): self.client.head_bucket(Bucket=self.bucket)
    def upload(self,path,key,meta):
        self.client.upload_file(str(path),self.bucket,'glass-music/'+key,ExtraArgs={'ContentType':meta['mime'],'Metadata':{'glass-music':json.dumps(meta,ensure_ascii=True)}}); return key
    def download(self,key,path): self.client.download_file(self.bucket,'glass-music/'+key,str(path))
    def delete(self,key): self.client.delete_object(Bucket=self.bucket,Key='glass-music/'+key)
    def scan(self):
        rows=[]
        for obj in self.client.list_objects_v2(Bucket=self.bucket,Prefix='glass-music/',MaxKeys=1000).get('Contents',[]):
            meta=self.client.head_object(Bucket=self.bucket,Key=obj['Key']).get('Metadata',{}).get('glass-music')
            if meta: rows.append({'key':obj['Key'].split('/')[-1],**json.loads(meta)})
        return rows

class Drive:
    def __init__(self,c):
        from googleapiclient.discovery import build
        self.api=build('drive','v3',credentials=google_credentials(c,'https://www.googleapis.com/auth/drive'),cache_discovery=False)
        self.folder=c['folderId']
    def test(self):
        f=self.api.files().get(fileId=self.folder,supportsAllDrives=True,fields='id,mimeType,capabilities(canAddChildren)').execute()
        if f.get('mimeType')!='application/vnd.google-apps.folder' or not f.get('capabilities',{}).get('canAddChildren'): raise StorageError('Drive folder is not writable. Share it with the configured account.')
    def upload(self,path,key,meta):
        from googleapiclient.http import MediaFileUpload
        return self.api.files().create(body={'name':key,'parents':[self.folder],'description':json.dumps(meta),'appProperties':{'glassMusic':'1'}},media_body=MediaFileUpload(str(path),mimetype=meta['mime'],resumable=True),supportsAllDrives=True,fields='id').execute()['id']
    def download(self,key,path):
        from googleapiclient.http import MediaIoBaseDownload
        with open(path,'wb') as f:
            download=MediaIoBaseDownload(f,self.api.files().get_media(fileId=key,supportsAllDrives=True));done=False
            while not done: _,done=download.next_chunk()
    def delete(self,key): self.api.files().delete(fileId=key,supportsAllDrives=True).execute()
    def scan(self):
        folder=self.folder.replace("'", "\\'")
        files=self.api.files().list(q="'"+folder+"' in parents and trashed=false and appProperties has { key='glassMusic' and value='1' }",fields='files(id,name,description)',pageSize=1000,supportsAllDrives=True,includeItemsFromAllDrives=True).execute().get('files',[])
        return [{'key':f['id'],**json.loads(f['description'])} for f in files if f.get('description')]

def main():
    try:
        data=json.loads(sys.stdin.read(65536));action=data['action']
        if action=='probe': result={'duration':duration(data['path'])}
        else:
            obj=adapter(data['provider'],data['config'])
            if action=='test': obj.test(); result={'ok':True}
            elif action=='upload': result={'key':obj.upload(data['path'],data['key'],data['metadata'])}
            elif action=='download':
                obj.download(data['key'],data['path'])
                if Path(data['path']).stat().st_size>MAX_BYTES: Path(data['path']).unlink();raise StorageError('Stored audio exceeds the upload limit.')
                result={'ok':True}
            elif action=='delete': obj.delete(data['key']);result={'ok':True}
            elif action=='scan': result={'tracks':obj.scan()}
            else: raise StorageError('Unsupported storage operation.')
        print(json.dumps(result))
    except StorageError as e: print(json.dumps({'error':str(e)}))
    except ImportError: print(json.dumps({'error':'Storage dependencies are missing. Run scripts/setup-storage.sh on the server.'}))
    except Exception:
        # Driver exceptions may contain credentials or connection strings. Never forward them.
        print(json.dumps({'error':'Storage operation failed. Check credentials, network access, bucket/folder permissions and provider limits.'}))
if __name__=='__main__': main()
