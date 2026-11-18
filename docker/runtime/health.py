#!/usr/bin/env python3
"""Readiness is world/native progress, never a socket or running process."""
import json
import os
from pathlib import Path
import re
import time


def observe():
    state=Path(os.environ.get('X4_STATE_DIR','/state')).resolve()
    run=json.loads((state/'current-run.json').read_text())
    results=Path(run['results']).resolve()
    if not results.is_relative_to(state/'logs'): raise ValueError('invalid results')
    profile=state/'.config/EgoSoft/X4'
    record=results/'health.json'
    old=json.loads(record.read_text()) if record.exists() else {}
    answer={'checked_at':time.time(),'ready':False,'reason':'waiting for real world/native progression'}
    log=profile/'host.log'; perf=results/'x4mp-perf.log'
    if log.exists() and perf.exists():
        samples=[line for line in log.read_text(errors='replace').splitlines() if '[GPUFREE] sample game_time=' in line]
        native=perf.read_text(errors='replace').splitlines()
        if samples and native:
            sample=samples[-1]; values=dict(re.findall(r'(\w+)=([^ ]+)',sample)); now=time.time()
            world=float(values['game_time']); previous=old.get('world_time',world)
            seen=now if sample!=old.get('sample') else old.get('sample_seen',now)
            progressed=now if world>previous else old.get('progress_at',now)
            reset=old.get('world_reset',False) or world<previous
            answer.update(sample=sample,sample_seen=seen,world_time=world,progress_at=progressed,world_reset=reset)
            answer['ready']=(not reset and values.get('paused')=='false' and values.get('world_ready')=='true'
                and int(values.get('moved','0'))>0 and now-seen<60 and now-progressed<60
                and now-perf.stat().st_mtime<60 and native[-1].startswith('role=host '))
            if answer['ready']: answer['reason']='world, sampled NPCs and native host progressing'
    pending=record.with_suffix('.next'); pending.write_text(json.dumps(answer)+'\n'); pending.replace(record)
    return answer


if __name__=='__main__':
    try:
        answer=observe(); print(answer['reason']); raise SystemExit(0 if answer['ready'] else 1)
    except (OSError,ValueError,KeyError):
        print('host readiness not established'); raise SystemExit(1)
