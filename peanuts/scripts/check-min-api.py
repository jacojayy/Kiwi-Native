#!/usr/bin/env python3
"""Every platform method and field an APK's code calls, against the Android SDK's own table of when each arrived (api-versions.xml): prints what is newer
than the app's minimum, and exits 1 when anything is. A release check: 0.1.1 and 0.1.2 closed on Android 13 because one call (Matcher.appendTail with a
StringBuilder, API 34) was newer than minSdk 33, and the compiler does not say so.
    check-min-api.py <apk> [<min sdk, default 33>]
Needs the SDK (ANDROID_HOME or ANDROID_SDK_ROOT): a platform's data/api-versions.xml and a build-tools dexdump, the newest of each."""
import subprocess, sys, re, tempfile, zipfile, os, glob, shutil
import xml.etree.ElementTree as ET
if len(sys.argv) < 2: sys.exit(__doc__)
apk, minsdk = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 33
SDK = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or sys.exit('[RED] set ANDROID_HOME to the Android SDK')
def newest(pattern):
    found = sorted(glob.glob(os.path.join(SDK, pattern)), key=lambda p: [int(x) if x.isdigit() else 0 for x in re.split(r'[^0-9]+', p)])
    return found[-1] if found else sys.exit('[RED] not in the SDK: ' + pattern)
X = newest('platforms/android-*/data/api-versions.xml')
DEXDUMP = newest('build-tools/*/dexdump')
classes = {}
for c in ET.parse(X).getroot().iter('class'):
    cs = int(c.get('since', '1'))
    d = {'since': cs, 'supers': [e.get('name') for e in c if e.tag in ('extends', 'implements')], 'm': {}, 'f': {}}
    for e in c:
        if e.tag == 'method': d['m'][e.get('name')] = int(e.get('since', cs))
        elif e.tag == 'field': d['f'][e.get('name')] = int(e.get('since', cs))
    classes[c.get('name')] = d
def since(cls, kind, name, seen=None):
    """The lowest API level at which cls has this member, itself or inherited; None when unknown"""
    seen = seen or set()
    if cls in seen or cls not in classes: return None
    seen.add(cls); c = classes[cls]; best = None
    if name in c[kind]: best = max(c[kind][name], c['since'])
    for s in c['supers']:
        v = since(s, kind, name, seen)
        if v is not None:
            v = max(v, c['since'])
            best = v if best is None else min(best, v)
    return best
tmp = tempfile.mkdtemp()
with zipfile.ZipFile(apk) as z:
    for n in z.namelist():
        if re.fullmatch(r'classes\d*\.dex', n): z.extract(n, tmp)
hits = {}; cur = None; nref = 0
for dex in sorted(glob.glob(tmp + '/classes*.dex')):
    out = subprocess.run([DEXDUMP, '-d', dex], capture_output=True, text=True, errors='replace').stdout
    for line in out.splitlines():
        m = re.match(r"\s+Class descriptor\s+:\s+'L([^;]+);'", line)
        if m: cur = m.group(1); continue
        m = re.search(r'(invoke-[a-z/-]+|[is](?:get|put)[a-z-]*) .*?, L((?:java|javax|android|dalvik|org/json|org/xml)[^;]*);\.([^:]+):(\S+)', line)
        if not m: continue
        op, cls, name, sig = m.groups(); nref += 1
        kind = 'm' if op.startswith('invoke') else 'f'
        key = name + sig if kind == 'm' else name
        if cls not in classes:
            hits.setdefault(('?', cls, key), set()).add(cur); continue
        s = since(cls, kind, key)
        if s is None and kind == 'm' and name == '<init>': s = since(cls, kind, key)
        if s is None: hits.setdefault(('?', cls, key), set()).add(cur)
        elif s > minsdk: hits.setdefault((s, cls, key), set()).add(cur)
print('%d platform references checked against API %d' % (nref, minsdk))
unknown = [k for k in hits if k[0] == '?']
for k in sorted((k for k in hits if k[0] != '?'), key=lambda k: (-k[0], k[1])):
    print('API %d  %s.%s   used by %s' % (k[0], k[1], k[2], ', '.join(sorted(x for x in hits[k] if x)[:4])))
shutil.rmtree(tmp, ignore_errors=True)
newer = [k for k in hits if k[0] != '?']
print('%d references not in the table (hidden or desugared classes): %s' % (len(unknown), ', '.join(sorted({k[1] for k in unknown})[:12])))
for k in sorted(unknown, key=lambda k: k[1])[:25]: print('   ?  %s.%s   used by %s' % (k[1], k[2], ', '.join(sorted(x for x in hits[k] if x)[:2])))
if newer: sys.exit('[RED] %d platform reference(s) newer than API %d' % (len(newer), minsdk))
print('nothing newer than API %d' % minsdk)
