"""Real native CLI Responses tests. No automatic approvals, shell or writes by model."""
import json,os,sys,time,subprocess,uuid
from pathlib import Path
binary=Path(sys.argv[1]).resolve();run_id=uuid.uuid4().hex[:10];root=(Path('build/openai-sol-state/native')/run_id).resolve();report=Path('reports/openai-sol')/('native-'+run_id);report.mkdir(parents=True,exist_ok=True)
key=os.environ['DUNLIN_OPENAI_SOL_KEY'] if '--key-env' in sys.argv[2:] else Path('dunlin-trial/openai-sol/api-key').read_text().strip();rows=[]
for effort in __import__('os').environ.get('DUNLIN_SOL_EFFORT_ORDER','low,xhigh').split(','):
 base=root/effort;base.mkdir(parents=True,exist_ok=True,mode=0o700)
 for name in ['work','sessions','home']:(base/name).mkdir(exist_ok=True,mode=0o700)
 (base/'work/fixture.txt').write_text('DUNLIN_OPENAI_READ_SOL61\n')
 cfg={'base_url':'https://api.openai.com/v1','model':'gpt-6.1-sol','protocol':'responses','key_env':'DUNLIN_OPENAI_SOL_KEY','stream':False,'show_reasoning':False,'max_tokens':4096,'reasoning_effort':effort,'reasoning_capabilities':['levels','xhigh'],'workspace':'work','session_dir':'sessions','enabled_tools':['echo','calc','read_file'],'timeout_ms':30000,'task_timeout_ms':120000,'max_rounds':5,'max_context_bytes':131072,'max_message_bytes':65536,'max_response_bytes':262144}
 path=base/'config.json';path.write_text(json.dumps(cfg,indent=2)+'\n');path.chmod(0o600);env=os.environ.copy();env['DUNLIN_OPENAI_SOL_KEY']=key;env['HOME']=str(base/'home')
 cases=[('hello','run','请用一句中文问候，不要调用工具。',None),('calc','run','请调用calc计算961乘47，然后报告实际结果。','45167'),('calc','resume','把刚才的结果加8，必须调用calc核对并报告实际结果。','45175'),('read','run','请调用read_file读取fixture.txt，原样报告其中内容。','DUNLIN_OPENAI_READ_SOL61')]
 for index,(session,action,prompt,expected) in enumerate(cases):
  cmd=[str(binary),'--config',str(path),'--session',session,'--events',action,prompt];started=time.monotonic();p=subprocess.run(cmd,env=env,capture_output=True,text=True);elapsed=time.monotonic()-started
  output=p.stdout.replace(key,'[REDACTED_KEY]');error=p.stderr.replace(key,'[REDACTED_KEY]');label=effort+'-'+str(index)+'-'+action; (report/(label+'.log')).write_text(output+'\nSTDERR\n'+error)
  events=[]
  for line in output.splitlines():
   try:events.append(json.loads(line))
   except ValueError:pass
  checkpoint=base/'sessions'/ (session+'.json');messages=json.loads(checkpoint.read_text()).get('messages',[]) if checkpoint.exists() else []
  tool_results=[{'id':m.get('tool_call_id'),'content':m.get('content')} for m in messages if m.get('role')=='tool'];calls=[c for m in messages for c in m.get('tool_calls',[])];metrics=[]
  for v in events:
   if v.get('event')=='model.metrics':
    data=v['data'];metrics.append(json.loads(data) if isinstance(data,str) else data)
  final=next((v for v in reversed(events) if 'status' in v),{})
  passed=p.returncode==0 and bool(final.get('text'))
  if expected:
   ids={e['data'] for e in events if e.get('event')=='tool.start'};names={c['id']:c['function']['name'] for c in calls};wanted='read_file' if session=='read' else 'calc'
   passed=passed and expected in final.get('text','') and any(v['id'] in ids and names.get(v['id'])==wanted and expected in str(v['content']) for v in tool_results)
  row={'effort':effort,'case':label,'action':action,'exit_code':p.returncode,'elapsed_s':round(elapsed,3),'status':'pass' if passed else 'fail','final':final,'tool_calls':calls,'tool_results':tool_results,'metrics':metrics,'model_requests':len(metrics),'log':str(report/(label+'.log'))};rows.append(row)
  (report/'results.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n');print(json.dumps({'case':label,'status':row['status'],'elapsed_s':row['elapsed_s'],'exit_code':p.returncode,'model_requests':len(metrics)},ensure_ascii=False),flush=True)
print('All cases passed:',all(x['status']=='pass' for x in rows),flush=True)
print("REPORT="+str(report),flush=True)
raise SystemExit(0 if all(x['status']=='pass' for x in rows) else 1)
