# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Campaign-owned persisted rhythm, RNG, daily crash claims and in-flight action state.

Browser restarts consume this state; they never recreate the timeline. Wall-clock
phase boundaries survive restart; monotonic exposure remains controller-owned.
"""
import random
from common import save
def tuples(value):return tuple(map(tuples,value)) if isinstance(value,list) else value
def restart_decision(age,day,profile_day):
    return dict(due=age>=28800 or day>profile_day,fresh=day>profile_day,profile_day=day)
class Rhythm:
    def __init__(self,path,seed,origin,clock):
        self.path=path;self.clock=clock;self.rng=random.Random(seed)
        self.data=__import__('json').loads(path.read_text()) if path.exists() else dict(origin=origin,seed=seed,phase=None,block=0,pages=0,night_day=-1,crash_day=-1,suite_block=-1,action=None)
        assert self.data['origin']==origin and self.data['seed']==seed,'campaign identity mismatch'
        if 'rng' in self.data:self.rng.setstate(tuples(self.data['rng']))
    def flush(self):self.data['rng']=self.rng.getstate();save(self.path,self.data)
    def day(self):return int((self.clock()-self.data['origin'])//86400)
    def phase(self):
        s=self.data;t=self.clock();day=self.day()
        if s['phase'] and t<s['until']:return s['phase']
        if s['phase']=='active':
            night=(t>=s['origin']+day*86400+18*3600 and s['night_day']!=day)
            duration=self.rng.randint(6*3600,8*3600) if night else self.rng.randint(10*60,90*60)
            if night:s['night_day']=day
            s.update(phase='idle',until=t+duration,night=night,duration=duration,action=None)
        else:
            s.update(phase='active',until=t+self.rng.randint(45*60,120*60),night=False,block=s['block']+1,action=None)
            s['duration']=s['until']-t
        self.flush();return s['phase']
    def crash_due(self):
        day=self.day()
        if self.data['crash_day']==day:return False
        self.data['crash_day']=day;self.flush();return True
    def begin_action(self,name,**fields):self.data['action']=dict(name=name,started=self.clock(),**fields);self.flush()
    def finish_action(self):self.data['action']=None;self.flush()
    def suite_done(self):self.data['suite_block']=self.data['block'];self.flush()
