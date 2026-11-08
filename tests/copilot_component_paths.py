#!/usr/bin/env python3
"""Static ESP-IDF source/dependency wiring; no hardware or firmware claim."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
components=root/'dunlin-copilot/esp32/esp32/components'
for name in ('dunlin_kernel','dunlin_embedded'):
 path=components/name/'CMakeLists.txt'
 assert path.is_file(), 'missing canonical component '+name
 text=path.read_text()
 declared=(path.parent/'../../../../..').resolve()
 assert declared==root
 for relative in re.findall(r'\$\{DUNLIN_ROOT\}/([^" ]+)',text):
  assert (root/relative).exists(), 'missing component source '+relative
print('PASS canonical ESP-IDF component directories, source roots and include paths exist; firmware unverified')

component=components/'micropython'
text=(component/'embed.mk').read_text()
vendor=re.search(r'^MICROPYTHON_TOP = (.+)$',text,re.M).group(1)
assert (component/vendor).resolve()==root/'build/vendor/micropython-1.26.1'
assert (component/'../../../python/mpbridge.c').resolve()==root/'dunlin-copilot/esp32/python/mpbridge.c'
assert (component/'../../../python/mpbridge.c').is_file()
prepare=(root/'scripts/prepare-micropython.sh').read_text()
assert 'component="$root/dunlin-copilot/esp32/esp32/components/micropython"' in prepare
assert 'MICROPYTHON_TOP = '+vendor in prepare
print('PASS MicroPython preparation and bridge references use canonical platform layout')
