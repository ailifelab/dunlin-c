"""Actual bounded S3 code generation and error repair. No credentials in evidence."""
from pathlib import Path
import json,re,time,uuid,os
from complex_cases import case,result_matches
exec(compile(Path('tests/embedded/openai_sol_live.py').read_text().split('ok=False\n')[0],'serial-helper','exec'))
rows=json.loads(Path('reports/sol-complex/device-results.json').read_text()) if os.environ.get('DUNLIN_COMPLEX_RESUME')=='1' else []
try:
 command('/model status');command('/python status');command('/status')
 for round_no in [1,2]:
  for effort in (['low','xhigh'] if round_no==1 else ['xhigh','low']):
   command('/effort '+effort)
   for name in ['money','graph']:
    if any(r['round']==round_no and r['effort']==effort and r['case']==name for r in rows):continue
    spec=case(name,round_no)
    # Data+complete script go directly in user text; no filesystem on S3.
    out,elapsed=command(spec['prompt']);blocks=re.split(r'\| \[模型\] 正在请求模型服务。',out)
    actual=[]
    for block in blocks:
     if '[Python] 正在执行MicroPython代码。' in block:actual.append(block.split('[Python] 正在执行MicroPython代码。',1)[1])
    has_error=any(('ValueError' in x if name=='money' else 'KeyError' in x) for x in actual)
    has_success=any(result_matches(x,spec['expected']) for x in actual)
    passed=has_error and has_success and 'status=observed' in out and len(actual)>=2
    metric=re.search(r'\[OpenAI metrics\] ([^\r\n]+)',out)
    row={'platform':'ESP32-S3','round':round_no,'effort':effort,'case':name,'status':'pass' if passed else 'fail','wall_s':elapsed,'review_wait_s':0,'active_s':elapsed,'expected':spec['expected'],'observed_error':has_error,'observed_result':has_success,'python_runs':len(actual),'metrics':metric.group(1) if metric else None,'log':str(report/(str(sequence)+'.log'))};rows.append(row)
    Path('reports/sol-complex/device-results.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n');print(json.dumps(row,ensure_ascii=False),flush=True)
    command('/status')
 command('/effort low');command('/led off');command('/model status')
 Path('reports/sol-complex/device-report.json').write_text(json.dumps({'report':str(report),'status':'pass' if all(r['status']=='pass' for r in rows) else 'fail'},indent=2)+'\n')
finally:s.close()
raise SystemExit(0 if len(rows)==8 and all(r['status']=='pass' for r in rows) else 1)
