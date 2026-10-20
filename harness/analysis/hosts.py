# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""DNS record owners and contacted hosts are different protocol concepts."""


def label(host, record_types=()):
    host = host.lower().rstrip('.')
    if host in ('nomad.home', 'nomad'):
        return 'kiwix'
    # RFC9460 non-default-port HTTPS service binding for the one Kiwix input.
    # This is not a wildcard subdomain exclusion or a contacted HTTP host.
    if host == '_8090._https.nomad.home' and set(record_types) == {'HTTPS'}:
        return 'kiwix'
    if host == 'example.com':
        return 'canary'
    return 'outside'
