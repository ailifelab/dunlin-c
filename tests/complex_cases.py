"""Deterministic cross-platform programming tasks and independent result oracles."""
import json
from decimal import Decimal

def case(name,round_no):
 if name=='money':
  rows=[{'region':' East ','price':'12.50','qty':2},{'region':'WEST','price':'0.35','qty':3},{'region':'east','price':'7.05','qty':round_no+1},{'region':'west','price':'oops','qty':2},{'region':'east','price':'1.00','qty':-1},{'region':' west ','price':'2','qty':4},{'region':'north','qty':1},{'region':'north','price':'1.20','qty':3}]
  tuples=[[r['region'],r.get('price'),r['qty']] for r in rows]
  initial='rows='+repr(tuples).replace(', ', ',')+';print(sum(int(p)*q for g,p,q in rows))'
  totals={};accepted=rejected=0
  for r in rows:
   try:
    n=Decimal(r['price'])*100
    if n!=n.to_integral_value() or n<0 or r['qty']<=0:raise ValueError()
    k=r['region'].strip().lower();totals[k]=totals.get(k,0)+int(n)*r['qty'];accepted+=1
   except Exception:rejected+=1
  expected={'totals_cents':totals,'accepted':accepted,'rejected':rejected}
  spec='按region去空格转小写分组，以整数分精确求price乘qty之和；price是非负金额字符串（整数或1到2位小数），qty须正整数，非法/缺字段记录跳过。输出JSON对象，字段totals_cents、accepted、rejected。不要使用浮点金额或硬编码结果。'
 else:
  graphs=[{'compile':[],'build':['compile'],'flash':['build'],'test':['build'],'release':['flash','test']},{'a':['b'],'b':['a']},{'ship':['absent']}]
  if round_no==2:graphs[0]['docs']=[];graphs[0]['release'].append('docs')
  initial='graphs='+repr(graphs).replace(' ','')+';print(graphs[0]["root"])'
  g=graphs[0];done=[]
  while len(done)<len(g):done.append(sorted(k for k,v in g.items() if k not in done and all(x in done for x in v))[0])
  expected={'order':done,'cycle':True,'missing':['absent']}
  spec='对graphs[0]作拓扑排序，每次选择所有可执行节点中字典序最小的一个；识别graphs[1]有环，找出graphs[2]引用但未定义的依赖。输出JSON对象，字段order（排序列表）、cycle（布尔）、missing（排序列表）。算法须通用，不要硬编码结果；用测试验证正常、有环和缺依赖三类输入。'
 if name=='money':
  prompt='Run initial unchanged, observe error; fix and rerun. Trim/lower group; exact cents*positive qty; reject bad price/qty. Print JSON keys totals_cents,accepted,rejected. Initial: '+initial
 else:
  prompt='Run initial unchanged, observe error; fix and rerun. graphs[0]: lexicographically smallest topo order; [1]: cycle detection; [2]: undefined deps. Print JSON order,cycle,missing. Initial: '+initial
 assert len(prompt.encode())<=383,(name,len(prompt.encode()))
 return {'name':name,'round':round_no,'initial':initial,'expected':expected,'prompt':prompt}

def result_matches(text,expected):
 decoder=json.JSONDecoder()
 for i,c in enumerate(text):
  if c!='{':continue
  try:obj,_=decoder.raw_decode(text[i:])
  except ValueError:continue
  if obj==expected:return True
 return False
