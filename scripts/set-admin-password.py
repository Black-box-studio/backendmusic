#!/usr/bin/env python3
"""Locally create/update an administrator without putting passwords in argv."""
import argparse
import getpass
import hashlib
from pathlib import Path
import secrets
import sqlite3

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('email')
parser.add_argument('--db', type=Path, default=Path(__file__).resolve().parents[1] / '.runtime/music.sqlite3')
args = parser.parse_args()
if not args.db.is_file():
    parser.error('Start the server first to initialize the database.')
password = getpass.getpass('New password (10–128 characters): ')
if not 10 <= len(password) <= 128 or password != getpass.getpass('Confirm password: '):
    parser.error('Passwords must match and contain 10–128 characters.')
salt = secrets.token_hex(16)
derived = hashlib.scrypt(password.encode(), salt=salt.encode(), n=32768, r=8, p=3, maxmem=64*1024*1024, dklen=32).hex()
with sqlite3.connect(args.db) as db:
    db.execute("INSERT INTO users(email,name,salt,password_hash,role) VALUES(?,?,?,?, 'admin') ON CONFLICT(email) DO UPDATE SET salt=excluded.salt,password_hash=excluded.password_hash,role='admin'", (args.email.strip().lower(), 'Admin', salt, derived))
    db.execute('DELETE FROM sessions WHERE user_id=(SELECT id FROM users WHERE email=?)', (args.email.strip().lower(),))
print('Administrator password updated; existing sessions revoked.')
