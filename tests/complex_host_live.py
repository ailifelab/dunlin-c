"""Real CLI coding benchmark. Every write/exec waits for an explicit external review."""
import os,sys,pty,select,subprocess,time,re,json,uuid,hashlib
from pathlib import Path
from complex_cases import case,result_matches
root=Path.cwd();report=root/'reports/sol-complex';queue=report/'requests';queue.mkdir(exist_ok=True)
platform=sys.argv[1];binary=Path(sys.argv[2]).resolve();python=sys.argv[3];state=root/'build'/('complex-'+uuid.uuid4().hex[:8]);state.mkdir();key=os.environ.get('DUNLIN_OPENAI_SOL_KEY') or (root/'dunlin-trial/openai-sol/api-key').read_text().strip();rows=[]
for round_no in [1,2]:
 for effort in (['low','xhigh'] if round_no==1 else ['xhigh','low']):
  for name in ['money','graph']:
   spec=case(name,round_no);job=platform+'-'+str(round_no)+'-'+effort+'-'+name;base=state/job
   for folder in ['work','sessions','home']:(base/folder).mkdir(parents=True,mode=0o700)
   (base/'work/task.py').write_text(spec['initial']+'\n')
   config={'base_url':'https://api.openai.com/v1','model':'gpt-6.1-sol','protocol':'responses','key_env':'DUNLIN_OPENAI_SOL_KEY','reasoning_effort':effort,'reasoning_capabilities':['levels','xhigh'],'stream':False,'show_reasoning':False,'workspace':'work','session_dir':'sessions','enabled_tools':['read_file','write_file','exec'],'max_tokens':4096,'max_rounds':12,'max_calls':4,'max_messages':40,'max_context_bytes':262144,'max_message_bytes':65536,'max_response_bytes':262144,'timeout_ms':60000,'task_timeout_ms':600000,'tool_timeout_ms':5000,'max_output_bytes':4096}
   cfg=base/'config.json';cfg.write_text(json.dumps(config,indent=2)+'\n');cfg.chmod(0o600)
   prompt=spec['prompt']+' Desktop: Read task.py, exec it unchanged first (executable='+python+', argv=["task.py"]). Then use write_file to replace task.py and exec it again. Only local computation; no other files, network, OS commands or subprocesses.'
   env=os.environ.copy();env.update(DUNLIN_OPENAI_SOL_KEY=key,HOME=str(base/'home'),TERM='dumb')
   master,slave=pty.openpty();events_path=report/(job+'-events.log');terminal=bytearray();review_wait=0.;approval=0;scan=0;begin=time.monotonic();sent=False;finished=False
   with events_path.open('wb') as events:
    p=subprocess.Popen([str(binary),'--config',str(cfg),'--session','task','--events','--permissions','strict','--approval-default','no','chat'],stdin=slave,stderr=slave,stdout=events,env=env);os.close(slave)
    try:
     while p.poll() is None and time.monotonic()-begin<650:
      if select.select([master],[],[],.05)[0]:
       try:b=os.read(master,65536)
       except OSError:break
       terminal.extend(b);(report/(job+'-terminal.log')).write_bytes(terminal)
      tail=bytes(terminal[scan:])
      if not sent and re.search(rb'(?:^|[\r\n])> $',tail):
       os.write(master,(prompt+'\n').encode());sent=True;scan=len(terminal);continue
      pattern=rb'Approval required for tool (\w+) with exactly these arguments\?\r?\n(.*?)\r?\nAllow once\?.*?Type yes to allow once: '
      m=re.search(pattern,tail,re.S)
      if m:
       approval+=1;identifier=job+'-'+str(approval);raw=m.group(2).decode('utf8');args=json.loads(raw);request={'id':identifier,'platform':platform,'round':round_no,'effort':effort,'case':name,'tool':m.group(1).decode(),'args':args,'work':str(base/'work'),'sha256':hashlib.sha256(raw.encode()).hexdigest()};(queue/(identifier+'.json')).write_text(json.dumps(request,ensure_ascii=False,indent=2)+'\n');print('REVIEW '+identifier,flush=True);waiting=time.monotonic();decision=queue/(identifier+'.decision')
       while not decision.exists() and p.poll() is None and time.monotonic()-begin<650:time.sleep(.1)
       if not decision.exists():raise TimeoutError('manual review not received')
       vote=json.loads(decision.read_text());assert vote['sha256']==request['sha256'] and vote['decision'] in ['yes','no']
       os.write(master,(vote['decision']+'\n').encode());review_wait+=time.monotonic()-waiting;scan=len(terminal);continue
      if sent and re.search(rb'(?:^|[\r\n])> $',tail):finished=True;break
     wall=time.monotonic()-begin
     if finished:
      os.write(master,b'/q\n');end=time.monotonic()+5
      while p.poll() is None and time.monotonic()<end:
       if select.select([master],[],[],.05)[0]:
        try:terminal.extend(os.read(master,65536))
        except OSError:break
      p.wait(timeout=2)
    finally:
     if p.poll() is None:p.kill();p.wait(timeout=3)
     os.close(master);(report/(job+'-terminal.log')).write_bytes(terminal)
   ev=[]
   for line in events_path.read_text().splitlines():
    try:ev.append(json.loads(line))
    except ValueError:pass
   checkpoint=base/'sessions/task.json';messages=json.loads(checkpoint.read_text()).get('messages',[]) if checkpoint.exists() else [];calls={c['id']:c['function'] for msg in messages for c in msg.get('tool_calls',[])};outputs=[{'id':msg.get('tool_call_id'),'content':msg.get('content',''),'function':calls.get(msg.get('tool_call_id'),{})} for msg in messages if msg.get('role')=='tool'];execs=[x for x in outputs if x['function'].get('name')=='exec'];has_error=any(('ValueError' if name=='money' else 'KeyError') in x['content'] for x in execs);success=any(result_matches(x['content'],spec['expected']) for x in execs);written=any(x['function'].get('name')=='write_file' and 'written' in x['content'] for x in outputs);final=next((e for e in reversed(ev) if 'status' in e),{});metrics=[json.loads(e['data']) if isinstance(e['data'],str) else e['data'] for e in ev if e.get('event')=='model.metrics'];passed=finished and p.returncode==0 and has_error and success and written and final.get('status')=='completed'
   row={'platform':platform,'round':round_no,'effort':effort,'case':name,'status':'pass' if passed else 'fail','wall_s':round(wall,3),'review_wait_s':round(review_wait,3),'active_s':round(wall-review_wait,3),'observed_error':has_error,'observed_result':success,'written':written,'exec_runs':len(execs),'approvals':approval,'metrics':metrics,'expected':spec['expected'],'final':final,'outputs':outputs,'events':str(events_path),'state':str(base)};rows.append(row);(report/(platform+'-results.json')).write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n');print('RESULT '+json.dumps({k:v for k,v in row.items() if k not in ['metrics','outputs','final','expected']}),flush=True)
print('DONE '+platform,flush=True)
raise SystemExit(0 if len(rows)==8 and all(x['status']=='pass' for x in rows) else 1)
