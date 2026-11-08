"""Check real transport/tool observations independently of JSON vs prose formatting."""
import re

def verify_weather(out):
    connects=re.findall(r'(?:^|[\r\n])\[Python/HTTPS\] 正在连接已授权主机：([^\r\n]+)',out)
    assert connects and connects[-1]=='api.open-meteo.com',out
    phases=list(re.finditer(r'(?:^|[\r\n])\[Python/HTTPS\] 已收到HTTP 200，正在读取有界响应。',out))
    assert len(phases)>=2,out
    tail=out[phases[-1].end():]
    parts=re.split(r'\| \[模型\] 正在请求(?:本地|模型)服务。',tail,maxsplit=1)
    actual=parts[0];separator=len(parts)==2;answer=parts[1] if separator else ''
    assert separator and '[任务结束] status=observed' in answer,out
    stamp=re.search(r'20\d\d-\d\d-\d\dT\d\d:\d\d',actual)
    temp=re.search(r'(?:"temperature(?:_2m)?"\s*:\s*|温度\s*[:：]\s*|temperature\s*[:=]\s*)(-?\d+(?:\.\d+)?)',actual)
    assert stamp and temp,out
    # Final answer must carry the measured time/temperature, units and source.
    assert stamp.group() in answer or stamp.group().replace('T',' ') in answer,answer
    value=float(temp.group(1))
    assert re.search(r'(?<![\d.])'+re.escape(str(value))+r'\s*°C',answer) or re.search(r'(?<![\d.])'+re.escape(format(value,'g'))+r'\s*°C',answer),answer
    assert '上海' in answer and 'open-meteo' in answer.lower(),answer
    return {'time':stamp.group(),'temperature_c':value}

if __name__=='__main__':
    prefix='[Python/HTTPS] 正在连接已授权主机：geocoding-api.open-meteo.com\n[Python/HTTPS] 已收到HTTP 200，正在读取有界响应。\n[Python/HTTPS] 正在连接已授权主机：api.open-meteo.com\n[Python/HTTPS] 已收到HTTP 200，正在读取有界响应。\n'
    suffix='| [模型] 正在请求本地服务。\n| 上海 2026-11-06 14:30 温度19.1°C 来源open-meteo\n[任务结束] status=observed;\n'
    for actual in ['| 数据时间: 2026-11-06T14:30\n| 温度: 19.1 °C\n','| {"time":"2026-11-06T14:30","temperature":19.1}\n']:
        assert verify_weather(prefix+actual+suffix)['temperature_c']==19.1
    assert verify_weather(prefix+'| {"time":"2026-11-06T14:30","temperature":19.1}\n'+suffix.replace('2026-11-06 14:30','2026-11-06T14:30+08:00'))['temperature_c']==19.1
    good=prefix+'| 数据时间: 2026-11-06T14:30\n| 温度: 19.1 °C\n'+suffix
    for bad in [suffix, prefix+suffix, good.replace('温度19.1','温度99.0'),good.replace('status=observed','status=stopped'),good.replace('2026-11-06 14:30','2026-11-06 15:00')]:
        try:verify_weather(bad)
        except AssertionError:pass
        else:raise AssertionError('accepted unsupported/mismatched weather')
    print('PASS JSON/prose actual weather observations; reject no transport, no actual data, mismatched final temperature/time and stopped completion')
