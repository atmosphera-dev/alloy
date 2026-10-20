# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Lossless event-envelope adapter for live AND normally closed Chromium NetLog."""
import json


def adapt(raw, destination):
    decoder=json.JSONDecoder()
    partial=[]
    count=0
    closed=False
    footer_lines=0
    with raw.open() as source:
        first=source.readline()
        constants,_=decoder.raw_decode(first[first.index(':')+1:])
        (destination/'constants.json').write_text(json.dumps(dict(constants=constants))+',\n"events": [\n')
        with (destination/'event_file_0.json').open('w') as target:
            for number,line in enumerate(source,2):
                text=line.strip()
                if closed:
                    footer_lines+=1
                    continue  # Raw polledData/footer bytes remain in the hashed source copy.
                if text in ('','"events": ['):
                    continue
                if text in (']','],',']}'):
                    closed=True
                    continue
                if not line.endswith('\n'):
                    partial.append(dict(path=str(raw),line=number,deferred_bytes=len(line.encode()),kind='netlog-tail'))
                    break
                event,end=decoder.raw_decode(text)
                if not isinstance(event,dict) or 'source' not in event or 'time' not in event:
                    raise ValueError(f'Unexpected event at {raw}:{number}')
                suffix=text[end:].strip()
                if suffix not in ('',',','],',']}'):
                    raise ValueError(f'Unexpected event envelope at {raw}:{number}: {suffix!r}')
                target.write(json.dumps(event,separators=(',',':'))+',\n')
                count+=1
                closed=suffix in ('],',']}')
    return dict(complete_events=count,partial_tails=partial,events_array_closed=closed,footer_lines=footer_lines)
