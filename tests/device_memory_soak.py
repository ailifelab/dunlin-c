"""Bounded actual MCU script errors/OOM/timeouts with post-job free-heap samples."""
from pathlib import Path
import json,re,time
exec(compile(Path('tests/embedded/openai_sol_live.py').read_text().split('ok=False\n')[0],'serial-helper','exec'))
rows=[]
def heap():
 out,_=command('/status');m=re.search(r'(?:heap_free|free heap)[=: ]+(\d+)',out);assert m,out;return int(m.group(1))
try:
 # Warm all intentional exception handlers before reference sample; keep raw warmup logs.
 scripts=["a=[str(i) for i in range(2000)];print(len(a))","raise ValueError('soak-expected')","a='x'*2000000","while True: pass"]
 for code in scripts:
  out,_=command('/python run '+code);assert '[Python结束]' in out
 baseline=heap()
 for i in range(40):
  kind=i%4;out,elapsed=command('/python run '+scripts[kind]);expected=['2000','ValueError','MemoryError','timeout'][kind];assert expected in out,out
  recovered,_=command('/python run print(7*9)');assert '| 63' in recovered,recovered
  sample={'index':i,'case':kind,'elapsed_s':elapsed,'heap_free':heap(),'recovered':True};rows.append(sample);Path('reports/sol-complex/device-memory.json').write_text(json.dumps({'baseline':baseline,'samples':rows},indent=2)+'\n')
 command('/led off');command('/effort low');command('/python status');print('PASS 40 workload + 40 recovery executions; baseline='+str(baseline)+' final='+str(rows[-1]['heap_free']))
finally:s.close()
