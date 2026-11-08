# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Scenario-owned creation plans checked against native callback identities."""


def make_scenario(drive, phases):
    assert isinstance(drive, str)
    plan = [dict(command=command, count=count) for command, count in phases]
    assert plan and len({step['command'] for step in plan}) == len(plan)
    assert all(isinstance(step['command'], str) and step['command'] and
               type(step['count']) is int and step['count'] > 0 for step in plan)
    return dict(drive=drive, browser_lifecycle=plan)


def inspect_browser_lifecycle(events, plan):
    """Never infer an expected count from the observed callback count."""
    make_scenario('', [(step['command'], step['count']) for step in plan])
    phases, created, closed, requests = [], [], [], set()
    current = None

    def complete_phase():
        if current is not None:
            expected = plan[len(phases) - 1]['count']
            assert len(current['browsers']) == expected, ('native creation phase', current, expected)

    for event in events:
        kind = event['event']
        token = event.get('token', '')
        creation = kind == 'navigation' and (token in (
            'start-stateless', 'memory-peer', 'memory-peer-stateless', 'memory-restart')
            or token.startswith('open-tab-'))
        if creation:
            complete_phase()
            assert len(phases) < len(plan), ('unexpected native creation command', token)
            wanted = plan[len(phases)]['command']
            assert (token.startswith(wanted) if wanted.endswith('-') else token == wanted), (
                'native creation phase order', token, wanted)
            request = event['request_id']
            assert request and request not in requests, ('duplicate native creation request', request)
            requests.add(request)
            current = dict(command=token, request_id=request, browsers=[])
            phases.append(current)
        elif kind == 'created':
            browser = event['browser']
            assert current is not None and type(browser) is int and browser > 0
            assert browser not in created, ('duplicate native browser creation', browser)
            created.append(browser)
            current['browsers'].append(browser)
            assert len(current['browsers']) <= plan[len(phases) - 1]['count'], current
        elif kind == 'closed':
            browser = event['browser']
            assert browser in created and browser not in closed, ('invalid native close', browser)
            closed.append(browser)
    complete_phase()
    assert len(phases) == len(plan), ('missing native creation phase', phases, plan)
    assert len(created) == len(closed) == sum(step['count'] for step in plan)
    assert set(created) == set(closed), ('unclosed native browser identities', created, closed)
    return dict(plan=plan, phases=phases, created=created, closed=closed)
